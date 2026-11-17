#include "gateway.h"

#include <csignal>

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "clock.h"
#include "ex_actor/internal/platform.h"
#include "protocol.h"
#include "utf8.h"

namespace mahjong {
namespace {

volatile sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int signal_number) {
  (void)signal_number;
  g_stop_requested = 1;
}

bool StopRequested() { return g_stop_requested != 0; }

// The idle sweep's period. It only decides how late an idle report can be (§1.6's threshold is
// 20 s, so 250 ms of slack is irrelevant) and how quickly Stop() is noticed.
constexpr int kIdleSweepMs = 250;

// Logs whatever a fire-and-forget coroutine threw instead of dropping it on the floor.
//
// `asio::detached` compiles to an empty handler, so a coroutine spawned with it that throws — a
// `bad_alloc` in the inbound buffer, a failing co_spawn — dies silently: the accept loop would stop
// accepting, a connection would simply stop being served, with nothing in the log. Every
// `co_spawn(..., asio::detached)` in this file therefore uses this instead.
struct LoggedDetached {
  const char* what;  // which coroutine this is, for the log line

  void operator()(std::exception_ptr error) const {
    if (!error) {
      return;
    }
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& e) {
      spdlog::error("gateway: {} coroutine threw: {}", what, e.what());
    } catch (...) {
      spdlog::error("gateway: {} coroutine threw a non-std exception", what);
    }
  }
};

// How many bytes one read may append before the frame handler runs again, so a single greedy peer
// cannot starve the other connections sharing this io thread.
constexpr size_t kMaxReadBytesPerIteration = protocol::kMaxReadBytesPerIteration;

size_t ResolveIoThreads(size_t requested) {
  if (requested != 0) {
    return requested;
  }
  const unsigned hardware = std::thread::hardware_concurrency();
  const size_t cores = hardware == 0 ? 1 : hardware;
  return std::clamp<size_t>(cores, 1, 4);
}

}  // namespace

void RequestGatewayStop() { g_stop_requested = 1; }

void InstallGatewaySignalHandlers(int wake_write_fd) {
  (void)wake_write_fd;  // no self-pipe needed: Run() polls g_stop_requested from its own timer
  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  // SA_RESTART is fine here: nothing blocks in a syscall that must return early. io_context::run()
  // keeps going until its work guard is released, and the idle timer notices the flag.
  action.sa_flags = SA_RESTART;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  ::signal(SIGPIPE, SIG_IGN);
}

Gateway::Gateway(uint16_t port, std::shared_ptr<PlayerIdAllocator> player_ids,
                 std::shared_ptr<SessionRegistry> sessions, ex_actor::ActorRef<LobbyActor> lobby, size_t io_threads)
    : port_(port),
      player_ids_(std::move(player_ids)),
      sessions_(std::move(sessions)),
      lobby_(std::move(lobby)),
      io_threads_(ResolveIoThreads(io_threads)) {}

Gateway::~Gateway() { Stop(); }

bool Gateway::Start(std::string* error) {
  asio::error_code ec;
  const asio::ip::address_v4 loopback = asio::ip::make_address_v4(protocol::kListenAddress, ec);
  if (ec) {
    *error = std::string("invalid listen address: ") + ec.message();
    return false;
  }
  try {
    asio::ip::tcp::endpoint endpoint(loopback, port_);
    acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(io_);
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
      *error = std::string("open() failed: ") + ec.message();
      return false;
    }
    // Tests pass --port 0 and read the bound port back through port(); the other two options make
    // a restart in the same process safe.
    acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
      *error = std::string("setsockopt(SO_REUSEADDR) failed: ") + ec.message();
      return false;
    }
    acceptor_->bind(endpoint, ec);
    if (ec) {
      *error = std::string("bind(") + std::string(protocol::kListenAddress) + ":" + std::to_string(port_) +
               ") failed: " + ec.message();
      return false;
    }
    acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
      *error = std::string("listen() failed: ") + ec.message();
      return false;
    }
    // Report the effective port (useful with --port 0 in tests).
    port_ = acceptor_->local_endpoint(ec).port();
    if (ec) {
      *error = std::string("local_endpoint() failed: ") + ec.message();
      return false;
    }
  } catch (const std::exception& e) {
    *error = std::string("listener setup failed: ") + e.what();
    return false;
  }

  idle_timer_ = std::make_unique<asio::steady_timer>(io_);
  work_guard_.emplace(asio::make_work_guard(io_));
  running_.store(true, std::memory_order_release);
  spdlog::info("gateway: listening on {}:{} (asio, {} io thread(s))", protocol::kListenAddress, port_, io_threads_);
  return true;
}

