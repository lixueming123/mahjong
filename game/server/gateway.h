// Gateway: single-threaded POSIX poll event loop that fronts the actor world.
//
// Responsibilities (PROTOCOL.md §1): accept, NDJSON framing, UTF-8 validation, the 64 KB
// frame limit, inbound rate limiting, heartbeat/idle handling and dispatching complete
// frames into the per-connection ConnectionActor. The loop is the only thread that touches
// a socket; actors reply through Connection::SendFrame, which queues into an outbox that
// the loop flushes.

#pragma once

#include <poll.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "ex_actor/api.h"

#include "connection.h"
#include "connection_actor.h"
#include "lobby.h"
#include "player.h"

namespace mahjong {

// Result of the per-frame rate-limit check (PROTOCOL.md §1.7).
enum class RateLimitDecision { kAccept, kDrop, kClose };

// Asks the running gateway loop to stop; safe to call from a signal handler.
void RequestGatewayStop();
// Installs SIGINT/SIGTERM handlers that request a stop and wake the loop through
// |wake_write_fd|. Pass -1 to detach the wakeup fd (used during shutdown).
void InstallGatewaySignalHandlers(int wake_write_fd);

class Gateway {
 public:
  // |accounts| and |sessions| are handed to every ConnectionActor: the first answers login,
  // register and stats (ACCOUNT_DESIGN.md), the second enforces "one account, one live
  // connection" (§1.3). Both may be null in a build without account support.
  Gateway(uint16_t port, std::shared_ptr<PlayerIdAllocator> player_ids, std::shared_ptr<AccountStore> accounts,
          std::shared_ptr<SessionRegistry> sessions, ex_actor::ActorRef<LobbyActor> lobby);
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;
  ~Gateway();

  // Binds the listening socket and creates the self-pipe. Returns false and fills |*error|.
  bool Start(std::string* error);
  // Runs until RequestGatewayStop() (or Stop()) is called.
  void Run();
  // Stops the loop and closes every client socket. Idempotent.
  void Stop();

  uint16_t port() const { return port_; }
  int wake_write_fd() const { return wake_fds_[1]; }
  uint64_t accepted_connections() const { return accepted_connections_; }
  uint64_t dispatched_frames() const { return dispatched_frames_; }

 private:
  struct Client {
    std::shared_ptr<Connection> conn;
    ex_actor::ActorRef<ConnectionActor> actor;
    std::string inbound;
    // Read cursor into |inbound|: everything before it has been consumed. Frames are taken from
    // the cursor instead of erasing each one from the front, so one buffered burst costs a
    // single compaction rather than a memmove per frame (§1.2).
    size_t inbound_begin = 0;
    int64_t last_inbound_ms = 0;
    // The last_inbound_ms value an idle report was already dispatched for (-1: none yet).
    // The idle check compares it with last_inbound_ms, so every inbound frame (including a
    // heartbeat `ping`) implicitly restarts the window: the timeout measures silence, not
    // connection age (PROTOCOL.md §1.6).
    int64_t idle_reported_for_inbound_ms = -1;
    bool close_pending = false;
    int64_t close_deadline_ms = 0;
    bool dead = false;
    // Sliding-window rate limiting (§1.7).
    std::deque<int64_t> frame_times;
    int64_t limited_second = -1;
    int consecutive_limited_seconds = 0;
    int64_t rate_limit_reported_second = -1;
  };

  void DrainWakePipe();
  void AcceptClients();
  void HandleClientEvents(Client& client, short revents);
  void ReadClient(Client& client);
  void ProcessInboundFrames(Client& client);
  // Drops the already-consumed prefix of |client.inbound| and resets the read cursor.
  static void CompactInbound(Client& client);
  void FlushOutbox(Client& client);
  RateLimitDecision CheckRateLimit(Client& client);
  void CloseClient(Client& client, const std::string& reason);
  void BeginCloseIfRequested(Client& client);
  void CheckTimers();
  void CompactClients();
  void DispatchFrame(Client& client, std::string frame);

  uint16_t port_;
  std::shared_ptr<PlayerIdAllocator> player_ids_;
  std::shared_ptr<AccountStore> accounts_;
  std::shared_ptr<SessionRegistry> sessions_;
  ex_actor::ActorRef<LobbyActor> lobby_;
  int listen_fd_ = -1;
  int wake_fds_[2] = {-1, -1};
  bool running_ = false;
  int next_client_id_ = 0;
  uint64_t accepted_connections_ = 0;
  uint64_t dispatched_frames_ = 0;
  std::vector<Client> clients_;
};

}  // namespace mahjong
