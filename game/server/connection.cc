#include "connection.h"

#include <utility>

#include "net.h"

namespace mahjong {

Connection::Connection(int fd, int id, bool owns_fd) : fd_(fd), id_(id), owns_fd_(owns_fd) {}

void Connection::SetWaker(Waker waker) {
  std::lock_guard lock(mutex_);
  waker_ = std::move(waker);
}

void Connection::Wake(const Waker& waker) {
  // Called with the outbox mutex released; the waker was copied under it (see the callers), so a
  // concurrent SetWaker/MarkClosed cannot pull it out from under this call. With no waker (unit
  // tests) there is nothing to wake: the frame stays queued until someone drains it.
  if (waker) {
    waker();
  }
}

void Connection::AppendFrameLocked(std::string_view frame) {
  // Bounded on purpose: see kMaxOutboxBytes (connection.h). A peer that never reads keeps the
  // gateway's write in EAGAIN, and the unsent tail comes straight back here, so without this the
  // outbox is an unbounded buffer fed by remote input. Dropping the frame and closing is the only
  // correct answer — the peer that filled it is not going to drain it.
  //
  // The budget covers the batch the writer is currently working on (in_flight_bytes_) as well as
  // what is queued: draining does not free memory, it only moves it into the write coroutine.
  const size_t needed = frame.size() + 1;  // + the terminator appended below
  if (in_flight_bytes_ + outbox_.size() + needed > kMaxOutboxBytes) {
    close_requested_ = true;
    if (close_reason_.empty()) {
      close_reason_ = "outbound buffer full";
    }
    return;
  }
  outbox_.append(frame.data(), frame.size());
  if (outbox_.empty() || outbox_.back() != '\n') {
    outbox_.push_back('\n');
  }
}

void Connection::SendFrame(std::string_view frame) {
  Waker waker;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    AppendFrameLocked(frame);
    waker = waker_;
  }
  Wake(waker);
}

void Connection::SendFrameThenClose(std::string_view frame, std::string reason) {
  Waker waker;
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
    waker = waker_;
  }
  Wake(waker);
}

void Connection::RequestClose(std::string reason) {
  Waker waker;
  {
    std::lock_guard lock(mutex_);
    if (closed_ || close_requested_) {
      return;
    }
    close_requested_ = true;
    close_reason_ = std::move(reason);
    waker = waker_;
  }
  Wake(waker);
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

size_t Connection::outbox_bytes() const {
  std::lock_guard lock(mutex_);
  return outbox_.size();
}

size_t Connection::in_flight_bytes() const {
  std::lock_guard lock(mutex_);
  return in_flight_bytes_;
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
  // The batch is now the writer's, but it is still this connection's memory. Recording it here is
  // what keeps kMaxOutboxBytes a bound on the connection and not just on the queue: a peer that
  // never reads empties the queue on every pass, and without this the writer's copy and a freshly
  // refilled queue add up to twice the cap.
  in_flight_bytes_ = out->size();
}

void Connection::FinishWrite() {
  std::lock_guard lock(mutex_);
  in_flight_bytes_ = 0;
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
    waker_ = nullptr;  // the gateway's I/O object is going away with the socket
  }
  if (owns_fd_) {
    mahjong::net::CloseFd(fd);
  }
}

std::string Connection::Describe() const {
  std::lock_guard lock(mutex_);
  return "conn#" + std::to_string(id_) + "(fd=" + std::to_string(fd_) + ")";
}

}  // namespace mahjong
