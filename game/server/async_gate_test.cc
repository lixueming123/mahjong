// Unit tests for AsyncGate: the coroutine mutex that gives an actor "one handler at a time"
// again once its handlers are allowed to `co_await` (GAME_DESIGN §0.3, PROTOCOL §0.5).
//
// The load-bearing assertion is the overlap detector's. Every handler bumps `inside` on the way
// in and drops it on the way out, so `max_inside > 1` means two handlers were in the critical
// section at once — no matter what the gate believes about itself. The detector is the pattern
// the game's actors will carry: it catches a handler that forgot to take the gate.
//
// Two more tests pin the execution model the gate sits on. The handlers of one actor do *not*
// share a thread — GateIsTouchedFromDifferentThreadsByHandlersOfOneActor records the gate being
// touched from two different workers — but two messages of one actor never execute at the same
// moment: MessagesOfOneActorNeverExecuteSimultaneously shows a suspended handler's continuation
// queueing behind the activation that is already running. So the actor model, not the gate,
// supplies mutual exclusion; the gate's `std::mutex` is defensive (it would matter only if a
// `Guard` were released outside an actor handler) rather than load-bearing.
//
// The turnstile makes the test deterministic: a handler parks *inside* the critical section
// until the test releases that exact turn, so "who is inside right now" is not a race.
//
// Note these fixtures send messages with `exec::start_detached`, NOT through the process-wide
// `ActorSendScope()` the game drains before shutdown: tests here deliberately leave a handler
// parked forever (ParkedWaiterDoesNotHoldThePoolThread never releases its turnstile), so waiting
// for the scope to drain would deadlock. Shutdown ordering is exercised in mahjong_lobby_test
// and mahjong_room_test instead.

#include "async_gate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <exec/start_detached.hpp>
#include <gtest/gtest.h>

#include "ex_actor/api.h"

#include "actor_scope.h"

namespace ex = stdexec;

namespace mahjong {
namespace {

// ---------------------------------------------------------------------------------------
// Scaffolding
// ---------------------------------------------------------------------------------------

// Lets the test release parked handlers one turn at a time, in order.
class Turnstile {
 public:
  class Awaiter {
   public:
    Awaiter(Turnstile& turnstile, int turn) : turnstile_(&turnstile), turn_(turn) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle) { return turnstile_->ParkOrPass(turn_, handle); }
    void await_resume() const noexcept {}

   private:
    Turnstile* turnstile_;
    int turn_;
  };

  [[nodiscard]] Awaiter WaitFor(int turn) { return Awaiter(*this, turn); }

