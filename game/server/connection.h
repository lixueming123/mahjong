// Thread-safe handle of one client TCP connection.
//
// Ownership split (keeps the I/O loop single-threaded, see PROTOCOL.md §0.5):
//   * the gateway event loop owns the socket fd, the read path and the close path; it is
//     the only thread calling read()/write()/close();
//   * actors (PlayerActor / LobbyActor / RoomActor) push outbound frames with
//     SendFrame(); frames land in an outbox that the loop flushes. The producer then calls the
//     connection's waker, which posts a flush onto the connection's strand, so the loop either
//     wakes up or already has a flush queued behind the one it is running.

#pragma once

#include <functional>
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
// shrinks it is the writer, and whether that makes progress depends on the *client* — a peer that
// never reads eventually fills its receive window, so the write stops completing. Without a cap
// nothing stops the growth: one connection that keeps a `ping` going every few seconds (enough to
// keep `last_inbound_ms` fresh, so the idle timeout never fires) can hold every frame of a whole
// match in server memory. The budget covers the batch the writer has in hand as well as the queue
// (see in_flight_bytes_ in connection.cc), so draining cannot move the overshoot out of sight.
//
// 4 MB is chosen to be several times the largest legitimate burst: a client that stops reading for
// a moment may legitimately queue a whole match of frames (~2.3 MB measured on a classic match,
// which is what the room broadcasts to all four seats). A peer that exceeds this is not merely
// slow, it is not reading at all.
inline constexpr size_t kMaxOutboxBytes = 4 * 1024 * 1024;

class Connection {
 public:
  // Called with the outbox mutex released, from whichever thread queued a frame. The gateway
  // installs one to wake its writer: it posts a drain onto the connection's strand. Tests pass
  // none, and then a queued frame simply sits in the outbox (which is exactly what the outbox
  // bound tests want).
  using Waker = std::function<void()>;

  // |owns_fd| is false when something else closes the socket (an asio socket, in the gateway):
  // MarkClosed then only marks the connection dead, because closing a descriptor that asio already
  // closed — or worse, already reused — is a real fault, not a tidiness issue.
  Connection(int fd, int id, bool owns_fd = true);
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() = default;

  // Installs the wake callback. Must be called before any peer can queue a frame; the gateway does
  // it right after constructing the connection and before spawning its PlayerActor.
  void SetWaker(Waker waker);

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
  // Unflushed bytes currently queued. Bounded by kMaxOutboxBytes; a test (and a log line) uses it
  // to see the backpressure that a peer which stopped reading produces.
  size_t outbox_bytes() const;
  // Bytes the writer has taken out of the outbox but has not finished writing. Together with
  // outbox_bytes() this is what kMaxOutboxBytes bounds: a drained batch is still live memory, so
  // counting only the outbox would let one connection hold twice the cap.
  size_t in_flight_bytes() const;

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
  // Hands the queued bytes to the writer. |out| receives the batch and the connection records its
  // size as in-flight, so kMaxOutboxBytes covers "queued + being written" as one budget. The
  // writer must report the outcome with FinishWrite(), which is what releases the batch again.
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
  // Bytes handed to the writer by DrainOutbox and not yet released by FinishWrite. Counted
  // against kMaxOutboxBytes, because that memory is just as real as the queued bytes.
  size_t in_flight_bytes_ = 0;
  bool closed_ = false;
  bool close_requested_ = false;
  std::string close_reason_;
  int bad_frames_ = 0;
  int invalid_names_ = 0;
  int illegal_ops_ = 0;
};

}  // namespace mahjong