void Gateway::Run() {
  ex_actor::internal::SetThreadName("gw-run");
  if (!acceptor_) {
    spdlog::error("gateway: Run() called before a successful Start()");
    return;
  }

  // The accept loop runs on its own strand: accepting is one coroutine, but the handlers it
  // touches (registration, the per-client co_spawn) then stay on a single thread by construction.
  asio::co_spawn(asio::make_strand(io_.get_executor()), AcceptLoop(), LoggedDetached{"accept loop"});

  // The idle sweep: one timer instead of a per-client steady_timer, because the sweep has to visit
  // every client anyway (PROTOCOL.md §1.6's report is per connection, not per socket event).
  auto arm_idle_sweep = [this](auto& self) -> void {
    idle_timer_->expires_after(std::chrono::milliseconds(kIdleSweepMs));
    idle_timer_->async_wait([this, &self](const asio::error_code& ec) {
      if (ec == asio::error::operation_aborted || !running_.load(std::memory_order_acquire)) {
        return;
      }
      if (StopRequested()) {
        Stop();
        return;
      }
      CheckIdle();
      self(self);
    });
  };
  arm_idle_sweep(arm_idle_sweep);

  std::vector<std::thread> threads;
  threads.reserve(io_threads_);
  for (size_t i = 1; i < io_threads_; ++i) {
    threads.emplace_back([this] { io_.run(); });
  }
  io_.run();  // this thread is one of them
  for (std::thread& thread : threads) {
    thread.join();
  }
}

void Gateway::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  // Mark every live connection dead so no producer queues another frame into an outbox nobody will
  // drain. MarkClosed is the only connection-side call that is safe from a non-strand thread: it is
  // mutex-guarded and, with owns_fd=false, closes no descriptor.
  //
  // The descriptor itself is left to teardown: closing it from here would race the read/write
  // coroutines that may still be inside asio on another io thread (Stop() is also called from the
  // idle timer), whereas ~Gateway destroys the io_context after every io thread has returned.
  // The consequence worth knowing: PlayerActor::OnClosed does not run on this path, so accounts and
  // seats are released by process exit rather than by the lobby.
  {
    std::lock_guard lock(clients_mutex_);
    for (const std::shared_ptr<Client>& client : clients_) {
      client->conn->MarkClosed();
    }
  }

  // Release the guard first so every io thread returns from run() even when it is idle, then stop
  // the io_context outright (that also cancels the pending accept and timers).
  if (work_guard_.has_value()) {
    work_guard_->reset();
  }
  io_.stop();
  asio::error_code ignored;
  if (acceptor_) {
    acceptor_->close(ignored);
    acceptor_.reset();
  }
  idle_timer_.reset();
}

