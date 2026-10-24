// AsyncGate: a coroutine-aware mutex held for the whole of one actor message handler.
//
// Why this exists (PROTOCOL.md §0.5 / GAME_DESIGN §0.3): ex_actor hands the actor its next
// message the moment the current handler suspends — `PullMailboxAndRun` is
// `while (msg = mailbox_.TryPop()) msg->Execute();`, and a coroutine handler returns control to
// that loop at its first suspension point. So as soon as a handler `co_await`s anything, the
// following message starts, and the two run concurrently against the same actor state.
//
// Acquiring this gate as the first statement of the actor's *single* entry point restores
// "one handler at a time" for that actor. The gate is not a std::mutex (that would block the
// scheduler thread): a waiter parks its coroutine and is resumed by the releaser.
//
// Three properties the design depends on:
//   * FIFO — the lock is handed to the oldest waiter, so handler order stays mailbox order;
//   * RAII — Guard releases in its destructor, so a handler that throws still unlocks;
//   * no lost wakeups — `await_ready` deliberately always returns false and `await_suspend`
//     re-checks under the lock; checking outside the lock would leave a window where the owner
//     releases between the check and the park, and the parked waiter would never be woken.
//
// It is the caller's job to make the entry the *only* way in: keep the real handler private so
// `SendLocal<&Actor::Handle>` cannot compile and the gate cannot be bypassed.
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
#include <mutex>
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

  // Diagnostics for tests and for the "handlers must not overlap" assertion.
  bool held() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return held_;
  }
  size_t waiting_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
  }

 private:
  bool AcquireOrPark(std::coroutine_handle<> handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!held_) {
      held_ = true;
      return false;
    }
    waiters_.push_back(handle);
    return true;
  }

  void Release() {
    std::coroutine_handle<> next;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      assert(held_);
      if (waiters_.empty()) {
        held_ = false;
        return;
      }
      // Hand the lock straight to the oldest waiter: `held_` stays true, so the handoff has no
      // window in which a new arrival could overtake the queue.
      next = waiters_.front();
      waiters_.pop_front();
    }
    next.resume();
  }

  mutable std::mutex mutex_;
  std::deque<std::coroutine_handle<>> waiters_;  // FIFO
  bool held_ = false;
};

}  // namespace mahjong
