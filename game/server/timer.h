// WindowTimer: delivers the delayed "action window timeout" message.
//
// PROTOCOL.md §0.5 forbids cancellation of actor messages, so a timeout is a message that
// carries the window's |seq|; when it arrives after the window already moved on, the
// RoomActor drops it and changes nothing (seq invalidation). Timers are therefore never
// cancelled either — a fired timer whose window is gone is simply ignored.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mahjong {

class WindowTimer {
 public:
  using Action = std::function<void()>;

  WindowTimer();
  WindowTimer(const WindowTimer&) = delete;
  WindowTimer& operator=(const WindowTimer&) = delete;
  ~WindowTimer();

  // Stops the worker thread; idempotent. Must be called before the actor world is torn
  // down, because pending actions post messages into actors.
  void Stop();

  // Runs |action| once, on the timer thread, no earlier than |delay_ms| from now.
  void Schedule(int64_t delay_ms, Action action);

  size_t pending_count() const;

 private:
  struct Entry {
    int64_t deadline_ms = 0;
    uint64_t order = 0;
    Action action;
  };

  void Run();
  static bool LaterFirst(const Entry& lhs, const Entry& rhs);

  mutable std::mutex mutex_;
  std::condition_variable wakeup_;
  std::vector<Entry> queue_;  // min-heap by (deadline_ms, order)
  uint64_t next_order_ = 0;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace mahjong