asio::awaitable<void> Gateway::AcceptLoop() {
  for (;;) {
    asio::error_code ec;
    // One accept per iteration, so the loop yields to the other strand work between connections.
    asio::ip::tcp::socket socket = co_await acceptor_->async_accept(asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
      if (ec == asio::error::operation_aborted || !running_.load(std::memory_order_acquire)) {
        co_return;
      }
      // A transient accept failure (EMFILE, ECONNABORTED) must not kill the loop: the gateway keeps
      // serving the connections it already has.
      spdlog::error("gateway: accept() failed: {}", ec.message());
      continue;
    }

    asio::error_code option_ec;
    socket.set_option(asio::ip::tcp::no_delay(true), option_ec);  // game frames are small and latency-bound
    socket.set_option(asio::socket_base::keep_alive(true), option_ec);

    const int connection_id = next_client_id_.fetch_add(1, std::memory_order_relaxed);
    const int fd = socket.native_handle();
    // asio owns the fd from here on, so Connection must not close it (owns_fd=false): a
    // double close would release a descriptor number asio may already have reused.
    auto conn = std::make_shared<Connection>(fd, connection_id, /*owns_fd=*/false);
    auto client = std::make_shared<Client>(std::move(socket), asio::make_strand(io_.get_executor()));
    client->conn = conn;
    client->last_inbound_ms = clock::MonotonicMs();
    client->first_frame_deadline_ms = client->last_inbound_ms + protocol::kFirstFrameTimeoutMs;

    // The outbox is producer-driven: whoever queues a frame posts a flush onto this connection's
    // strand. Captured weakly so a waker that outlives the client (a queued SendFrame racing a
    // close) cannot resurrect it.
    std::weak_ptr<Client> weak_client = client;
    conn->SetWaker([this, weak_client] {
      if (std::shared_ptr<Client> alive = weak_client.lock()) {
        WakeWriter(alive);
      }
    });

    ex_actor::ActorRef<PlayerActor> actor;
    try {
      std::optional<std::tuple<ex_actor::ActorRef<PlayerActor>>> spawned =
          stdexec::sync_wait(ex_actor::Spawn<PlayerActor>(conn, player_ids_, sessions_, lobby_));
      if (!spawned.has_value()) {
        CloseClient(std::move(client), "failed to spawn PlayerActor");
        continue;
      }
      actor = std::get<0>(*spawned);
    } catch (const std::exception& e) {
      CloseClient(std::move(client), std::string("spawn PlayerActor threw: ") + e.what());
      continue;
    }
    client->actor = std::move(actor);

    // The address the actor needs for asynchronous account results. Queued before the read loop
    // starts, so no frame can overtake it.
    ActorSendScope().spawn(client->actor.Send<&PlayerActor::Dispatch>(PlayerEnvelope::SetSelfRef(client->actor)));

    ++accepted_connections_;
    spdlog::info("gateway: accepted conn#{}", connection_id);

    {
      std::lock_guard lock(clients_mutex_);
      clients_.push_back(client);
    }

    // Hand the connection its own coroutine. co_spawn on the client's strand keeps every touch of
    // socket/inbound/framing state on one thread for the whole life of the connection.
    asio::co_spawn(client->strand, ServeClient(std::move(client)), LoggedDetached{"serve client"});
  }
}

asio::awaitable<void> Gateway::ServeClient(std::shared_ptr<Client> client) {
  co_await ReadLoop(*client);
  // The read side is finished. Two independent things must happen, and conflating them is what
  // made this gateway slow: the actor has to be told *now* (OnClosed is what releases the account
  // and hands the seat to the room — the account_e2e_test reconnects immediately after closing and
  // retries until the seat is free), while the socket has to stay open until the outbox drains (a
  // quota error frame explaining the disconnect must still reach the peer). Notify here, close in
  // the writer.
  NotifyActorClosed(client, client->conn->close_reason());
  BeginClose(std::move(client), "read loop finished");
}

asio::awaitable<void> Gateway::ReadLoop(Client& client) {
  // One read chunk reused across iterations; inbound is the accumulating frame buffer, so a
  // frame larger than the chunk is still reassembled (gateway_smoke_test's split-frame case).
  std::vector<char> chunk(16 * 1024);
  for (;;) {
    asio::error_code ec;
    size_t total_read = 0;
    while (total_read < kMaxReadBytesPerIteration) {
      const size_t n = co_await client.socket.async_read_some(asio::buffer(chunk),
                                                             asio::redirect_error(asio::use_awaitable, ec));
      if (ec) {
        break;
      }
      client.inbound.append(chunk.data(), n);
      total_read += n;
      client.last_inbound_ms = clock::MonotonicMs();
      if (n < chunk.size()) {
        break;  // the socket is drained
      }
    }
    if (ec) {
      if (ec != asio::error::eof && ec != asio::error::operation_aborted) {
        spdlog::info("gateway: conn#{} read ended: {}", client.conn->id(), ec.message());
      }
      // A peer that closes right after sending its last frame still gets those frames processed:
      // the buffered bytes are complete lines far more often than not, and dropping them would
      // lose e.g. the final `ping` of a client that shuts down immediately after it.
      if (ec == asio::error::eof) {
        static_cast<void>(ProcessInboundFrames(client));
      }
      co_return;
    }

    if (!ProcessInboundFrames(client)) {
      co_return;
    }
    if (client.conn->close_requested()) {
      // The error frame is in the outbox and the waker is flushing it; ServeClient's BeginClose
      // then closes once it has drained.
      co_return;
    }
  }
}

