// Unit tests for AsyncGate: the coroutine mutex that gives an actor "one handler at a time"
// again once its handlers are allowed to `co_await` (GAME_DESIGN §0.3, PROTOCOL §0.5).
//
// The load-bearing assertion is the overlap detector's. Every handler bumps `inside` on the way
// in and drops it on the way out, so `max_inside > 1` means two handlers were in the critical
// section at once — no matter what the gate believes about itself. The detector is the pattern
// the game's actors will carry: it catches a handler that forgot to take the gate.
//
// The turnstile makes the test deterministic: a handler parks *inside* the critical section
// until the test releases that exact turn, so "who is inside right now" is not a race.
//
// Note these fixtures send messages with `ex::start_detached`, NOT through the process-wide
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

#include <gtest/gtest.h>

#include "ex_actor/api.h"

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
// base's member, inherited), and the real handler stays private so `SendLocal<&Actor::Handle>`
// cannot compile. `OnArrived` is the hook the tests use to observe messages that are at the
// door but not through it; it must not touch actor state.
template <class Impl, class... Args>
class SerializedEntry {
 public:
  exec::task<void> Entries(Args... args) {
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
  void OnArrived() { detector_->arrived.fetch_add(1, std::memory_order_acq_rel); }

  exec::task<void> Handle(int turn) {
    const int inside = detector_->inside.fetch_add(1, std::memory_order_acq_rel) + 1;
    RecordMaxInside(*detector_, inside);
    {
      std::lock_guard<std::mutex> lock(detector_->order_mutex);
      detector_->entry_order.push_back(turn);
    }
    if (turnstile_ != nullptr) {
      co_await turnstile_->WaitFor(turn);  // suspend *while holding the gate*
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

  exec::task<void> Handle(int id) {
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
  auto coroutine = []() -> exec::task<void> {
    constexpr int kMessages = 5;
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();
    auto actor = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile);

    for (int turn = 1; turn <= kMessages; ++turn) {
      ex::start_detached(actor.SendLocal<&SerializedActor::Entries>(turn));
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
  auto coroutine = []() -> exec::task<void> {
    constexpr int kMessages = 200;
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();
    auto actor = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile);

    for (int turn = 1; turn <= kMessages; ++turn) {
      ex::start_detached(actor.SendLocal<&SerializedActor::Entries>(turn));
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
  auto coroutine = []() -> exec::task<void> {
    auto detector = std::make_shared<OverlapDetector>();
    auto turnstile = std::make_shared<Turnstile>();  // never released: the handler stays parked
    auto count = std::make_shared<std::atomic<int>>(0);
    auto serialized = co_await ex_actor::Spawn<SerializedActor>(detector, turnstile);
    auto other = co_await ex_actor::Spawn<CounterActor>(count);

    ex::start_detached(serialized.SendLocal<&SerializedActor::Entries>(1));
    if (!WaitForOrFail([&] { return detector->inside.load() == 1; }, "the handler to park inside the gate")) {
      co_return;
    }

    ex::start_detached(other.SendLocal<&CounterActor::Bump>());
    if (!WaitForOrFail([&] { return count->load() == 1; }, "the other actor to run while the gate is held")) {
      co_return;
    }

    // The gate is still held by the parked handler: the second actor ran *beside* it, not
    // because it finished.
    EXPECT_EQ(detector->finished.load(), 0);
    EXPECT_EQ(detector->inside.load(), 1);

    // Now the other kind of waiting: a handler parked *on* the gate (not inside it). It must
    // give up the single pool thread too.
    ex::start_detached(serialized.SendLocal<&SerializedActor::Entries>(2));
    if (!WaitForOrFail([&] { return detector->arrived.load() == 2; }, "the second handler to park on the gate")) {
      co_return;
    }
    ex::start_detached(other.SendLocal<&CounterActor::Bump>());
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

TEST(AsyncGateTest, ExceptionInsideCriticalSectionStillReleasesTheGate) {
  auto coroutine = []() -> exec::task<void> {
    auto detector = std::make_shared<OverlapDetector>();
    auto actor = co_await ex_actor::Spawn<ThrowingActor>(detector);

    co_await actor.SendLocal<&ThrowingActor::Entries>(1);
    EXPECT_EQ(detector->finished.load(), 1);

    // The failure must reach the caller...
    bool threw = false;
    try {
      co_await actor.SendLocal<&ThrowingActor::Entries>(2);
    } catch (const std::exception& error) {
      threw = true;
      EXPECT_NE(std::string(error.what()).find("boom from handler 2"), std::string::npos);
    }
    EXPECT_TRUE(threw) << "the handler's exception did not reach the caller";

    // ...and the gate must have been released while that coroutine unwound. If it were not,
    // #3 would park on a gate nobody ever unlocks and this test would hang until the ctest
    // timeout rather than fail an expectation.
    co_await actor.SendLocal<&ThrowingActor::Entries>(3);
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
  auto coroutine = []() -> exec::task<void> {
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

}  // namespace
}  // namespace mahjong
