// mahjong server entry point: TCP JSON gateway + actor world (milestone: one room, one
// round, one human plus three bots).
//
// Usage: mahjong_server [--port N] [--seed N] [--action-timeout-ms N] [--ai-delay-ms N]

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "clock.h"
#include "db.h"
#include "ex_actor/api.h"
#include "gateway.h"
#include "lobby.h"
#include "player.h"
#include "protocol.h"
#include "timer.h"

namespace {

constexpr uint32_t kThreadPoolSize = 4;

// Account-storage shards. A MySQL connection is thread-scoped, so this is also the number of
// dedicated db threads (and the DB's concurrency ceiling). Four is deliberately modest: the
// bottleneck for this workload is the server, not the client, and every shard costs a thread.
constexpr int kDbShardCount = 4;

struct Options {
  uint16_t port = mahjong::protocol::kDefaultPort;
  uint64_t seed = 0;
  int action_timeout_ms = mahjong::protocol::kDefaultActionTimeoutMs;
  int ai_delay_ms = mahjong::kDefaultAiDelayMs;
  // v1.6（一场四局）: the pause between two rounds of one match.
  int inter_hand_delay_ms = mahjong::kDefaultInterHandDelayMs;
  // ACCOUNT_DESIGN.md §6.5: never a password in the source — the empty default is filled from
  // the environment (or --mysql-password) so it stays out of the argv of the process table.
  mahjong::DbConfig db;
  bool show_help = false;
};

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--port N] [--seed N] [--action-timeout-ms N] [--ai-delay-ms N]\n"
               "          [--mysql-host H] [--mysql-port N] [--mysql-user U] [--mysql-password P] [--mysql-db D]\n"
               "  --port N               listen port (default %u)\n"
               "  --seed N               wall shuffle seed (default: derived from the clock)\n"
               "  --action-timeout-ms N  action window length, %d..%d (default %d)\n"
               "  --ai-delay-ms N        delay before an AI decision, 0..%d (default %d; 0 = immediate)\n"
               "  --inter-hand-delay-ms N pause between rounds of a match, 0..%d (default %d)\n"
               "  --mysql-host H         MySQL host for accounts (default %s)\n"
               "  --mysql-port N         MySQL port (default %d)\n"
               "  --mysql-user U         MySQL user (default %s)\n"
               "  --mysql-password P     MySQL password (default: $MAHJONG_MYSQL_PASSWORD)\n"
               "  --mysql-db D           database, created if missing (default %s)\n"
               "  accounts are optional: if MySQL is unreachable the server starts anyway and only\n"
               "  the account frames report an error (the game itself is unaffected)\n",
               program, mahjong::protocol::kDefaultPort, mahjong::protocol::kMinActionTimeoutMs,
               mahjong::protocol::kMaxActionTimeoutMs, mahjong::protocol::kDefaultActionTimeoutMs,
               mahjong::kMaxAiDelayMs, mahjong::kDefaultAiDelayMs, mahjong::kMaxInterHandDelayMs,
               mahjong::kDefaultInterHandDelayMs, mahjong::DbConfig {}.host.c_str(),
               mahjong::DbConfig {}.port, mahjong::DbConfig {}.user.c_str(), mahjong::DbConfig {}.db.c_str());
}

// Splits "--key=value" into its two halves.
bool SplitArgument(const std::string& argument, std::string* key, std::string* inline_value, bool* has_inline_value) {
  size_t equals = argument.find('=');
  if (equals == std::string::npos) {
    *key = argument;
    inline_value->clear();
    *has_inline_value = false;
    return true;
  }
  *key = argument.substr(0, equals);
  *inline_value = argument.substr(equals + 1);
  *has_inline_value = true;
  return true;
}

bool ParseInt64(const std::string& text, int64_t* out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  long long value = std::strtoll(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    return false;
  }
  *out = static_cast<int64_t>(value);
  return true;
}