bool Gateway::ProcessInboundFrames(Client& client) {
  for (;;) {
    if (client.conn->close_requested()) {
      return false;
    }
    const size_t newline = client.inbound.find('\n', client.inbound_begin);
    if (newline == std::string::npos) {
      break;
    }
    std::string line = client.inbound.substr(client.inbound_begin, newline - client.inbound_begin);
    client.inbound_begin = newline + 1;
    // The slowloris bound only applies until the first complete line arrives; from then on the idle
    // window is the right measure (a logged-in client legitimately goes quiet between frames).
    client.first_frame_deadline_ms = 0;

    if (line.size() > protocol::kMaxFrameBytes) {
      client.inbound.clear();
      client.inbound_begin = 0;
      client.conn->SendFrameThenClose(
          protocol::ErrorFrame(protocol::ErrorCode::kFrameTooLarge, "frame exceeds 65536 bytes"), "frame too large");
      return false;
    }
    switch (CheckRateLimit(client)) {
      case RateLimitDecision::kClose:
        return false;
      case RateLimitDecision::kDrop:
        continue;
      case RateLimitDecision::kAccept:
        break;
    }
    if (!utf8::IsValid(line)) {
      // Read-side and parse-side failures share one bad-frame budget (§7.2), so the counter lives
      // on the shared Connection rather than in this loop.
      client.conn->SendFrame(protocol::ErrorFrame(protocol::ErrorCode::kBadRequest, "frame is not valid UTF-8"));
      if (client.conn->RecordBadFrame() >= protocol::kMaxBadFrames) {
        return false;
      }
      continue;
    }
    DispatchFrame(client, std::move(line));
  }

  // Drop the consumed prefix once, not per frame (§1.2).
  if (client.inbound_begin > 0) {
    client.inbound.erase(0, client.inbound_begin);
    client.inbound_begin = 0;
  }
  if (client.inbound.size() > protocol::kMaxUnframedBytes) {
    // §1.2.3: no terminator within 64 KB, never buffer without bound.
    client.inbound.clear();
    client.conn->SendFrameThenClose(
        protocol::ErrorFrame(protocol::ErrorCode::kFrameTooLarge, "no frame terminator within 65536 bytes"),
        "unterminated frame too large");
    return false;
  }
  return true;
}

RateLimitDecision Gateway::CheckRateLimit(Client& client) {
  const int64_t now = clock::MonotonicMs();
  client.frame_times.push_back(now);
  while (!client.frame_times.empty() && client.frame_times.front() <= now - 1000) {
    client.frame_times.pop_front();
  }
  const size_t frames_in_window = client.frame_times.size();
  if (frames_in_window <= static_cast<size_t>(protocol::kRateLimitPerSecond)) {
    return RateLimitDecision::kAccept;
  }

  const int64_t second = now / 1000;
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
    client.conn->SendFrame(protocol::ErrorFrame(protocol::ErrorCode::kRateLimited, "inbound frame rate exceeds 100/s"));
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
    // Fire and forget: the frame is handed to the actor's own pool and this awaitable never waits
    // for it. This is the whole interop story between the asio half and the stdexec half.
    ActorSendScope().spawn(client.actor.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnFrame(std::move(frame))));
    dispatched_frames_.fetch_add(1, std::memory_order_relaxed);
  } catch (const std::exception& e) {
    spdlog::error("gateway: failed to dispatch a frame to conn#{}: {}", client.conn->id(), e.what());
    client.conn->RequestClose("dispatch failure");
  }
}

