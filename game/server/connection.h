// Thread-safe handle of one client TCP connection.
//
// Ownership split (keeps the I/O loop single-threaded, see PROTOCOL.md §0.5):
//   * the gateway event loop owns the socket fd, the read path and the close path; it is
//     the only thread calling read()/write()/close();
//   * actors (PlayerActor / LobbyActor / RoomActor) push outbound frames with
//     SendFrame(); frames land in an outbox that the loop flushes. The first sender while
//     the loop is asleep writes one byte to a self-pipe; later senders only set a shared flag.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "protocol.h"

namespace mahjong {

// How much unflushed outbound data one connection may hold before the gateway gives up on it
// (PROTOCOL §1.2.2 / §7.2: a connection that stops draining is a client fault, closed like any
// other quota).
//
// The problem this bounds: `outbox_` only ever grows from the producer side. The single thing that
// shrinks it is the gateway's `send()`, and whether that succeeds depends on the *client* — a peer
// that never reads eventually fills its receive window, TCP advertises zero, the server's `send()`
// returns EAGAIN, and the gateway pushes the unsent tail straight back into the outbox. EAGAIN is
// retried rather than treated as an error, so without a cap nothing ever stops the growth: one
// connection that sends a `ping` every few seconds (enough to keep `last_inbound_ms` fresh, so the
// idle timeout never fires) can hold every frame of a whole match in server memory.
//
// 4 MB is chosen to be several times the largest legitimate burst: a client that stops reading for
// a moment may legitimately queue a whole match of frames (~2.3 MB measured on a classic match,
// which is what the room broadcasts to all four seats). A peer that exceeds this is not merely
// slow, it is not reading at all.
inline constexpr size_t kMaxOutboxBytes = 4 * 1024 * 1024;

class Connection {
 public:
  // |wake_flag| is shared by every connection of one gateway. Null skips coalescing and writes
  // a wake byte on every call (tests pass a negative wake fd and never get that far).
  Connection(int fd, int id, int wake_write_fd, std::atomic_flag* wake_flag = nullptr);
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() = default;

  int fd() const { return fd_; }
  int id() const { return id_; }

  // ---- producer side (any thread) -----------------------------------------------------

  // Queues one frame. The '\n' terminator is appended when missing. No-op after close.
  //
  // A frame that would push the outbox past kMaxOutboxBytes is dropped and the connection is
  // closed instead: see that constant for why an unbounded outbox is a remote memory-exhaustion
  // hole rather than merely untidy. Closing (not growing) is the only safe answer, because the
  // peer that caused it is by definition not reading.
  void SendFrame(std::string_view frame);
  // Queues the frame and asks the loop to close the connection once the outbox is flushed.
  void SendFrameThenClose(std::string_view frame, std::string reason);
  // Asks the loop to close the connection once the outbox is flushed.
  void RequestClose(std::string reason);

  bool closed() const;
  bool close_requested() const;
  std::string close_reason() const;
  bool HasPendingOutbox() const;
  // Unflushed bytes currently queued. Bounded by kMaxOutboxBytes; a test (and a log line) uses it
  // to see the backpressure that a peer which stopped reading produces.
  size_t outbox_bytes() const;

  // ---- quota counters (PROTOCOL §7.2); each returns the value after incrementing ------
  int AddBadFrame();
  int AddInvalidName();
  int AddIllegalOp();
  int bad_frames() const;
  int invalid_names() const;
  int illegal_ops() const;

  // Accounts one BAD_REQUEST-class / illegal-operation error and asks the loop to close the
  // connection once the frozen quota is spent (§7.2). Every producer of these errors — the
  // connection actor, the gateway and a room through its seat sink — goes through these two,
  // so the disconnect quota spans all of them. Returns the counter after incrementing.
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
  void DrainOutbox(std::string* out);
  void PushBackOutbox(std::string remainder);
  // Closes the fd and marks the connection dead; idempotent.
  void MarkClosed();

  std::string Describe() const;

 private:
  void Wake();
  void AppendFrameLocked(std::string_view frame);

  mutable std::mutex mutex_;
  int fd_;
  int id_;
  int wake_write_fd_;
  std::atomic_flag* wake_flag_ = nullptr;
  std::string outbox_;
  bool closed_ = false;
  bool close_requested_ = false;
  std::string close_reason_;
  int bad_frames_ = 0;
  int invalid_names_ = 0;
  int illegal_ops_ = 0;
};

}  // namespace mahjong
