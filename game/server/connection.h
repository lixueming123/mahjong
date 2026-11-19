// Thread-safe handle of one client TCP connection.
//
// Ownership split:
//   * the gateway event loop owns the socket fd, the read path and the close path — the only thread
//     calling read()/write()/close();
//   * actors push outbound frames with SendFrame(); they land in an outbox the loop flushes, and the
//     producer then calls the waker, which posts a flush onto the connection's strand.

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "protocol.h"

namespace mahjong {

// How much unflushed outbound data one connection may hold before the gateway gives up on it: a peer
// that stops draining is a client fault.
//
// The problem this bounds: `outbox_` only grows from the producer side and only the writer shrinks
// it, so a peer that never reads stops the write from completing. Without a cap, one connection that
// keeps a `ping` going (enough to keep the idle timeout away) can hold a whole match in memory. The
// budget covers the batch in the writer's hand too (in_flight_bytes_ in connection.cc).
//
// 4 MB is several times the largest legitimate burst: a client that stops reading for a moment may
// queue a whole match of frames (~2.3 MB measured on a classic match).
inline constexpr size_t kMaxOutboxBytes = 4 * 1024 * 1024;

class Connection {
 public:
  // Called with the outbox mutex released, from whichever thread queued a frame. The gateway installs
  // one to wake its writer; tests pass none, and then a queued frame sits in the outbox.
  using Waker = std::function<void()>;

  // |owns_fd| is false when something else closes the socket (an asio socket, in the gateway):
  // MarkClosed then only marks the connection dead, because closing a descriptor asio already closed
  // — or reused — is a real fault.
  Connection(int fd, int id, bool owns_fd = true);
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() = default;

  // Installs the wake callback. Must be called before any peer can queue a frame.
  void SetWaker(Waker waker);

  int fd() const { return fd_; }
  int id() const { return id_; }

  // ---- producer side (any thread) -----------------------------------------------------

  // Queues one frame; the '\n' terminator is appended when missing. No-op after close.
  //
  // A frame that would push the outbox past kMaxOutboxBytes is dropped and the connection closed:
  // the peer that caused it is by definition not reading.
  void SendFrame(std::string_view frame);
  // Queues the frame and asks the loop to close the connection once the outbox is flushed.
  void SendFrameThenClose(std::string_view frame, std::string reason);
  // Asks the loop to close the connection once the outbox is flushed.
  void RequestClose(std::string reason);

  bool closed() const;
  bool close_requested() const;
  std::string close_reason() const;
  // Unflushed bytes currently queued; bounded by kMaxOutboxBytes.
  size_t outbox_bytes() const;
  // Bytes the writer has taken out of the outbox but not finished writing. Counted with
  // outbox_bytes() against kMaxOutboxBytes, because a drained batch is still live memory.
  size_t in_flight_bytes() const;

  // ---- quota counters; each returns the value after incrementing ----------------------
  int AddBadFrame();
  int AddInvalidName();
  int AddIllegalOp();
  int bad_frames() const;
  int invalid_names() const;
  int illegal_ops() const;

  // Accounts one BAD_REQUEST-class / illegal-operation error and asks the loop to close the connection
  // once the frozen quota is spent. Every producer goes through these two, so the quota spans the
  // connection actor, the gateway and a room through its seat sink.
  int RecordBadFrame() {
    const int count = AddBadFrame();
    if (count >= protocol::kMaxBadFrames) {
      RequestClose("too many bad frames");
    }
    return count;
  }
  int RecordIllegalOp() {
    const int count = AddIllegalOp();
    if (count >= protocol::kMaxIllegalOps) {
      RequestClose("too many illegal operations");
    }
    return count;
  }

  // ---- consumer side (gateway loop only) ----------------------------------------------
  // Hands the queued bytes to the writer. |out| receives the batch and the connection records its size
  // as in-flight; the writer must report the outcome with FinishWrite().
  void DrainOutbox(std::string* out);
  void FinishWrite();
  // Closes the fd and marks the connection dead; idempotent.
  void MarkClosed();

  std::string Describe() const;

 private:
  void Wake(const Waker& waker);
  void AppendFrameLocked(std::string_view frame);

  mutable std::mutex mutex_;
  int fd_;
  int id_;
  Waker waker_;
  bool owns_fd_;
  std::string outbox_;
  // Bytes handed to the writer by DrainOutbox and not yet released by FinishWrite.
  size_t in_flight_bytes_ = 0;
  bool closed_ = false;
  bool close_requested_ = false;
  std::string close_reason_;
  int bad_frames_ = 0;
  int invalid_names_ = 0;
  int illegal_ops_ = 0;
};

}  // namespace mahjong