void Gateway::WakeWriter(const std::shared_ptr<Client>& client) {
  // Called from any thread (the actor pool). Only the posting is thread-safe; the actual write
  // happens on the client's strand.
  //
  // Coalesced: a room broadcasts to four seats frame by frame, and each of those frames would
  // otherwise cost its own post (and each post that arrives while a write is in flight is a no-op).
  // RunWriter drains whatever has accumulated, so one queued post covers all of them.
  if (client->wake_pending.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  asio::post(client->strand, [this, client] {
    client->wake_pending.store(false, std::memory_order_release);
    RunWriter(client);
  });
}

void Gateway::RunWriter(const std::shared_ptr<Client>& client) {
  // Strand-only. This is the ONE place that decides "write more" vs "close": every path that can
  // change either answer (a producer queueing a frame, the read loop finishing, the close-flush
  // deadline expiring) calls it, and it does its work synchronously on the strand.
  if (client->dead.load(std::memory_order_acquire)) {
    return;
  }
  if (client->conn->close_requested() && !client->close_when_drained) {
    client->close_when_drained = true;
    client->close_deadline_ms = clock::MonotonicMs() + protocol::kCloseFlushTimeoutMs;
  }
  if (client->writing) {
    // A write coroutine owns the socket. It re-enters this function when it finishes — and if it
    // never finishes (the peer stopped reading), its own watchdog in WritePending closes the
    // connection, which is the only way out of this state.
    return;
  }

  std::string pending;
  client->conn->DrainOutbox(&pending);
  if (!pending.empty()) {
    client->writing = true;
    asio::co_spawn(client->strand, WritePending(client, std::move(pending)), LoggedDetached{"writer"});
    return;
  }

  if (!client->close_when_drained) {
    return;  // nothing queued, nothing to do
  }
  if (clock::MonotonicMs() >= client->close_deadline_ms ||
      (client->close_grace_ms != 0 && clock::MonotonicMs() >= client->close_grace_ms)) {
    // Two separate bounds, and this is where the first one lands when the outbox is already empty:
    // the flush deadline (a close that had to wait, e.g. a write that was cancelled by the watchdog)
    // or the short grace period granted for the last frame. Either way: close now.
    CloseClient(client, client->conn->close_reason());
    return;
  }
  // The outbox is empty and a close is pending. An actor that asked for the close has already
  // appended its explaining frame (SendFrameThenClose appends before RequestClose), so give the
  // queue one very short grace period for it to land, then close.
  //
  // The other half of the bound lives on the write path: while a batch is in flight, WriteBudgetMs
  // shrinks each write's timeout to what is left of close_deadline_ms, so this function is not the
  // only thing enforcing kCloseFlushTimeoutMs (and cannot be, since it is never reached while a
  // write is stuck).
  //
  // This waits on a timer owned by the Client, not on a re-posted RunWriter: re-posting spins the
  // io thread for the whole grace period (observed as tens of thousands of iterations in 5 ms,
  // which starved every other connection on that thread — the reason idle connections stopped
  // answering pings). The timer must outlive this function, so it is a member: a local one would be
  // destroyed on return and its async_wait cancelled, leaving the connection open forever.
  if (client->close_grace_ms == 0) {
    client->close_grace_ms = clock::MonotonicMs() + protocol::kCloseGraceMs;
    client->close_timer.expires_after(std::chrono::milliseconds(protocol::kCloseGraceMs));
    client->close_timer.async_wait([this, client](const asio::error_code& ec) {
      if (ec == asio::error::operation_aborted) {
        return;
      }
      RunWriter(client);
    });
  }
}

std::chrono::milliseconds Gateway::WriteBudgetMs(const Client& client) {
  int64_t budget = protocol::kWriteStallTimeoutMs;
  if (client.close_when_drained && client.close_deadline_ms != 0) {
    budget = std::min<int64_t>(budget, client.close_deadline_ms - clock::MonotonicMs());
  }
  // A non-positive value would make cancel_after fire immediately on every write, so never let the
  // deadline collapse to zero: the caller's loop retries and the close path closes instead.
  return std::chrono::milliseconds(std::max<int64_t>(budget, 1));
}

asio::awaitable<void> Gateway::WritePending(std::shared_ptr<Client> client, std::string pending) {
  asio::error_code ec;
  size_t written = 0;
  while (written < pending.size()) {
    // Bounded on purpose: asio operations have no timeout of their own, and `async_write_some`
    // against a peer that stopped reading simply never completes — TCP advertises a zero window and
    // the kernel blocks the write instead of failing it (measured: still pending after 10 s; only a
    // reset makes it return). Without this the write coroutine would pin `writing = true` forever,
    // which strands the read coroutine too, because the only thing that cancels it is the
    // socket.close() inside CloseClient.
    const std::chrono::milliseconds budget = WriteBudgetMs(*client);
    const size_t n = co_await client->socket.async_write_some(
        asio::buffer(pending.data() + written, pending.size() - written),
        asio::redirect_error(asio::cancel_after(budget, asio::use_awaitable), ec));
    if (ec) {
      client->writing = false;
      client->conn->FinishWrite();
      // `dead` means CloseClient already tore this down (gateway Stop, an idle sweep, the read
      // side): nothing left to report.
      if (client->dead.load(std::memory_order_acquire)) {
        co_return;
      }
      // Everything else ends the connection, including our own watchdog. The watchdog is the case
      // that is easy to misread in a log, so it gets its own reason instead of "Operation aborted".
      const bool timed_out = ec == asio::error::operation_aborted;
      CloseClient(std::move(client),
                  timed_out ? "write stalled: peer stopped reading" : "write error: " + ec.message());
      co_return;
    }
    if (n == 0) {
      // A zero-length completion on a non-empty buffer cannot make progress; treating it as success
      // would spin this loop forever.
      client->writing = false;
      client->conn->FinishWrite();
      CloseClient(std::move(client), "write made no progress");
      co_return;
    }
    written += n;
  }
  client->writing = false;
  client->conn->FinishWrite();
  // Back to the single decision point: more queued, or close now?
  asio::post(client->strand, [this, client] { RunWriter(client); });
  co_return;
}

void Gateway::BeginClose(std::shared_ptr<Client> client, std::string reason) {
  // Strand-only. CloseClient is what sends OnClosed, so this only records that a close is coming
  // and wakes the writer.
  if (client->dead.load(std::memory_order_acquire)) {
    return;
  }
  if (!client->close_when_drained) {
    client->close_when_drained = true;
    client->close_deadline_ms = clock::MonotonicMs() + protocol::kCloseFlushTimeoutMs;
  }
  // The grace period RunWriter grants here is the window for frames the close itself causes to
  // land: ServeClient has already sent OnClosed, and an actor answering it (a seat-release notice,
  // for instance) gets protocol::kCloseGraceMs to queue that answer. It is deliberately short —
  // releasing the account and handing the seat to the room matters more than a courtesy frame, and
  // account_e2e_test closes and relogs in a loop, so a long window would show up as a retry storm.
  WakeWriter(client);
}

void Gateway::CheckIdle() {
  // Walks the live clients through a snapshot, because CloseClient mutates the registry. The
  // snapshot is taken under the registry mutex; the per-client work below happens outside it.
  std::vector<std::shared_ptr<Client>> snapshot;
  {
    std::lock_guard lock(clients_mutex_);
    snapshot.reserve(clients_.size());
    for (const std::shared_ptr<Client>& client : clients_) {
      snapshot.push_back(client);
    }
  }
  const int64_t now = clock::MonotonicMs();
  for (const std::shared_ptr<Client>& client : snapshot) {
    // This runs on the timer's thread, so it must not touch Client's strand-owned fields
    // (dead / close_when_drained / last_inbound_ms / close_deadline_ms). Only the shared
    // Connection's accessors are safe here; everything else is posted onto the client's strand.
    //
    // A connection that has asked to close is NOT skipped: the writer normally finishes it, but a
    // writer wedged on an unreadable peer would never get there, and this sweep is the backstop
    // that closes it (see ReportIdleIfDue).
    //
    // One post per connection per tick at most: a strand that is busy with a big write must not
    // accumulate idle checks behind it.
    if (client->idle_check_pending.exchange(true, std::memory_order_acq_rel)) {
      continue;
    }
    asio::post(client->strand, [this, client, now] {
      client->idle_check_pending.store(false, std::memory_order_release);
      ReportIdleIfDue(client, now);
    });
  }
}

void Gateway::ReportIdleIfDue(const std::shared_ptr<Client>& client, int64_t now) {
  if (client->dead.load(std::memory_order_acquire)) {
    return;
  }
  if (client->conn->close_requested()) {
    // Backstop for the close path: the writer owns the normal route (drain, grace, then close),
    // and this only fires if that route never runs — the sweep's whole purpose is that no
    // connection can stay alive just because no event ever arrives for it.
    if (client->close_deadline_ms != 0 && now >= client->close_deadline_ms) {
      CloseClient(client, client->conn->close_reason());
    }
    return;
  }
  // Slowloris bound before the idle window: see first_frame_deadline_ms.
  if (client->first_frame_deadline_ms != 0 && now >= client->first_frame_deadline_ms) {
    CloseClient(client, "no complete frame within the first-frame timeout");
    return;
  }
  if (now - client->last_inbound_ms < protocol::kIdleTimeoutMs ||
      client->idle_reported_for_inbound_ms == client->last_inbound_ms) {
    return;
  }
  client->idle_reported_for_inbound_ms = client->last_inbound_ms;
  try {
    // §1.6: the lobby drops idle lobby connections, rooms keep the seat entrusted instead.
    ActorSendScope().spawn(client->actor.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnIdleTimeout()));
  } catch (const std::exception& e) {
    spdlog::error("gateway: failed to dispatch the idle timeout for conn#{}: {}", client->conn->id(), e.what());
  }
}

