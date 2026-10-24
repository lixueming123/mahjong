// AsyncGate: a coroutine-aware mutex held for the whole of one actor message handler.
//
// Why this exists (PROTOCOL.md §0.5 / GAME_DESIGN §0.3): ex_actor hands the actor its next
// message the moment the current handler suspends — `PullMailboxAndRun` is
// `while (msg = mailbox_.TryPop()) msg->Execute();`, and a coroutine handler returns control to
// that loop at its first suspension point. So as soon as a handler `co_await`s anything, the
// following message starts and *interleaves* with the suspended handler against the same actor
// state — not concurrently (activations stay mutually exclusive), but with no atomicity: the
// second handler runs while the first one's state is half-updated.
//
// Acquiring this gate as the first statement of the actor's *single* entry point restores
// "one handler at a time" for that actor. The gate is not a std::mutex (that would block the
// scheduler thread): a waiter parks its coroutine and is resumed by the releaser.
//
// Three properties the design depends on:
//   * FIFO — the lock is handed to the oldest waiter, so handler order stays mailbox order;
//   * RAII — Guard releases in its destructor, so a handler that throws still unlocks;
//   * no lost wakeups — `await_ready` deliberately always returns false, so every acquire goes
//     through `await_suspend`'s `held_` check; a waiter that loses is parked exactly once, and
//     the releaser that hands the lock over is the one that resumes it.
//
// No mutex inside, on purpose: every touch of `held_`/`waiters_` happens inside an actor message
// handler, and ex-actor runs at most one activation per actor at a time — a suspended handler's
// continuation is queued behind the running one, not run beside it (async_gate_test.cc,
// MessagesOfOneActorNeverExecuteSimultaneously). That is also why the check-and-park in
// `AcquireOrPark` cannot race a releaser. Note the guarantee is mutual exclusion, not a fixed
// thread: activations can move between pool workers, so an owning-thread assertion would be
// wrong as well (GateIsTouchedFromDifferentThreadsByHandlersOfOneActor). A Guard that escaped
// its handler and was released on another thread would break this invariant — and this class.
//
// It is the caller's job to make the entry the *only* way in: keep the real handler private so
// `Send<&Actor::Handle>` cannot compile and the gate cannot be bypassed.
//
// Resumption cost, accepted on purpose: `Release` resumes the next waiter inline, so that
// handler continues on the releasing thread and consecutive waiters nest on the call stack
// (A unlocks -> B runs inside A's stack -> B unlocks -> C inside B's, ...). All of our releases
// happen on a scheduler thread and a burst is bounded by how many frames arrive during one
// suspension, so this is fine here; if a queue of thousands ever becomes possible, post the
// resume back to a scheduler instead of calling `resume()`.

#pragma once

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <utility>

#include <exec/task.hpp>

namespace mahjong {

class AsyncGate {
 public:
  // The lock is held while a Guard is alive. Movable, not copyable.
  class Guard {
   public:
    Guard() = default;
    explicit Guard(AsyncGate* gate) : gate_(gate) {}

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}
    Guard& operator=(Guard&& other) noexcept {
      if (this != &other) {
        Reset();
        gate_ = std::exchange(other.gate_, nullptr);
      }
      return *this;
    }
    ~Guard() { Reset(); }

    // Releases now; idempotent, so a moved-from Guard releases nothing.
    void Reset() {
      if (gate_ != nullptr) {
        AsyncGate* gate = std::exchange(gate_, nullptr);
        gate->Release();
      }
    }

   private:
    AsyncGate* gate_ = nullptr;
  };

  // Suspends until the lock is ours. `await_resume` deliberately returns nothing: the result of
  // `Acquire()` is delivered as a task value instead (see below).
  class ParkAwaiter {
   public:
    explicit ParkAwaiter(AsyncGate& gate) : gate_(&gate) {}

    // Always false: every acquire goes through `await_suspend`, which re-checks under the lock
    // (see the header comment).
    bool await_ready() const noexcept { return false; }
    // false => do not suspend, the lock is ours; true => parked, Release will resume us.
    bool await_suspend(std::coroutine_handle<> handle) { return gate_->AcquireOrPark(handle); }
    void await_resume() const noexcept {}

   private:
    AsyncGate* gate_;
  };

  // The lock, as a lazily-started task the caller `co_await`s:
  //     AsyncGate::Guard guard = co_await gate.Acquire();
  //
  // Why a task and not a bare awaitable: an awaitable whose `await_resume()` returns a value has
  // that value transported by stdexec's scheduler-affinity machinery, which hands it back through
  // a sender's `set_value(Guard&&)` — and inside the actor dispatch path that crashed (LobbyActor
  // stopped at the first `co_await` of a guarded handler). Sending the Guard as a *task result*
  // uses the same path as any other task-returning handler in this process, and keeps the exotic
  // part down to a void awaitable. Cost: one coroutine frame per acquire.
  [[nodiscard]] stdexec::task<Guard> Acquire() {
    co_await ParkAwaiter(*this);
    co_return Guard(this);
  }

  // Diagnostics for tests. Call them only while no handler of the owning actor can be running
  // (the actor-level tests do); they are plain reads now, see the "no mutex inside" note above.
  bool held() const { return held_; }
  size_t waiting_count() const { return waiters_.size(); }

 private:
  // Called from `Awaitable::await_suspend`, i.e. inside a message handler. `return false` means
  // the lock was free and the caller must not suspend.
  bool AcquireOrPark(std::coroutine_handle<> handle) {
    if (!held_) {
      held_ = true;
      return false;
    }
    waiters_.push_back(handle);
    return true;
  }

  void Release() {
    assert(held_);
    if (waiters_.empty()) {
      held_ = false;
      return;
    }
    // Hand the lock straight to the oldest waiter: `held_` stays true, so the handoff has no
    // window in which a new arrival could overtake the queue.
    std::coroutine_handle<> next = waiters_.front();
    waiters_.pop_front();
    next.resume();
  }

  std::deque<std::coroutine_handle<>> waiters_;  // FIFO
  bool held_ = false;
};

}  // namespace mahjong
