// Gateway: the network front of the actor world, on an asio event loop.
//
// Responsibilities (PROTOCOL.md §1): accept, NDJSON framing, UTF-8 validation, the 64 KB frame
// limit, inbound rate limiting, heartbeat/idle handling — and nothing else. It knows no frame
// *type*: it hands each complete line to that client's PlayerActor, which is the entry point for
// client protocol.
//
// This replaces the hand-written epoll/kqueue reactor (reactor.h/net.cc) with asio, and it is
// deliberately a *separate layer* from the actor world: the I/O half is `asio::awaitable`
// coroutines, the game half stays on `stdexec::task`, and the two never co_await each other.
//
//   * inbound:  an awaitable read loop cuts frames out of the byte stream and calls the plain,
//               non-suspending `actor.Send<&PlayerActor::Dispatch>(...)`. Fire and forget: the
//               sender is started on the actor's own pool via ActorSendScope, so the awaitable
//               never waits for an actor, and no coroutine type ever crosses the boundary.
//   * outbound: any actor thread calls Connection::SendFrame. That appends to the connection's
//               bounded outbox and then calls its waker, which `asio::post`s a flush onto the
//               connection's strand. The strand is the only place the socket is written, which
//               is also why the outbox needs no asio locking of its own. Asio has no timed
//               operations, so the writer arms its own bound with `cancel_after`: a peer that
//               stops reading never fails the write, it just never completes it.
//
// The one thing that does cross is the socket itself: asio owns the fd, so Connection is built
// with owns_fd=false and closes nothing (Connection::MarkClosed only marks it dead).
//
// Threading: one io_context, `io_threads` threads running it. Every connection has its own strand,
// so per-connection state (inbound buffer, framing cursor, rate-limit window) is single-threaded
// by construction and needs no mutex.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "connection.h"
#include "ex_actor/api.h"
#include "lobby.h"
#include "player.h"
#include "player_actor.h"

namespace mahjong {

// Asks the running gateway loop to stop; safe to call from a signal handler.
void RequestGatewayStop();
// Installs SIGINT/SIGTERM handlers that request a stop. There is no wakeup fd: the signal handler
// only sets a flag, and Run() polls it from its own timer, so nothing has to be async-signal-safe
// except the flag itself. |wake_write_fd| is ignored (pass gateway.wake_write_fd()).
void InstallGatewaySignalHandlers(int wake_write_fd);

// Result of the per-frame rate-limit check (PROTOCOL.md §1.7).
enum class RateLimitDecision { kAccept, kDrop, kClose };

class Gateway {
 public:
  // |sessions| is handed to every PlayerActor and enforces "one account, one live connection"
  // (§1.3). Login, register and stats go through GetDbService() (ACCOUNT_DESIGN.md).
  //
  // |io_threads| is how many threads run the io_context. 0 means "pick from the hardware"
  // (clamped to 1..4): the loop is I/O bound, so more threads than cores buys nothing but
  // contention on the accept path.
  Gateway(uint16_t port, std::shared_ptr<PlayerIdAllocator> player_ids, std::shared_ptr<SessionRegistry> sessions,
          ex_actor::ActorRef<LobbyActor> lobby, size_t io_threads = 0);
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;
  ~Gateway();

  // Binds the listening socket. Returns false and fills |*error| on failure.
  bool Start(std::string* error);
  // Runs until RequestGatewayStop() (or Stop()) is called. Blocks the calling thread; the io
  // threads do the actual work.
  void Run();
  // Stops the loop and marks every live connection dead (no producer can queue into an outbox
  // nobody drains). It deliberately does not run the per-client close path: descriptors are owned by
  // asio and released when the io_context is destroyed, after every io thread has returned. So
  // PlayerActor::OnClosed does not run here — the accounts and seats go away with the process.
  // Idempotent.
  void Stop();

  uint16_t port() const { return port_; }
  // Kept so main.cc's signal-handler call needs no special case. There is no wakeup fd in this
  // gateway; it always returns -1.
  int wake_write_fd() const { return -1; }
  uint64_t accepted_connections() const { return accepted_connections_.load(std::memory_order_relaxed); }
  uint64_t dispatched_frames() const { return dispatched_frames_.load(std::memory_order_relaxed); }

 private:
  // Per-connection state. Owned by the gateway, touched only from that connection's strand
  // (accepted by the accept loop before hand-off, then by the read/write coroutines).
  struct Client {
    std::shared_ptr<Connection> conn;
    ex_actor::ActorRef<PlayerActor> actor;
    asio::ip::tcp::socket socket;
    asio::strand<asio::any_io_executor> strand;
    // Lives for the connection's lifetime: RunWriter arms it for the close grace period, and it
    // must outlive that call or the wait would be cancelled and the connection left open.
    asio::steady_timer close_timer;

    // Inbound framing: everything before |inbound_begin| has been consumed, so one buffered burst
    // costs a single compaction instead of a memmove per frame (§1.2).
    std::string inbound;
    size_t inbound_begin = 0;

    int64_t last_inbound_ms = 0;
    // Monotonic deadline for the first complete frame; 0 once one has been dispatched. This is the
    // slowloris bound (protocol::kFirstFrameTimeoutMs): a peer that dribbles a byte at a time keeps
    // last_inbound_ms fresh, so the idle window never fires, and it never completes a line, so
    // neither the frame handler nor the bad-frame quota ever sees it.
    int64_t first_frame_deadline_ms = 0;