void Gateway::NotifyActorClosed(const std::shared_ptr<Client>& client, const std::string& reason) {
  if (client->actor_closed_notified) {
    return;
  }
  client->actor_closed_notified = true;
  // Accept-time failures (a spawn that returned nothing or threw) close the client before there is
  // an actor to tell. Sending to an empty ref throws, which would turn a plain "no actor was ever
  // created" into a logged error, so check first.
  if (client->actor.IsEmpty()) {
    return;
  }
  try {
    // This is what releases the account (ACCOUNT_DESIGN §1.3 "one account, one live connection")
    // and hands the seat to the room, so it must not wait for the socket to be torn down: a client
    // that reconnects right after closing (account_e2e_test) retries until the seat is free.
    ActorSendScope().spawn(client->actor.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnClosed(reason)));
  } catch (const std::exception& e) {
    spdlog::error("gateway: failed to notify conn#{} about the close: {}", client->conn->id(), e.what());
  }
}

void Gateway::CloseClient(std::shared_ptr<Client> client, const std::string& reason) {
  if (client->dead.load(std::memory_order_acquire)) {
    return;
  }
  client->dead.store(true, std::memory_order_release);
  {
    std::lock_guard lock(clients_mutex_);
    std::erase(clients_, client);
  }
  // Cancel the grace timer: its handler would otherwise run after the socket is gone, and the timer
  // holds a shared_ptr to this Client, so leaving it armed keeps the whole connection (buffers,
  // actor ref) alive until it fires.
  client->close_timer.cancel();
  asio::error_code ignored;
  // MarkClosed only marks (owns_fd is false): asio owns the descriptor, and closing it twice could
  // release a number asio has already reused.
  client->conn->MarkClosed();
  // Closing the socket is what unblocks a read or write coroutine still parked inside asio on this
  // descriptor (they complete with operation_aborted), which is why it happens before the notify.
  client->socket.close(ignored);
  NotifyActorClosed(client, reason);
  spdlog::info("gateway: closed conn#{} ({})", client->conn->id(), reason);
}

}  // namespace mahjong
