#include "connection.h"

#include <unistd.h>

#include <cerrno>
#include <utility>

#include "net.h"

namespace mahjong {
namespace {

// True when the byte is in the pipe or a previous byte is already waiting (EAGAIN).
bool WriteWakeByte(int fd) {
  const char kByte = 'w';
  while (true) {
    const ssize_t written = ::write(fd, &kByte, 1);
    if (written >= 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    return false;
  }
}

}  // namespace

Connection::Connection(int fd, int id, int wake_write_fd, std::atomic_flag* wake_flag)
    : fd_(fd), id_(id), wake_write_fd_(wake_write_fd), wake_flag_(wake_flag) {}

void Connection::Wake() {
  if (wake_write_fd_ < 0) {
    return;
  }
  // The outbox mutex is already released. The flag only coalesces the pipe write: the first
  // sender arms it and writes one byte, and everyone else returns without a syscall.
  if (wake_flag_ == nullptr) {
    static_cast<void>(WriteWakeByte(wake_write_fd_));
    return;
  }
  if (wake_flag_->test_and_set(std::memory_order_acq_rel)) {
    return;
  }
  if (!WriteWakeByte(wake_write_fd_)) {
    wake_flag_->clear(std::memory_order_release);
  }
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
