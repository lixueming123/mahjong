#include "timer.h"

#include <algorithm>
#include <utility>

#include "ex_actor/api.h"
#include "ex_actor/internal/platform.h"

namespace mahjong {

WindowTimer::WindowTimer() : worker_([this] { Run(); }) {}

WindowTimer::~WindowTimer() { Stop(); }

bool WindowTimer::LaterFirst(const Entry& lhs, const Entry& rhs) {
  if (lhs.deadline_ms != rhs.deadline_ms) {
    return lhs.deadline_ms > rhs.deadline_ms;  // std::*_heap builds a max-heap
  }
  return lhs.order > rhs.order;
}

void WindowTimer::Schedule(int64_t delay_ms, Action action) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    Entry entry;
    entry.deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count() +
                        delay_ms;
    entry.order = next_order_++;
    entry.action = std::move(action);
    queue_.push_back(std::move(entry));
    std::push_heap(queue_.begin(), queue_.end(), LaterFirst);
  }
  wakeup_.notify_one();
}

size_t WindowTimer::pending_count() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

void WindowTimer::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    queue_.clear();
  }
  wakeup_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void WindowTimer::Run() {
  ex_actor::internal::SetThreadName("win-timer");
  std::vector<Action> due;
  while (true) {
    {
      std::unique_lock lock(mutex_);
      while (!stopping_ && queue_.empty()) {
        wakeup_.wait(lock);
      }
      if (stopping_) {
        return;
      }
      int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
      if (queue_.front().deadline_ms > now) {
        wakeup_.wait_until(lock, std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(queue_.front().deadline_ms - now));
        continue;
      }
      // Collect every entry that is due; never hold the lock while running actions, because
      // an action posts into an actor mailbox.
      int64_t current = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
      while (!queue_.empty() && queue_.front().deadline_ms <= current) {
        std::pop_heap(queue_.begin(), queue_.end(), LaterFirst);
        Action action = std::move(queue_.back().action);
        queue_.pop_back();
        due.push_back(std::move(action));
      }
    }
    for (Action& action : due) {
      action();
    }
    due.clear();
  }
}

}  // namespace mahjong
