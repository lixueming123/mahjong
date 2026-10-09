#include "connection.h"

#include <unistd.h>

#include <cerrno>
#include <utility>

#include "net.h"

namespace mahjong {

Connection::Connection(int fd, int id, int wake_write_fd) : fd_(fd), id_(id), wake_write_fd_(wake_write_fd) {}

void Connection::Wake() {
  if (wake_write_fd_ < 0) {
    return;
  }
  const char kByte = 'w';
  ssize_t written = ::write(wake_write_fd_, &kByte, 1);
  (void)written;  // EAGAIN means the loop already has a pending wakeup; nothing to do.
}

void Connection::AppendFrameLocked(std::string_view frame) {
  outbox_.append(frame.data(), frame.size());
  if (outbox_.empty() || outbox_.back() != '\n') {
    outbox_.push_back('\n');
  }
}

void Connection::SendFrame(std::string_view frame) {
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    AppendFrameLocked(frame);
  }
  Wake();
}

void Connection::SendFrameThenClose(std::string_view frame, std::string reason) {
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    AppendFrameLocked(frame);
    close_requested_ = true;
    if (close_reason_.empty()) {
      close_reason_ = std::move(reason);
    }
  }
  Wake();
}

void Connection::RequestClose(std::string reason) {
  {
    std::lock_guard lock(mutex_);
    if (closed_ || close_requested_) {
      return;
    }
    close_requested_ = true;
    close_reason_ = std::move(reason);
  }
  Wake();
}

bool Connection::closed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

bool Connection::close_requested() const {
  std::lock_guard lock(mutex_);
  return close_requested_;
}

std::string Connection::close_reason() const {
  std::lock_guard lock(mutex_);
  return close_reason_;
}

bool Connection::HasPendingOutbox() const {
  std::lock_guard lock(mutex_);
  return !outbox_.empty();
}

int Connection::AddBadFrame() {
  std::lock_guard lock(mutex_);
  return ++bad_frames_;
}

int Connection::AddInvalidName() {
  std::lock_guard lock(mutex_);
  return ++invalid_names_;
}

int Connection::AddIllegalOp() {
  std::lock_guard lock(mutex_);
  return ++illegal_ops_;
}

int Connection::bad_frames() const {
  std::lock_guard lock(mutex_);
  return bad_frames_;
}

int Connection::invalid_names() const {
  std::lock_guard lock(mutex_);
  return invalid_names_;
}

int Connection::illegal_ops() const {
  std::lock_guard lock(mutex_);
  return illegal_ops_;
}

void Connection::DrainOutbox(std::string* out) {
  std::lock_guard lock(mutex_);
  out->clear();
  out->swap(outbox_);
}

void Connection::PushBackOutbox(std::string remainder) {
  if (remainder.empty()) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (closed_) {
    return;
  }
  outbox_.insert(0, remainder);
}

void Connection::MarkClosed() {
  int fd = -1;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    fd = fd_;
    fd_ = -1;
  }
  mahjong::net::CloseFd(fd);
}

std::string Connection::Describe() const {
  std::lock_guard lock(mutex_);
  return "conn#" + std::to_string(id_) + "(fd=" + std::to_string(fd_) + ")";
}

}  // namespace mahjong
