#include "gateway.h"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include "actor_scope.h"
#include "clock.h"
#include "net.h"
#include "protocol.h"
#include "utf8.h"

#include "ex_actor/internal/platform.h"

namespace ex = stdexec;

namespace mahjong {
namespace {

volatile sig_atomic_t g_stop_requested = 0;
volatile sig_atomic_t g_wake_write_fd = -1;

void HandleStopSignal(int signal_number) {
  (void)signal_number;
  g_stop_requested = 1;
  int fd = g_wake_write_fd;
  if (fd >= 0) {
    const char kByte = 's';
    ssize_t ignored = ::write(fd, &kByte, 1);
    (void)ignored;  // async-signal-safe; EAGAIN means a wakeup is already pending
  }
}

bool StopRequested() { return g_stop_requested != 0; }

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS/BSD use SO_NOSIGPIPE instead.
#endif

}  // namespace

void RequestGatewayStop() { g_stop_requested = 1; }

void InstallGatewaySignalHandlers(int wake_write_fd) {
  g_wake_write_fd = wake_write_fd;
  if (wake_write_fd < 0) {
    return;
  }
  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  // No SA_RESTART: poll() must return EINTR so the loop notices the stop request.
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  ::signal(SIGPIPE, SIG_IGN);
}

Gateway::Gateway(uint16_t port, std::shared_ptr<PlayerIdAllocator> player_ids, DbService db,
                 std::shared_ptr<SessionRegistry> sessions, ex_actor::ActorRef<LobbyActor> lobby)
    : port_(port),
      player_ids_(std::move(player_ids)),
      db_(std::move(db)),
      sessions_(std::move(sessions)),
      lobby_(std::move(lobby)) {}

Gateway::~Gateway() {
  Stop();
  if (wake_fds_[0] >= 0 || wake_fds_[1] >= 0) {
    InstallGatewaySignalHandlers(-1);
  }
  net::CloseFd(wake_fds_[0]);
  net::CloseFd(wake_fds_[1]);
  wake_fds_[0] = wake_fds_[1] = -1;
}

bool Gateway::Start(std::string* error) {
  listen_fd_ = net::CreateListener(port_, error);
  if (listen_fd_ < 0) {
    return false;
  }
  // Report the effective port (useful with --port 0 in tests).
  sockaddr_in bound {};
  socklen_t bound_length = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    port_ = ntohs(bound.sin_port);
  }
  if (!net::CreateWakePipe(wake_fds_, error)) {
    net::CloseFd(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  running_ = true;
  spdlog::info("gateway: listening on {}:{}", protocol::kListenAddress, port_);
  return true;
}

void Gateway::Stop() {
  running_ = false;
  for (Client& client : clients_) {
    if (!client.dead) {
      client.dead = true;
      client.conn->MarkClosed();
    }
  }
  clients_.clear();
  net::CloseFd(listen_fd_);
  listen_fd_ = -1;
}

void Gateway::Run() {
  ex_actor::internal::SetThreadName("gw-poll");
  if (listen_fd_ < 0) {
    spdlog::error("gateway: Run() called before a successful Start()");
    return;
  }
  running_ = true;
  std::vector<pollfd> poll_fds;
  std::vector<size_t> poll_clients;
  while (!StopRequested() && running_) {
    poll_fds.clear();
    poll_clients.clear();
    poll_fds.push_back(pollfd {listen_fd_, POLLIN, 0});
    poll_fds.push_back(pollfd {wake_fds_[0], POLLIN, 0});
    for (size_t index = 0; index < clients_.size(); ++index) {
      Client& client = clients_[index];
      if (client.dead) {
        continue;
      }
      // Once a close is pending only the flush of the outbox still matters.
      bool closing = client.close_pending || client.conn->close_requested();
      short events = closing ? POLLOUT : POLLIN;
      if (!closing && client.conn->HasPendingOutbox()) {
        events |= POLLOUT;
      }
      poll_fds.push_back(pollfd {client.conn->fd(), events, 0});
      poll_clients.push_back(index);
    }

    int ready = ::poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), protocol::kPollTimeoutMs);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;  // stop flag is checked at the top of the loop
      }
      spdlog::error("gateway: poll() failed: {}", std::strerror(errno));
      continue;
    }
    if (StopRequested()) {
      break;
    }
    if (ready > 0) {
      if ((poll_fds[1].revents & POLLIN) != 0) {
        DrainWakePipe();
      }
      if ((poll_fds[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        AcceptClients();
      }
      for (size_t index = 0; index < poll_clients.size(); ++index) {
        const pollfd& entry = poll_fds[2 + index];
        if (entry.revents == 0) {
          continue;
        }
        Client& client = clients_[poll_clients[index]];
        if (client.dead) {
          continue;
        }
        HandleClientEvents(client, entry.revents);
      }
    }
    CheckTimers();
    CompactClients();
  }
}