bool IsKnownOption(const std::string& key) {
  return key == "--port" || key == "--seed" || key == "--action-timeout-ms" || key == "--ai-delay-ms" ||
         key == "--inter-hand-delay-ms" ||
         key == "--mysql-host" || key == "--mysql-port" || key == "--mysql-user" || key == "--mysql-password" ||
         key == "--mysql-db";
}

std::optional<Options> ParseOptions(int argc, char** argv, std::string* error) {
  Options options;
  const char* env_password = std::getenv("MAHJONG_MYSQL_PASSWORD");
  if (env_password != nullptr) {
    options.db.password = env_password;
  }
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    std::string key;
    std::string inline_value;
    bool has_inline_value = false;
    SplitArgument(argument, &key, &inline_value, &has_inline_value);
    if (key == "--help" || key == "-h") {
      options.show_help = true;
      continue;
    }
    if (!IsKnownOption(key)) {
      *error = "unknown argument: " + argument;
      return std::nullopt;
    }
    std::string value;
    if (has_inline_value) {
      value = inline_value;
    } else if (i + 1 < argc) {
      value = argv[++i];
    } else {
      *error = "missing value for " + key;
      return std::nullopt;
    }
    // The connection options are free-form strings; everything else is an integer.
    if (key == "--mysql-host") {
      options.db.host = value;
      continue;
    }
    if (key == "--mysql-user") {
      options.db.user = value;
      continue;
    }
    if (key == "--mysql-password") {
      options.db.password = value;
      continue;
    }
    if (key == "--mysql-db") {
      options.db.db = value;
      continue;
    }

    int64_t number = 0;
    if (!ParseInt64(value, &number)) {
      *error = key + " expects an integer, got: " + value;
      return std::nullopt;
    }
    if (key == "--port") {
      if (number < 1 || number > 65535) {
        *error = "--port must be within 1..65535, got: " + value;
        return std::nullopt;
      }
      options.port = static_cast<uint16_t>(number);
    } else if (key == "--seed") {
      if (number < 0) {
        *error = "--seed must be non-negative, got: " + value;
        return std::nullopt;
      }
      options.seed = static_cast<uint64_t>(number);
    } else if (key == "--action-timeout-ms") {
      if (number < mahjong::protocol::kMinActionTimeoutMs || number > mahjong::protocol::kMaxActionTimeoutMs) {
        *error = "--action-timeout-ms must be within " + std::to_string(mahjong::protocol::kMinActionTimeoutMs) + ".." +
                 std::to_string(mahjong::protocol::kMaxActionTimeoutMs) + ", got: " + value;
        return std::nullopt;
      }
      options.action_timeout_ms = static_cast<int>(number);
    } else if (key == "--mysql-port") {
      if (number < 1 || number > 65535) {
        *error = "--mysql-port must be within 1..65535, got: " + value;
        return std::nullopt;
      }
      options.db.port = static_cast<int>(number);
    } else if (key == "--inter-hand-delay-ms") {  // v1.6（C7）：局间停顿
      if (number < 0 || number > mahjong::kMaxInterHandDelayMs) {
        *error = "--inter-hand-delay-ms must be within 0.." + std::to_string(mahjong::kMaxInterHandDelayMs) +
                 ", got: " + value;
        return std::nullopt;
      }
      options.inter_hand_delay_ms = static_cast<int>(number);
    } else {  // --ai-delay-ms
      if (number < 0 || number > mahjong::kMaxAiDelayMs) {
        *error = "--ai-delay-ms must be within 0.." + std::to_string(mahjong::kMaxAiDelayMs) + ", got: " + value;
        return std::nullopt;
      }
      options.ai_delay_ms = static_cast<int>(number);
    }
  }
  if (options.seed == 0) {
    // Derived from the clock so every run differs unless --seed is given (§1.8).
    options.seed = static_cast<uint64_t>(mahjong::clock::EpochMs()) ^ 0x5DEECE66Dull;
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  std::string error;
  std::optional<Options> options = ParseOptions(argc, argv, &error);
  if (!options.has_value()) {
    std::fprintf(stderr, "mahjong_server: %s\n", error.c_str());
    PrintUsage(argv[0]);
    return 2;
  }
  if (options->show_help) {
    PrintUsage(argv[0]);
    return 0;
  }

  // Ignore SIGPIPE up front: a peer that goes away must never kill the server.
  std::signal(SIGPIPE, SIG_IGN);
  ex_actor::ConfigureLogging();
  spdlog::info(
      "mahjong_server: starting (port={}, seed={}, action_timeout_ms={}, ai_delay_ms={}, thread_pool={}, db_shards={})",
      options->port, options->seed, options->action_timeout_ms, options->ai_delay_ms, kThreadPoolSize, kDbShardCount);

  // ex_actor must be initialized explicitly and shut down before main returns.
  // GAME_DESIGN v1.4 §5.4: rooms read the AI pacing from the process-wide setting (the
  // lobby spawns them without carrying it).
  mahjong::SetAiActionDelayMs(options->ai_delay_ms);
  // v1.6（一场四局）：局间停顿同理走进程级设置
  mahjong::SetInterHandDelayMs(options->inter_hand_delay_ms);

  // Account storage (ACCOUNT_DESIGN.md §2). MySQL blocks, so it never runs on a game pool. Each
  // shard is one DbActor on its own single-thread pool: a connection is thread-scoped, so the
  // connection count and the thread count are the same number, and each shard's mailbox keeps
  // the FIFO guarantee §4.1 needs. DbService spreads requests over the shards by key, so one
  // account's requests — a match record and the stats request that must include it — land on the
  // same connection and stay ordered.
  auto game_pool = std::make_unique<ex_actor::WorkSharingThreadPool>(kThreadPoolSize);
  std::vector<std::unique_ptr<ex_actor::WorkSharingThreadPool>> db_pools;
  std::vector<ex_actor::WorkSharingThreadPool::Scheduler> schedulers {game_pool->GetScheduler()};
  for (int i = 0; i < kDbShardCount; ++i) {
    db_pools.push_back(std::make_unique<ex_actor::WorkSharingThreadPool>(1));
    schedulers.push_back(db_pools.back()->GetScheduler());
  }
  auto pool_union = std::make_unique<ex_actor::SchedulerUnion<ex_actor::WorkSharingThreadPool::Scheduler>>(
      std::move(schedulers));

  ex_actor::Init(pool_union->GetScheduler());
  // The registry holds the scheduler by raw pointers, so the pools and the union must outlive it.
  ex_actor::HoldResource(std::move(game_pool));
  for (auto& db_pool : db_pools) {
    ex_actor::HoldResource(std::move(db_pool));
  }
  ex_actor::HoldResource(std::move(pool_union));

  // mysql_library_init is process-wide and not thread safe, so it happens once, here, before any
  // db thread exists. Each db thread registers itself separately in DbActor::Initialize.
  static_cast<void>(mahjong::DbLibraryInit());

  // Everything that needs to be spawned, and the startup probe, runs in one coroutine so the
  // DbActor refs exist before the lobby and the gateway are built. The probes are sequential, so
  // the schema DDL is never issued by two shards at once.
  mahjong::DbService db;
  {
    auto startup = [&]() -> exec::task<void> {
      for (int i = 0; i < kDbShardCount; ++i) {
        ex_actor::ActorConfig config {.scheduler_index = static_cast<size_t>(i) + 1};
        db.AddShard(co_await ex_actor::Spawn<mahjong::DbActor>(config, options->db));
      }
      // Probe on the shard's own thread — that thread owns its connection (db.h). A dead MySQL
      // is not fatal: the server starts and only the account frames answer INTERNAL (§2.3).
      // Initialize() logs the outcome itself.
      for (const auto& shard : db.shards()) {
        static_cast<void>(co_await shard.Send<&mahjong::DbActor::Initialize>());
      }
    };
    stdexec::sync_wait(startup());
  }
  // Drains every shard's mailbox before the world goes away: the writes §4.1 ordered must land.
  auto stop_db = [&db] {
    for (const auto& shard : db.shards()) {
      stdexec::sync_wait(shard.Send<&mahjong::DbActor::Stop>());
    }
  };

  // Window timers: the delayed, seq-carrying timeout messages (PROTOCOL.md §0.5).
  auto window_timer = std::make_shared<mahjong::WindowTimer>();

  auto lobby_result = stdexec::sync_wait(ex_actor::Spawn<mahjong::LobbyActor>(
      options->seed, options->action_timeout_ms, window_timer, mahjong::kGameEndReclaimMs, db));
  if (!lobby_result.has_value()) {
    spdlog::error("mahjong_server: failed to spawn LobbyActor");
    window_timer->Stop();
    stop_db();
    stdexec::sync_wait(mahjong::ActorSendScope().on_empty());
    // Every db thread has unregistered itself in DbActor::Stop, so the library can go away now.
  mahjong::DbLibraryShutdown();
  ex_actor::Shutdown();
    return 1;
  }
  ex_actor::ActorRef<mahjong::LobbyActor> lobby = std::get<0>(*lobby_result);
  // The lobby schedules its own post-game reclaim messages (PROTOCOL.md §1.4.5), so it needs
  // its own address. Enqueued before the gateway can accept anything: the room registry is
  // empty either way, so the ordering is inconsequential.
  mahjong::ActorSendScope().spawn(lobby.SendLocal<&mahjong::LobbyActor::SetSelfRef>(lobby));

  auto player_ids = std::make_shared<mahjong::PlayerIdAllocator>();
  // ACCOUNT_DESIGN.md §1.3: "one account, one live connection", enforced across all
  // connections of this process.
  auto sessions = std::make_shared<mahjong::SessionRegistry>();
  mahjong::Gateway gateway(options->port, player_ids, db, sessions, lobby);
  if (!gateway.Start(&error)) {
    spdlog::error("mahjong_server: {}", error);
    window_timer->Stop();
    stop_db();
    stdexec::sync_wait(mahjong::ActorSendScope().on_empty());
    // Every db thread has unregistered itself in DbActor::Stop, so the library can go away now.
  mahjong::DbLibraryShutdown();
  ex_actor::Shutdown();
    return 1;
  }
  mahjong::InstallGatewaySignalHandlers(gateway.wake_write_fd());
  gateway.Run();  // returns on SIGINT/SIGTERM
  spdlog::info("mahjong_server: stopping (accepted={}, frames={})", gateway.accepted_connections(),
               gateway.dispatched_frames());
  gateway.Stop();

  // Stop the timer before the actor world goes away: pending timeout actions post messages.
  window_timer->Stop();

  // Same rule for the database: it is an actor, but its results and match records are posted
  // into other actors, so it must be drained before the world is torn down. Awaiting Stop()
  // also waits for its mailbox, which is the FIFO drain the writes need.
  stop_db();

  // Drain every fire-and-forget handler chain before tearing the actor world down. Both external
  // senders are already stopped, so nothing new can appear and this is guaranteed to finish.
  // Needed because a handler parked in a `co_await` (the room fans broadcasts out to four seats)
  // outlives its activation, and `ex_actor::Shutdown()` only waits for activations.
  stdexec::sync_wait(mahjong::ActorSendScope().on_empty());

  // Shutdown is required before main exits (api.h).
  // Every db thread has unregistered itself in DbActor::Stop, so the library can go away now.
  mahjong::DbLibraryShutdown();
  ex_actor::Shutdown();
  spdlog::info("mahjong_server: bye");
  return 0;
}