  // Lets through every waiter whose turn has come. The test calls this one turn at a time, so
  // exactly one handler resumes per call.
  void PassThrough(int turn) {
    std::vector<std::coroutine_handle<>> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      passed_through_ = turn;
      for (auto it = waiters_.begin(); it != waiters_.end();) {
        if (it->first <= turn) {
          ready.push_back(it->second);
          it = waiters_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (std::coroutine_handle<> handle : ready) {
      handle.resume();
    }
  }

 private:
  // Same shape as AsyncGate::AcquireAwaiter: check under the lock, so a pass arriving between
  // await_ready and await_suspend cannot be missed.
  bool ParkOrPass(int turn, std::coroutine_handle<> handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (turn <= passed_through_) {
      return false;
    }
    waiters_.emplace_back(turn, handle);
    return true;
  }

  std::mutex mutex_;
  std::vector<std::pair<int, std::coroutine_handle<>>> waiters_;
  int passed_through_ = 0;  // turns <= this have been let through
};

// Shared with the test thread. Everything cross-thread goes through the atomics/mutex here.
struct OverlapDetector {
  std::atomic<int> arrived {0};     // reached the door (before the gate)
  std::atomic<int> inside {0};      // inside the critical section
  std::atomic<int> max_inside {0};  // the overlap assertion: must stay 1
  std::atomic<int> finished {0};

  // Threads that touched the gate, for GateIsTouchedFromDifferentThreads.... The gate has no
  // single owning thread, so these can differ even for one actor; the test pins that down.
  std::atomic<std::thread::id> second_arrival_thread {};  // where handler #2 reached the gate
  std::atomic<std::thread::id> release_thread {};         // where handler #1 finished (and will unlock)
  std::mutex order_mutex;
  std::vector<int> entry_order;
  std::vector<int> exit_order;
};

// Atomic max, so a concurrent observer can never overwrite a larger value with a smaller one
// (which is exactly how an overlap could hide from the assertion).
void RecordMaxInside(OverlapDetector& detector, int candidate) {
  int observed = detector.max_inside.load(std::memory_order_relaxed);
  while (observed < candidate &&
         !detector.max_inside.compare_exchange_weak(observed, candidate, std::memory_order_acq_rel)) {
  }
}

// The actor side of the "single door" pattern: the framework can only reach `Entries` (the
// base's member, inherited), and the real handler stays private so `Send<&Actor::Handle>`
// cannot compile. `OnArrived` is the hook the tests use to observe messages that are at the
// door but not through it; it must not touch actor state.
template <class Impl, class... Args>
class SerializedEntry {
 public:
  stdexec::task<void> Entries(Args... args) {
    static_cast<Impl*>(this)->OnArrived();
    AsyncGate::Guard guard = co_await gate_.Acquire();
    co_await static_cast<Impl*>(this)->Handle(std::move(args)...);
  }

 protected:
  AsyncGate gate_;
};

class SerializedActor : public SerializedEntry<SerializedActor, int> {
 public:
  SerializedActor(std::shared_ptr<OverlapDetector> detector, std::shared_ptr<Turnstile> turnstile)
      : detector_(std::move(detector)), turnstile_(std::move(turnstile)) {}

 private:
  // Runs on the thread that executes the message, i.e. the thread that is about to call
  // `AcquireOrPark` — synchronously, since acquiring runs inline up to its suspension point.
  void OnArrived() {
    const int index = detector_->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (index == 2) {
      detector_->second_arrival_thread.store(std::this_thread::get_id(), std::memory_order_release);
    }
  }

  stdexec::task<void> Handle(int turn) {
    const int inside = detector_->inside.fetch_add(1, std::memory_order_acq_rel) + 1;
    RecordMaxInside(*detector_, inside);
    {
      std::lock_guard<std::mutex> lock(detector_->order_mutex);
      detector_->entry_order.push_back(turn);
    }
    if (turnstile_ != nullptr) {
      co_await turnstile_->WaitFor(turn);  // suspend *while holding the gate*
    }
    if (turn == 1) {
      // Nested `co_await` on a task completes by symmetric transfer, so this statement, the
      // return from `Entries`, and the guard's destructor all run on this same thread: this is
      // the thread that will call `AsyncGate::Release`.
      detector_->release_thread.store(std::this_thread::get_id(), std::memory_order_release);
    }
    {
      std::lock_guard<std::mutex> lock(detector_->order_mutex);
      detector_->exit_order.push_back(turn);
    }
    detector_->inside.fetch_sub(1, std::memory_order_acq_rel);
    detector_->finished.fetch_add(1, std::memory_order_acq_rel);
  }

  friend class SerializedEntry<SerializedActor, int>;

  std::shared_ptr<OverlapDetector> detector_;
  std::shared_ptr<Turnstile> turnstile_;
};

// Same door, but the handler throws: the guard must release while the coroutine unwinds.
class ThrowingActor : public SerializedEntry<ThrowingActor, int> {
 public:
  explicit ThrowingActor(std::shared_ptr<OverlapDetector> detector) : detector_(std::move(detector)) {}

 private:
  void OnArrived() { detector_->arrived.fetch_add(1, std::memory_order_acq_rel); }

  stdexec::task<void> Handle(int id) {
    const int inside = detector_->inside.fetch_add(1, std::memory_order_acq_rel) + 1;
    RecordMaxInside(*detector_, inside);
    detector_->inside.fetch_sub(1, std::memory_order_acq_rel);
    if (id == 2) {
      throw std::runtime_error("boom from handler 2");
    }
    detector_->finished.fetch_add(1, std::memory_order_acq_rel);
    co_return;
  }

  friend class SerializedEntry<ThrowingActor, int>;

  std::shared_ptr<OverlapDetector> detector_;
};

// A second actor with no gate and no suspension: it only proves that the pool still runs it.
class CounterActor {
 public:
  explicit CounterActor(std::shared_ptr<std::atomic<int>> count) : count_(std::move(count)) {}

  void Bump() { count_->fetch_add(1, std::memory_order_acq_rel); }

 private:
  std::shared_ptr<std::atomic<int>> count_;
};

// Occupies one pool worker for as long as the test wants: a plain (non-suspending) handler that
// spins on a flag. Used to decide *which* worker is free when a parked handler is resumed, so
// the two handlers of GateIsTouchedFromDifferentThreads... are forced onto different threads.
// Blocking a worker is only acceptable here because the test releases it deterministically;
// an actor in the game never blocks (GAME_DESIGN §0.3).
struct BlockerControl {
  std::atomic<bool> release {false};
  std::atomic<bool> started {false};
  std::atomic<bool> finished {false};
  std::atomic<std::thread::id> thread {};  // the worker this control occupies
};

class BlockerActor {
 public:
  void Block(std::shared_ptr<BlockerControl> control) {
    control->thread.store(std::this_thread::get_id(), std::memory_order_release);
    control->started.store(true, std::memory_order_release);
    while (!control->release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    control->finished.store(true, std::memory_order_release);
  }
};

// Releases both controls however the test leaves its coroutine, so a failed expectation cannot
// leave a worker spinning until the ctest timeout.
struct BlockersReleaseOnExit {
  ~BlockersReleaseOnExit() {
    first->release.store(true, std::memory_order_release);
    second->release.store(true, std::memory_order_release);
  }
  BlockerControl* first;
  BlockerControl* second;
};

bool WaitUntil(const std::function<bool()>& predicate, int timeout_ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

// ASSERT_* cannot be used inside a coroutine (it expands to a bare `return`), so waits inside
// the test coroutines go through this.
bool WaitForOrFail(const std::function<bool()>& predicate, const char* what) {
  if (WaitUntil(predicate)) {
    return true;
  }
  ADD_FAILURE() << "timed out waiting for " << what;
  return false;
}

size_t OrderSize(OverlapDetector& detector) {
  std::lock_guard<std::mutex> lock(detector.order_mutex);
  return detector.entry_order.size();
}

std::vector<int> OrderCopy(OverlapDetector& detector) {
  std::lock_guard<std::mutex> lock(detector.order_mutex);
  return detector.entry_order;
}

// ---------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------

TEST(AsyncGateTest, HandlersDoNotOverlapAndEntriesStayInOrder) {
  auto coroutine = []() -> stdexec::task<void> {
    constexpr int kMessages = 5;
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();
    auto actor = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile);

    for (int turn = 1; turn <= kMessages; ++turn) {
      exec::start_detached(actor.Send<&SerializedActor::Entries>(turn));
    }

    // Every message reached the door, but exactly one got through it: the rest are parked on
    // the gate while the first one sits inside the critical section. The wait is on the entry,
    // not on the arrival — a handler that has arrived may not have taken the gate yet.
    if (!WaitForOrFail([&] { return detector->arrived.load() == kMessages && OrderSize(*detector) == 1; },
                       "all 5 messages at the door and the first one inside")) {
      co_return;
    }
    EXPECT_EQ(detector->inside.load(), 1);
    EXPECT_EQ(detector->max_inside.load(), 1);

    // Releasing a handler lets exactly one more in, and it is the next in mailbox order. The
    // last release is deliberately outside the loop: it finishes the queue, so `inside`
    // legitimately drops back to 0 rather than staying at 1.
    for (int turn = 1; turn < kMessages; ++turn) {
      turnstile->PassThrough(turn);
      if (!WaitForOrFail([&] { return OrderSize(*detector) == static_cast<size_t>(turn) + 1; },
                         "the next handler to enter")) {
        co_return;
      }
      EXPECT_EQ(detector->inside.load(), 1) << "overlap after releasing turn " << turn;
      EXPECT_EQ(detector->max_inside.load(), 1);
    }
    turnstile->PassThrough(kMessages);

    if (!WaitForOrFail([&] { return detector->finished.load() == kMessages; }, "all handlers to finish")) {
      co_return;
    }
    EXPECT_EQ(detector->inside.load(), 0);
    EXPECT_EQ(detector->max_inside.load(), 1);
    EXPECT_EQ(OrderCopy(*detector), (std::vector<int> {1, 2, 3, 4, 5}));
    {
      std::lock_guard<std::mutex> lock(detector->order_mutex);
      EXPECT_EQ(detector->exit_order, (std::vector<int> {1, 2, 3, 4, 5}));
    }
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown();
}

// A burst big enough that handlers queue up on the gate: every handler is parked inside the
// critical section at the same moment, so letting them all through at once forces 200 handoffs
// in a row — covering lost wakeups (a waiter parked after the releaser had already looked) and
// the inline-handoff chain, and catching any overlap the gate lets through under that pressure.
TEST(AsyncGateTest, BurstStaysSerializedAndLosesNoWakeups) {
  auto coroutine = []() -> stdexec::task<void> {
    constexpr int kMessages = 200;
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();
    auto actor = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile);

    for (int turn = 1; turn <= kMessages; ++turn) {
      exec::start_detached(actor.Send<&SerializedActor::Entries>(turn));
    }
    if (!WaitForOrFail([&] { return detector->arrived.load() == kMessages && OrderSize(*detector) == 1; },
                       "all 200 messages at the door and the first one inside")) {
      co_return;
    }
    // One handler is inside the critical section; the other 199 are parked on the gate.
    EXPECT_EQ(detector->inside.load(), 1);

    turnstile->PassThrough(kMessages);
    if (!WaitForOrFail([&] { return detector->finished.load() == kMessages; }, "all 200 handlers to finish")) {
      co_return;
    }

    std::vector<int> order = OrderCopy(*detector);
    EXPECT_EQ(order.size(), static_cast<size_t>(kMessages));
    EXPECT_TRUE(std::is_sorted(order.begin(), order.end())) << "mailbox order was not preserved";
    EXPECT_EQ(detector->inside.load(), 0);
    EXPECT_EQ(detector->max_inside.load(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown();
}

// The gate must not cost a thread while it waits: a handler parked on it suspends its
// coroutine and gives the pool thread back. Run on a *single-threaded* pool, so if a parked
// waiter occupied the thread, nothing else could ever run — this test would time out instead
// of passing.
TEST(AsyncGateTest, ParkedWaiterDoesNotHoldThePoolThread) {
  auto coroutine = []() -> stdexec::task<void> {
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();  // never released: the handler stays parked
    auto count = std::make_shared<std::atomic<int>>(0);
    auto serialized = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile);
    auto other = co_await ex_actor::Spawn<CounterActor>(count);

    exec::start_detached(serialized.Send<&SerializedActor::Entries>(1));
    if (!WaitForOrFail([&] { return detector->inside.load() == 1; }, "the handler to park inside the gate")) {
      co_return;
    }

    exec::start_detached(other.Send<&CounterActor::Bump>());
    if (!WaitForOrFail([&] { return count->load() == 1; }, "the other actor to run while the gate is held")) {
      co_return;
    }

    // The gate is still held by the parked handler: the second actor ran *beside* it, not
    // because it finished.
    EXPECT_EQ(detector->finished.load(), 0);
    EXPECT_EQ(detector->inside.load(), 1);

    // Now the other kind of waiting: a handler parked *on* the gate (not inside it). It must
    // give up the single pool thread too.
    exec::start_detached(serialized.Send<&SerializedActor::Entries>(2));
    if (!WaitForOrFail([&] { return detector->arrived.load() == 2; }, "the second handler to park on the gate")) {
      co_return;
    }
    exec::start_detached(other.Send<&CounterActor::Bump>());
    if (!WaitForOrFail([&] { return count->load() == 2; }, "the other actor to run while a waiter is parked on the gate")) {
      co_return;
    }
    EXPECT_EQ(detector->inside.load(), 1) << "the second handler entered while the first held the gate";
    EXPECT_EQ(detector->max_inside.load(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/1);
  ex::sync_wait(coroutine());
  stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown();
}

// The gate is not touched by one thread: the actor's activations run on whatever worker the
// pool hands them, and a resumed handler's continuation is a fresh activation, so nothing ties
// it to the worker that ran the handler before it. Thread identity is therefore not a property
// the gate may rely on — but this is *not* concurrency; the touches never overlap (see
// MessagesOfOneActorNeverExecuteSimultaneously), because the actor model itself serializes
// activations. Mutex or not, no caller-side locking is needed for this.
//
// Determinism on a 2-worker pool:
//   * handler #1 takes the gate and parks inside the critical section;
//   * handler #2 arrives, parks on the gate, and records the worker it arrived on;
//   * two blockers (one spinning handler each, on two different actors) occupy both workers,
//     which also proves the two handlers are really suspended — their activations have returned;
//   * the test frees only the worker that is *not* handler #2's, then lets #1 continue. With a
//     single worker free, #1's continuation must run there, so the gate's `Release` and #2's
//     `AcquireOrPark` provably happen on two different threads.
TEST(AsyncGateTest, GateIsTouchedFromDifferentThreadsByHandlersOfOneActor) {
  auto coroutine = []() -> stdexec::task<void> {
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();
    auto actor = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile);
    // One actor per blocker: a blocking handler never returns, so a second message to the same
    // actor would sit in its mailbox behind the first.
    auto first_blocker = co_await ex_actor::Spawn<BlockerActor>();
    auto second_blocker = co_await ex_actor::Spawn<BlockerActor>();

    exec::start_detached(actor.Send<&SerializedActor::Entries>(1));
    if (!WaitForOrFail([&] { return detector->inside.load() == 1; }, "the first handler to hold the gate")) {
      co_return;
    }
    // Parks on the gate, recording the worker that ran its activation.
    exec::start_detached(actor.Send<&SerializedActor::Entries>(2));
    if (!WaitForOrFail([&] { return detector->arrived.load() == 2; }, "the second handler to reach the gate")) {
      co_return;
    }

    auto first_control = std::make_shared<BlockerControl>();
    auto second_control = std::make_shared<BlockerControl>();
    BlockersReleaseOnExit release_on_exit {first_control.get(), second_control.get()};
    // Deliberately SendLocal: a BlockerControl is atomics and shared memory, i.e. process-local
    // test scaffolding that could never be serialized. Every game message uses Send.
    exec::start_detached(first_blocker.SendLocal<&BlockerActor::Block>(first_control));
    exec::start_detached(second_blocker.SendLocal<&BlockerActor::Block>(second_control));
    if (!WaitForOrFail([&] { return first_control->started.load() && second_control->started.load(); },
                       "both pool workers to be occupied")) {
      co_return;
    }

    const std::thread::id acquire_thread = detector->second_arrival_thread.load(std::memory_order_acquire);
    const std::thread::id first_worker = first_control->thread.load(std::memory_order_acquire);
    const std::thread::id second_worker = second_control->thread.load(std::memory_order_acquire);
    EXPECT_TRUE(first_worker == acquire_thread || second_worker == acquire_thread)
        << "handler #2 did not run on one of the pool workers";

    BlockerControl* freed = (first_worker == acquire_thread) ? second_control.get() : first_control.get();
    // Free exactly one worker: #1's continuation is scheduled back onto the actor's scheduler,
    // and only this worker is in a position to run it.
    freed->release.store(true, std::memory_order_release);

    turnstile->PassThrough(1);
    if (!WaitForOrFail([&] { return OrderSize(*detector) == 2; }, "the second handler to enter after the handoff")) {
      co_return;
    }
    const std::thread::id release_thread = detector->release_thread.load(std::memory_order_acquire);

    EXPECT_EQ(release_thread, freed->thread.load()) << "handler #1 did not resume on the only free worker";
    EXPECT_NE(release_thread, acquire_thread)
        << "both handlers touched the gate from the same worker; thread identity is not stable enough "
           "for the gate to assume one thread";

    turnstile->PassThrough(2);
    if (!WaitForOrFail([&] { return detector->finished.load() == 2; }, "both handlers to finish")) {
      co_return;
    }
    // The other worker is still spinning on its flag; let it go before the world shuts down.
    for (BlockerControl* control : {first_control.get(), second_control.get()}) {
      control->release.store(true, std::memory_order_release);
    }
    if (!WaitForOrFail([&] { return first_control->finished.load() && second_control->finished.load(); },
                       "both blockers to finish")) {
      co_return;
    }
    EXPECT_EQ(detector->inside.load(), 0);
    EXPECT_EQ(detector->max_inside.load(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown();
}

TEST(AsyncGateTest, ExceptionInsideCriticalSectionStillReleasesTheGate) {
  auto coroutine = []() -> stdexec::task<void> {
    auto detector = std::make_shared<OverlapDetector>();
    auto actor = co_await ex_actor::Spawn<ThrowingActor>(detector);

    co_await actor.Send<&ThrowingActor::Entries>(1);
    EXPECT_EQ(detector->finished.load(), 1);

    // The failure must reach the caller...
    bool threw = false;
    try {
      co_await actor.Send<&ThrowingActor::Entries>(2);
    } catch (const std::exception& error) {
      threw = true;
      EXPECT_NE(std::string(error.what()).find("boom from handler 2"), std::string::npos);
    }
    EXPECT_TRUE(threw) << "the handler's exception did not reach the caller";

    // ...and the gate must have been released while that coroutine unwound. If it were not,
    // #3 would park on a gate nobody ever unlocks and this test would hang until the ctest
    // timeout rather than fail an expectation.
    co_await actor.Send<&ThrowingActor::Entries>(3);
    EXPECT_EQ(detector->finished.load(), 2);
    EXPECT_EQ(detector->max_inside.load(), 1);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  stdexec::sync_wait(ActorSendScope().on_empty());
    ex_actor::Shutdown();
}

// The gate itself, with no actor runtime involved: a moved-from Guard must not release a second
// time, and a normal acquisition must not. `AsyncGate` depends on nothing but <coroutine>.
TEST(AsyncGateTest, GuardMoveReleasesExactlyOnce) {
  auto coroutine = []() -> stdexec::task<void> {
    AsyncGate gate;
    EXPECT_FALSE(gate.held());
    {
      auto guard = co_await gate.Acquire();
      EXPECT_TRUE(gate.held());
      auto moved = std::move(guard);
      // `guard` is now empty: its destructor must not unlock on behalf of `moved`.
    }
    EXPECT_FALSE(gate.held()) << "the lock was released twice or not at all";
    {
      auto guard = co_await gate.Acquire();  // proves the lock is actually free again
      EXPECT_TRUE(gate.held());
    }
    EXPECT_FALSE(gate.held());
    EXPECT_EQ(gate.waiting_count(), 0u);
  };
  ex::sync_wait(coroutine());
}


// The alternative to the gate's mutex: pin the actor to a thread, so every touch of the gate
// (and of the actor's state) comes from one thread. `CorePinnedThreadPool` dispatches by
// `ActorConfig::core_index`, and the pinning survives a suspension: `std::execution::task`'s
// scheduler affinity brings the continuation back to the actor's scheduler, which is the pinned
// one. Handler #2's activation and handler #1's post-suspension code therefore share a thread
// (both run on core 0's queue here), which is *not* the case on the default work-sharing pool
// (see GateIsTouchedFromDifferentThreadsByHandlersOfOneActor).
//
// What this does not buy: serialization. Handler #1 is still suspended while #2 runs on the same
// thread, so a handler's state can still change across a suspension.
TEST(AsyncGateTest, PinnedActorKeepsHandlerAndContinuationOnOneThread) {
  auto coroutine = []() -> stdexec::task<void> {
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();
    auto actor = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile).WithConfig({.core_index = 0});

    exec::start_detached(actor.Send<&SerializedActor::Entries>(1));
    if (!WaitForOrFail([&] { return detector->inside.load() == 1; }, "first handler inside")) { co_return; }
    exec::start_detached(actor.Send<&SerializedActor::Entries>(2));
    if (!WaitForOrFail([&] { return detector->arrived.load() == 2; }, "second handler arrived")) { co_return; }

    turnstile->PassThrough(1);
    if (!WaitForOrFail([&] { return OrderSize(*detector) == 2; }, "second handler entered")) { co_return; }
    const std::thread::id release_thread = detector->release_thread.load();
    const std::thread::id acquire_thread = detector->second_arrival_thread.load();
    EXPECT_EQ(release_thread, acquire_thread) << "continuation left the pinned thread";

    turnstile->PassThrough(2);
    if (!WaitForOrFail([&] { return detector->finished.load() == 2; }, "both finished")) { co_return; }
  };
  ex_actor::CorePinnedThreadPool pool(/*thread_count=*/2);
  ex_actor::Init(pool.GetScheduler());
  ex::sync_wait(coroutine());
  stdexec::sync_wait(ActorSendScope().on_empty());
  ex_actor::Shutdown();
}

// Shared with the test thread; everything cross-thread is atomic.
struct ExecutionProbe {
  std::atomic<int> inside {0};
  std::atomic<int> max_inside {0};
  std::atomic<int> finished {0};
  std::atomic<std::thread::id> fast_thread {};
  std::atomic<std::thread::id> slow_thread_after {};
  std::atomic<bool> slow_at_door {false};
  std::atomic<bool> fast_running {false};
  std::atomic<bool> slow_resumed {false};
  std::atomic<bool> stop_spin {false};
};

void RecordMax(std::atomic<int>& target, int candidate) {
  int observed = target.load(std::memory_order_relaxed);
  while (observed < candidate &&
         !target.compare_exchange_weak(observed, candidate, std::memory_order_acq_rel)) {
  }
}

// `Slow` suspends once. Both handlers then stay inside their section until the test says stop, so
// "were two of them inside at the same time" is observable from `max_inside`.
class ProbeActor {
 public:
  ProbeActor(std::shared_ptr<ExecutionProbe> probe, std::shared_ptr<Turnstile> turnstile)
      : probe_(std::move(probe)), turnstile_(std::move(turnstile)) {}

  stdexec::task<void> Slow() {
    probe_->slow_at_door.store(true, std::memory_order_release);
    co_await turnstile_->WaitFor(1);  // the only suspension
    const int inside = probe_->inside.fetch_add(1, std::memory_order_acq_rel) + 1;
    RecordMax(probe_->max_inside, inside);
    probe_->slow_thread_after.store(std::this_thread::get_id(), std::memory_order_release);
    probe_->slow_resumed.store(true, std::memory_order_release);
    SpinUntilStop();
    probe_->inside.fetch_sub(1, std::memory_order_acq_rel);
    probe_->finished.fetch_add(1, std::memory_order_acq_rel);
    co_return;
  }

  void Fast() {
    probe_->fast_thread.store(std::this_thread::get_id(), std::memory_order_release);
    const int inside = probe_->inside.fetch_add(1, std::memory_order_acq_rel) + 1;
    RecordMax(probe_->max_inside, inside);
    probe_->fast_running.store(true, std::memory_order_release);
    SpinUntilStop();
    probe_->inside.fetch_sub(1, std::memory_order_acq_rel);
    probe_->finished.fetch_add(1, std::memory_order_acq_rel);
  }

 private:
  void SpinUntilStop() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!probe_->stop_spin.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
  }

  std::shared_ptr<ExecutionProbe> probe_;
  std::shared_ptr<Turnstile> turnstile_;
};

// Pins the execution model: the messages of one actor never execute at the same time, even on a
// multi-threaded pool with an idle worker. A suspended handler's continuation is not a
// free-floating pool task — `std::execution::task`'s scheduler affinity hands it back to the
// actor, i.e. it queues behind the activation that is currently running. (It still *interleaves*
// with that message at its suspension point, which is the atomicity gap AsyncGate closes; see the
// file header. Interleaving is not concurrency.)
//
// The probe: handler #1 suspends; handler #2 then runs on one worker while the other sits idle;
// releasing #1's turnstile must therefore not produce any execution until #2 returns. A
// pool-scheduled continuation would have run on the idle worker within microseconds.
TEST(AsyncGateTest, MessagesOfOneActorNeverExecuteSimultaneously) {
  auto coroutine = []() -> stdexec::task<void> {
    auto probe = std::make_shared<ExecutionProbe>();
    auto turnstile = std::make_shared<Turnstile>();
    auto actor = co_await ex_actor::Spawn<ProbeActor>(probe, turnstile);

    exec::start_detached(actor.Send<&ProbeActor::Slow>());
    if (!WaitForOrFail([&] { return probe->slow_at_door.load(); }, "the slow handler to reach its suspension")) {
      co_return;
    }
    // Let the activation return so the handler is genuinely suspended before it is released.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    exec::start_detached(actor.Send<&ProbeActor::Fast>());
    if (!WaitForOrFail([&] { return probe->fast_running.load(); }, "the second handler to run")) {
      co_return;
    }

    turnstile->PassThrough(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(probe->slow_resumed.load())
        << "a handler continuation ran beside the activation that was already running";
    EXPECT_EQ(probe->max_inside.load(), 1) << "two messages of one actor were executing at once";

    probe->stop_spin.store(true, std::memory_order_release);
    if (!WaitForOrFail([&] { return probe->slow_resumed.load(); }, "the continuation to run after the other message")) {
      co_return;
    }
    if (!WaitForOrFail([&] { return probe->finished.load() == 2; }, "both handlers to finish")) {
      co_return;
    }

    EXPECT_EQ(probe->max_inside.load(), 1);
    EXPECT_EQ(probe->slow_thread_after.load(), probe->fast_thread.load())
        << "the continuation did not resume on the worker that ran the previous message";
    EXPECT_EQ(probe->inside.load(), 0);
  };
  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  stdexec::sync_wait(ActorSendScope().on_empty());
  ex_actor::Shutdown();
}

}  // namespace
}  // namespace mahjong
