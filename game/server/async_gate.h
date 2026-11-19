// AsyncGate: a coroutine-aware mutex held for the whole of one actor message handler.
//
// Why: ex_actor hands the actor its next message the moment the current handler suspends, so as
// soon as a handler `co_await`s anything, the following message starts and *interleaves* with the
// suspended handler (not concurrently, but with no atomicity — the second handler runs against
// half-updated state).
//
// Taking this gate as the first statement of the actor's *single* entry point restores "one
// handler at a time". It is not a std::mutex: a waiter parks its coroutine instead of blocking the
// scheduler thread.
//
// Three properties the design depends on:
//   * FIFO — the lock is handed to the oldest waiter, so handler order stays mailbox order;
//   * RAII — Guard releases in its destructor, so a handler that throws still unlocks;
//   * no lost wakeups — `await_ready` always returns false, so every acquire goes through
//     `await_suspend`'s `held_` check and a waiter is parked exactly once.
//
// No mutex inside: every touch of `held_`/`waiters_` happens inside an actor message handler and
// ex-actor runs at most one activation per actor at a time. The guarantee is mutual exclusion, not
// a fixed thread (activations move between pool workers), so a Guard must not escape its handler.
//
// It is the caller's job to make the entry the *only* way in: keep the real handlers private so
// `Send<&Actor::Handle>` cannot compile.

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

  // Suspends until the lock is ours.
  class ParkAwaiter {
   public:
    explicit ParkAwaiter(AsyncGate& gate) : gate_(&gate) {}

    // Always false: every acquire goes through `await_suspend` (see the header comment).
    bool await_ready() const noexcept { return false; }
    // false => do not suspend, the lock is ours; true => parked, Release will resume us.
    bool await_suspend(std::coroutine_handle<> handle) { return gate_->AcquireOrPark(handle); }
    void await_resume() const noexcept {}

   private:
    AsyncGate* gate_;
  };

  // The lock, as a task the caller `co_await`s: `AsyncGate::Guard guard = co_await gate.Acquire();`
  //
  // Not a bare awaitable returning a value: stdexec transports such a value back through a
  // sender's `set_value(Guard&&)`, which crashed inside the actor dispatch path. Sending the Guard
  // as a task result uses the path every task-returning handler already uses. Cost: one coroutine
  // frame per acquire.
  [[nodiscard]] stdexec::task<Guard> Acquire() {
    co_await ParkAwaiter(*this);
    co_return Guard(this);
  }

  // Diagnostics for tests. Call them only while no handler of the owning actor can be running.
  bool held() const { return held_; }
  size_t waiting_count() const { return waiters_.size(); }

 private:
  // Called from `await_suspend`, i.e. inside a message handler. `return false` means the lock was
  // free and the caller must not suspend.
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
    // Hand the lock straight to the oldest waiter: `held_` stays true, so no arrival can overtake
    // the queue. The resume is inline, so consecutive waiters nest on the stack; a burst is bounded
    // by the frames arriving during one suspension, which is fine here.
    std::coroutine_handle<> next = waiters_.front();
    waiters_.pop_front();
    next.resume();
  }

  std::deque<std::coroutine_handle<>> waiters_;  // FIFO
  bool held_ = false;
};

}  // namespace mahjong
