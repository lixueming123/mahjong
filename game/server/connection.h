// Thread-safe handle of one client TCP connection.
//
// Ownership split (keeps the I/O loop single-threaded, see PROTOCOL.md §0.5):
//   * the gateway poll loop owns the socket fd, the read path and the close path; it is
//     the only thread calling read()/write()/close();
//   * actors (ConnectionActor / LobbyActor / RoomActor) push outbound frames with
//     SendFrame(); frames land in an outbox that the loop flushes, and a self-pipe wakes
//     the loop when new data arrives.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "protocol.h"

namespace mahjong {

class Connection {
 public:
  Connection(int fd, int id, int wake_write_fd);
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() = default;

  int fd() const { return fd_; }
  int id() const { return id_; }

  // ---- producer side (any thread) -----------------------------------------------------

  // Queues one frame. The '\n' terminator is appended when missing. No-op after close.
  void SendFrame(std::string_view frame);
  // Queues the frame and asks the loop to close the connection once the outbox is flushed.
  void SendFrameThenClose(std::string_view frame, std::string reason);
  // Asks the loop to close the connection once the outbox is flushed.
  void RequestClose(std::string reason);

  bool closed() const;
  bool close_requested() const;
  std::string close_reason() const;
  bool HasPendingOutbox() const;

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
  std::string outbox_;
  bool closed_ = false;
  bool close_requested_ = false;
  std::string close_reason_;
  int bad_frames_ = 0;
  int invalid_names_ = 0;
  int illegal_ops_ = 0;
};

}  // namespace mahjong
