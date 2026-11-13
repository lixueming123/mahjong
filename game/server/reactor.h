// Level-triggered socket reactor for the gateway thread.
//
// Linux waits with epoll, macOS with kqueue. Both are level-triggered (no EPOLLET, no EV_CLEAR),
// so an fd keeps being reported while it is readable or writable. Events are normalized to the
// poll(2) bits the gateway already switches on: POLLIN, POLLOUT, POLLERR, POLLHUP.

#pragma once

#include <poll.h>

#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#else
static_assert(false, "Reactor requires epoll (Linux) or kqueue (macOS)");
#endif

namespace mahjong {

struct ReadyEvent {
  int fd = -1;
  // POLLIN | POLLOUT | POLLERR | POLLHUP.
  short events = 0;
};

class Reactor {
 public:
  Reactor() = default;
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;
  ~Reactor();

  // Creates the epoll or kqueue fd. Returns false and fills |*error| on failure.
  bool Open(std::string* error);
  bool is_open() const { return fd_ >= 0; }
  void Close();

  // |events| is a subset of POLLIN | POLLOUT. Mod takes the previously registered set so kqueue
  // can EV_DELETE only the filters that were actually added.
  bool Add(int fd, short events);
  bool Mod(int fd, short old_events, short new_events);
  bool Del(int fd, short events);

  // Waits up to |timeout_ms|. Returns the number of ready fds, 0 on timeout, or -1 with errno
  // set. |*out| is replaced with one entry per fd (kqueue's read and write filters are merged).
  int Wait(int timeout_ms, std::vector<ReadyEvent>* out);

 private:
#if defined(__linux__)
  bool Control(int operation, int fd, short events);
#elif defined(__APPLE__)
  bool Change(int fd, short old_events, short new_events);
#endif

  int fd_ = -1;
#if defined(__linux__)
  std::vector<epoll_event> buffer_;
#elif defined(__APPLE__)
  std::vector<struct kevent> buffer_;
#endif
};

}  // namespace mahjong