void Gateway::DrainWakePipe() {
  char buffer[256];
  while (true) {
    ssize_t n = ::read(wake_fds_[0], buffer, sizeof(buffer));
    if (n > 0) {
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;  // EAGAIN/EWOULDBLOCK: the pipe is drained
  }
}

void Gateway::AcceptClients() {
  while (true) {
    sockaddr_in address {};
    socklen_t address_length = sizeof(address);
    int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&address), &address_length);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        spdlog::error("gateway: accept() failed: {}", std::strerror(errno));
      }
      return;
    }
    net::SetNonBlocking(fd);
    net::SetNoSigpipe(fd);

    int connection_id = next_client_id_++;
    auto conn = std::make_shared<Connection>(fd, connection_id, wake_fds_[1]);
    ex_actor::ActorRef<PlayerActor> actor;
    try {
      std::optional<std::tuple<ex_actor::ActorRef<PlayerActor>>> spawned =
          stdexec::sync_wait(ex_actor::Spawn<PlayerActor>(conn, player_ids_, db_, sessions_, lobby_));
      if (!spawned.has_value()) {
        conn->MarkClosed();
        continue;
      }
      actor = std::get<0>(*spawned);
    } catch (const std::exception& e) {
      spdlog::error("gateway: failed to spawn PlayerActor: {}", e.what());
      conn->MarkClosed();
      continue;
    }
    // The address the actor needs for the asynchronous account results. The fd is not in the
    // poll set yet (poll_fds is built before accept), so no frame can overtake this message.
    ActorSendScope().spawn(actor.SendLocal<&PlayerActor::Dispatch>(PlayerEnvelope::SetSelfRef(actor)));

    Client client;
    client.conn = std::move(conn);
    client.actor = std::move(actor);
    client.last_inbound_ms = clock::MonotonicMs();
    client.idle_reported_for_inbound_ms = -1;  // nothing reported yet for this connection
    clients_.push_back(std::move(client));
    ++accepted_connections_;
    spdlog::info("gateway: accepted conn#{} from {}:{}", connection_id, inet_ntoa(address.sin_addr),
                 ntohs(address.sin_port));
  }
}

void Gateway::HandleClientEvents(Client& client, short revents) {
  if ((revents & POLLOUT) != 0) {
    FlushOutbox(client);
    if (client.dead) {
      return;
    }
  }
  if ((revents & (POLLERR | POLLNVAL)) != 0) {
    CloseClient(client, "socket error");
    return;
  }
  if (client.close_pending) {
    return;  // drain-only; CheckTimers() closes it once flushed or timed out
  }
  if ((revents & POLLIN) != 0) {
    ReadClient(client);
    if (client.dead || client.close_pending) {
      return;
    }
  }
  if ((revents & POLLHUP) != 0) {
    // The peer is gone; flush any final error frame and close.
    client.conn->RequestClose("peer hung up");
  }
}