    // The last_inbound_ms value an idle report was already dispatched for (-1: none yet). The idle
    // check compares it with last_inbound_ms, so every inbound frame (including a heartbeat
    // `ping`) implicitly restarts the window: the timeout measures silence, not connection age
    // (PROTOCOL.md §1.6).
    int64_t idle_reported_for_inbound_ms = -1;

    // Sliding-window rate limiting (§1.7).
    std::deque<int64_t> frame_times;
    int64_t limited_second = -1;
    int64_t consecutive_limited_seconds = 0;
    int64_t rate_limit_reported_second = -1;

    // Set once the connection has been closed. Atomic because CloseClient can be reached from the
    // read coroutine, the write coroutine and (via a post) the idle sweep — different strands, so a
    // plain bool was a race that occasionally let a close be "lost" or run twice.
    std::atomic<bool> dead {false};
    // True while a RunWriter post is queued or running. Producers only post when this flips from
    // false, so a burst of frames from four seats costs one post instead of one each (the handler
    // drains whatever is queued when it runs).
    std::atomic<bool> wake_pending {false};
    // True while an idle sweep's post is queued, so the 250 ms sweep does not pile up one post per
    // connection per tick behind a busy strand.
    std::atomic<bool> idle_check_pending {false};
    // True while a flush coroutine owns the socket for writing, so producers do not spawn a second
    // one and interleave their bytes.
    bool writing = false;
    // True once the outbox should be flushed and the connection closed: either the close was
    // requested by an actor (SendFrameThenClose / RequestClose) or the read side is finished.
    bool close_when_drained = false;
    // Monotonic deadline for the final flush; past it the connection is closed regardless
    // (protocol::kCloseFlushTimeoutMs).
    int64_t close_deadline_ms = 0;
    // The short grace period the writer gives the outbox to receive the last frame(s) a close
    // implies, before it closes the socket. Zero means "not started".
    int64_t close_grace_ms = 0;
    // True once PlayerActor has been told this connection is gone. The read side sets it as soon as
    // EOF is seen, so the account/seat is released without waiting for the socket to drain.
    bool actor_closed_notified = false;

    Client(asio::ip::tcp::socket socket_in, asio::strand<asio::any_io_executor> strand_in)
        : socket(std::move(socket_in)), strand(std::move(strand_in)), close_timer(strand) {}
  };

  asio::awaitable<void> AcceptLoop();
  asio::awaitable<void> ServeClient(std::shared_ptr<Client> client);
  // Reads until the socket yields EOF/error, handing every complete frame to the actor. Returns
  // when the socket is done; the caller then runs CloseClient.
  asio::awaitable<void> ReadLoop(Client& client);
  // Cuts complete lines out of |client.inbound| and dispatches them. Returns false when the
  // connection must stop being read (close requested, or the frame budget was blown).
  bool ProcessInboundFrames(Client& client);
  RateLimitDecision CheckRateLimit(Client& client);
  void DispatchFrame(Client& client, std::string frame);

  // Called from any thread (the actor pool) by Connection's waker.
  void WakeWriter(const std::shared_ptr<Client>& client);
  // How long the next write may stay incomplete. Takes the min of the general write-stall bound and
  // whatever is left of the close deadline, so a connection on its way out is not kept for the full
  // budget just to deliver one explaining frame.
  static std::chrono::milliseconds WriteBudgetMs(const Client& client);
  // Strand-only work queue: drains the outbox and decides "write more" vs "close".
  void RunWriter(const std::shared_ptr<Client>& client);
  // Writes one drained batch, then hands control back to RunWriter. Strand-only, and the only
  // coroutine that touches the socket for writing.
  asio::awaitable<void> WritePending(std::shared_ptr<Client> client, std::string pending);
  // Flushes whatever is pending and then closes. Runs on the strand.
  void BeginClose(std::shared_ptr<Client> client, std::string reason);

  void CloseClient(std::shared_ptr<Client> client, const std::string& reason);
  // Tells the PlayerActor the connection is gone. Idempotent, so the read side can report EOF
  // immediately while the writer later finishes the socket teardown.
  void NotifyActorClosed(const std::shared_ptr<Client>& client, const std::string& reason);
  void CheckIdle();
  // Both run on the client's strand (posted by CheckIdle), which is where the fields they read and
  // write are owned.
  void ReportIdleIfDue(const std::shared_ptr<Client>& client, int64_t now);

  uint16_t port_;
  std::shared_ptr<PlayerIdAllocator> player_ids_;
  std::shared_ptr<SessionRegistry> sessions_;
  ex_actor::ActorRef<LobbyActor> lobby_;
  size_t io_threads_;

  asio::io_context io_;
  // Keeps io_.run() from returning between accepts; released by Stop().
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
  std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
  std::unique_ptr<asio::steady_timer> idle_timer_;

  std::atomic<bool> running_ {false};
  std::atomic<int> next_client_id_ {0};
  std::atomic<uint64_t> accepted_connections_ {0};
  std::atomic<uint64_t> dispatched_frames_ {0};

  // The live connection registry. Only the idle sweep and AcceptLoop insert, and only
  // CloseClient removes; the mutex covers both, plus the snapshot the sweep walks. Per-client
  // state is NOT protected by it — that stays on the client's strand.
  mutable std::mutex clients_mutex_;
  std::vector<std::shared_ptr<Client>> clients_;
};

}  // namespace mahjong
