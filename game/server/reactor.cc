#include "reactor.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace mahjong {
namespace {

constexpr int kWaitBatch = 64;

}  // namespace

Reactor::~Reactor() { Close(); }

bool Reactor::Open(std::string* error) {
  if (fd_ >= 0) {
    return true;
  }
#if defined(__linux__)
  fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd_ < 0) {
    *error = std::string("epoll_create1() failed: ") + std::strerror(errno);
    return false;
  }
#elif defined(__APPLE__)
  fd_ = ::kqueue();
  if (fd_ < 0) {
    *error = std::string("kqueue() failed: ") + std::strerror(errno);
    return false;
  }
  int flags = ::fcntl(fd_, F_GETFD, 0);
  if (flags >= 0) {
    ::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC);
  }
#endif
  buffer_.resize(kWaitBatch);
  return true;
}

void Reactor::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  buffer_.clear();
}

#if defined(__linux__)

bool Reactor::Control(int operation, int fd, short events) {
  if (fd_ < 0 || fd < 0) {
    errno = EBADF;
    return false;
  }
  epoll_event event {};
  event.data.fd = fd;
  if ((events & POLLIN) != 0) {
    event.events |= EPOLLIN | EPOLLRDHUP;
  }
  if ((events & POLLOUT) != 0) {
    event.events |= EPOLLOUT;
  }
  while (true) {
    if (::epoll_ctl(fd_, operation, fd, &event) == 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (operation == EPOLL_CTL_DEL && errno == ENOENT) {
      return true;
    }
    return false;
  }
}

bool Reactor::Add(int fd, short events) { return Control(EPOLL_CTL_ADD, fd, events); }

bool Reactor::Mod(int fd, short old_events, short new_events) {
  (void)old_events;
  return Control(EPOLL_CTL_MOD, fd, new_events);
}

bool Reactor::Del(int fd, short events) {
  (void)events;
  return Control(EPOLL_CTL_DEL, fd, 0);
}

int Reactor::Wait(int timeout_ms, std::vector<ReadyEvent>* out) {
  out->clear();
  if (fd_ < 0) {
    errno = EBADF;
    return -1;
  }
  const int ready = ::epoll_wait(fd_, buffer_.data(), static_cast<int>(buffer_.size()), timeout_ms);
  if (ready < 0) {
    return -1;
  }
  out->reserve(static_cast<size_t>(ready));
  for (int index = 0; index < ready; ++index) {
    const epoll_event& event = buffer_[static_cast<size_t>(index)];
    short bits = 0;
    if ((event.events & EPOLLIN) != 0) {
      bits |= POLLIN;
    }
    if ((event.events & EPOLLOUT) != 0) {
      bits |= POLLOUT;
    }
    if ((event.events & EPOLLERR) != 0) {
      bits |= POLLERR;
    }
    if ((event.events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
      bits |= POLLHUP;
    }
    out->push_back(ReadyEvent {.fd = event.data.fd, .events = bits});
  }
  return ready;
}

#elif defined(__APPLE__)

bool Reactor::Change(int fd, short old_events, short new_events) {
  if (fd_ < 0 || fd < 0) {
    errno = EBADF;
    return false;
  }
  struct kevent changes[2];
  int count = 0;
  const bool had_read = (old_events & POLLIN) != 0;
  const bool want_read = (new_events & POLLIN) != 0;
  if (had_read != want_read) {
    EV_SET(&changes[count], static_cast<uintptr_t>(fd), EVFILT_READ, want_read ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    ++count;
  }
  const bool had_write = (old_events & POLLOUT) != 0;
  const bool want_write = (new_events & POLLOUT) != 0;
  if (had_write != want_write) {
    EV_SET(&changes[count], static_cast<uintptr_t>(fd), EVFILT_WRITE, want_write ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    ++count;
  }
  if (count == 0) {
    return true;
  }
  while (true) {
    if (::kevent(fd_, changes, count, nullptr, 0, nullptr) >= 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    // Deleting a filter that is already gone happens if the fd was closed elsewhere.
    if (!want_read && !want_write && errno == ENOENT) {
      return true;
    }
    return false;
  }
}

bool Reactor::Add(int fd, short events) { return Change(fd, /*old_events=*/0, events); }

bool Reactor::Mod(int fd, short old_events, short new_events) { return Change(fd, old_events, new_events); }

bool Reactor::Del(int fd, short events) { return Change(fd, events, /*new_events=*/0); }

int Reactor::Wait(int timeout_ms, std::vector<ReadyEvent>* out) {
  out->clear();
  if (fd_ < 0) {
    errno = EBADF;
    return -1;
  }
  struct timespec timeout {};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
  const int ready = ::kevent(fd_, nullptr, 0, buffer_.data(), static_cast<int>(buffer_.size()), &timeout);
  if (ready < 0) {
    return -1;
  }
  out->reserve(static_cast<size_t>(ready));
  for (int index = 0; index < ready; ++index) {
    const struct kevent& event = buffer_[static_cast<size_t>(index)];
    short bits = 0;
    if ((event.flags & EV_ERROR) != 0) {
      bits |= POLLERR;
    }
    if (event.filter == EVFILT_READ) {
      bits |= POLLIN;
      if ((event.flags & EV_EOF) != 0) {
        bits |= POLLHUP;
      }
    } else if (event.filter == EVFILT_WRITE) {
      bits |= POLLOUT;
      if ((event.flags & EV_EOF) != 0) {
        bits |= POLLHUP;
      }
    }
    const int event_fd = static_cast<int>(event.ident);
    bool merged = false;
    for (ReadyEvent& existing : *out) {
      if (existing.fd == event_fd) {
        existing.events |= bits;
        merged = true;
        break;
      }
    }
    if (!merged) {
      out->push_back(ReadyEvent {.fd = event_fd, .events = bits});
    }
  }
  return static_cast<int>(out->size());
}

#endif

}  // namespace mahjong