void Gateway::ReadClient(Client& client) {
  bool peer_closed = false;
  size_t total_read = 0;
  while (total_read < protocol::kMaxReadBytesPerIteration) {
    char buffer[16384];
    ssize_t n = ::read(client.conn->fd(), buffer, sizeof(buffer));
    if (n > 0) {
      client.inbound.append(buffer, static_cast<size_t>(n));
      total_read += static_cast<size_t>(n);
      client.last_inbound_ms = clock::MonotonicMs();
      if (static_cast<size_t>(n) < sizeof(buffer)) {
        break;  // the socket is drained
      }
      continue;
    }
    if (n == 0) {
      peer_closed = true;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    CloseClient(client, std::string("read error: ") + std::strerror(errno));
    return;
  }

  ProcessInboundFrames(client);
  if (client.dead) {
    return;
  }
  if (peer_closed) {
    client.conn->RequestClose("peer closed");
  }
}

void Gateway::ProcessInboundFrames(Client& client) {
  while (!client.dead) {
    const size_t newline = client.inbound.find('\n', client.inbound_begin);
    if (newline == std::string::npos) {
      break;
    }
    std::string line = client.inbound.substr(client.inbound_begin, newline - client.inbound_begin);
    client.inbound_begin = newline + 1;

    if (line.size() > protocol::kMaxFrameBytes) {
      CompactInbound(client);
      client.conn->SendFrameThenClose(protocol::ErrorFrame(protocol::kCodeFrameTooLarge, "frame exceeds 65536 bytes"),
                                      "frame too large");
      return;
    }
    switch (CheckRateLimit(client)) {
      case RateLimitDecision::kClose:
        CompactInbound(client);
        return;
      case RateLimitDecision::kDrop:
        continue;
      case RateLimitDecision::kAccept:
        break;
    }
    if (!utf8::IsValid(line)) {
      // Read-side and parse-side failures share one bad-frame budget (§7.2), so the counter
      // lives on the shared Connection rather than in this loop.
      client.conn->SendFrame(protocol::ErrorFrame(protocol::kCodeBadRequest, "frame is not valid UTF-8"));
      if (client.conn->RecordBadFrame() >= protocol::kMaxBadFrames) {
        CompactInbound(client);
        return;
      }
      continue;
    }
    DispatchFrame(client, std::move(line));
  }

  CompactInbound(client);
  if (client.inbound.size() > protocol::kMaxUnframedBytes) {
    // §1.2.3: no terminator within 64 KB, never buffer without bound.
    client.conn->SendFrameThenClose(
        protocol::ErrorFrame(protocol::kCodeFrameTooLarge, "no frame terminator within 65536 bytes"),
        "unterminated frame too large");
  }
}

void Gateway::CompactInbound(Client& client) {
  if (client.inbound_begin == 0) {
    return;
  }
  client.inbound.erase(0, client.inbound_begin);
  client.inbound_begin = 0;
}

void Gateway::FlushOutbox(Client& client) {
  std::string pending;
  client.conn->DrainOutbox(&pending);
  if (pending.empty()) {
    return;
  }
  size_t written = 0;
  while (written < pending.size()) {
    ssize_t n = ::send(client.conn->fd(), pending.data() + written, pending.size() - written, kSendFlags);
    if (n > 0) {
      written += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    CloseClient(client, std::string("write error: ") + std::strerror(errno));
    return;
  }
  if (written < pending.size()) {
    client.conn->PushBackOutbox(pending.substr(written));
  }
}

RateLimitDecision Gateway::CheckRateLimit(Client& client) {
  int64_t now = clock::MonotonicMs();
  client.frame_times.push_back(now);
  while (!client.frame_times.empty() && client.frame_times.front() <= now - 1000) {
    client.frame_times.pop_front();
  }
  size_t frames_in_window = client.frame_times.size();
  if (frames_in_window <= static_cast<size_t>(protocol::kRateLimitPerSecond)) {
    return RateLimitDecision::kAccept;
  }

  int64_t second = now / 1000;
  if (second == client.limited_second) {
    // still inside the same limited second
  } else if (second == client.limited_second + 1) {
    ++client.consecutive_limited_seconds;
    client.limited_second = second;
  } else {
    client.consecutive_limited_seconds = 1;
    client.limited_second = second;
  }

  // At most one RATE_LIMITED frame per limiting event (§1.7).
  if (client.rate_limit_reported_second != second) {
    client.rate_limit_reported_second = second;
    client.conn->SendFrame(protocol::ErrorFrame(protocol::kCodeRateLimited, "inbound frame rate exceeds 100/s"));
  }

  if (frames_in_window > static_cast<size_t>(protocol::kRateLimitStormPerSecond) ||
      client.consecutive_limited_seconds >= 3) {
    client.conn->RequestClose("rate limit storm");
    return RateLimitDecision::kClose;
  }
  return RateLimitDecision::kDrop;
}

void Gateway::DispatchFrame(Client& client, std::string frame) {
  try {
    ActorSendScope().spawn(
        client.actor.SendLocal<&PlayerActor::Dispatch>(PlayerEnvelope::OnFrame(std::move(frame))));
    ++dispatched_frames_;
  } catch (const std::exception& e) {
    spdlog::error("gateway: failed to dispatch a frame to conn#{}: {}", client.conn->id(), e.what());
    client.conn->RequestClose("dispatch failure");
  }
}

void Gateway::BeginCloseIfRequested(Client& client) {
  if (client.close_pending || !client.conn->close_requested()) {
    return;
  }
  client.close_pending = true;
  client.close_deadline_ms = clock::MonotonicMs() + protocol::kCloseFlushTimeoutMs;
}

void Gateway::CheckTimers() {
  int64_t now = clock::MonotonicMs();
  for (Client& client : clients_) {
    if (client.dead) {
      continue;
    }
    BeginCloseIfRequested(client);
    if (client.close_pending) {
      // Best effort: flush the outbox (e.g. the FRAME_TOO_LARGE frame of §1.2.2) and close.
      if (!client.conn->HasPendingOutbox() || now >= client.close_deadline_ms) {
        CloseClient(client, client.conn->close_reason());
      }
      continue;
    }
    // PROTOCOL.md §1.6: the idle window is measured from the last frame received from this
    // client (any frame counts, heartbeats included), never from when the connection was
    // accepted or from when the previous report was sent. The marker keeps the report unique
    // per silent period: while the client keeps sending frames last_inbound_ms moves on, so
    // the condition can never become true again without a fresh silence window.
    if (now - client.last_inbound_ms >= protocol::kIdleTimeoutMs &&
        client.idle_reported_for_inbound_ms != client.last_inbound_ms) {
      client.idle_reported_for_inbound_ms = client.last_inbound_ms;
      try {
        // PROTOCOL.md §1.6: the lobby drops idle lobby connections, rooms keep the seat
        // entrusted instead.
        ActorSendScope().spawn(client.actor.SendLocal<&PlayerActor::Dispatch>(PlayerEnvelope::OnIdleTimeout()));
      } catch (const std::exception& e) {
        spdlog::error("gateway: failed to dispatch the idle timeout for conn#{}: {}", client.conn->id(), e.what());
      }
    }
  }
}

void Gateway::CloseClient(Client& client, const std::string& reason) {
  if (client.dead) {
    return;
  }
  client.dead = true;
  client.conn->MarkClosed();
  try {
    ActorSendScope().spawn(client.actor.SendLocal<&PlayerActor::Dispatch>(PlayerEnvelope::OnClosed(reason)));
  } catch (const std::exception& e) {
    spdlog::error("gateway: failed to notify conn#{} about the close: {}", client.conn->id(), e.what());
  }
  spdlog::info("gateway: closed conn#{} ({})", client.conn->id(), reason);
}

void Gateway::CompactClients() {
  std::erase_if(clients_, [](const Client& client) { return client.dead; });
}

}  // namespace mahjong
