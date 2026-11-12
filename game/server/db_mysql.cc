// The MySQL-backed DbActor (ACCOUNT_DESIGN.md §2), and the connection pool behind it (§2.4).
//
// Shape: DbActor no longer owns a connection. A handler takes a DbLease from the process-wide
// DbPool, runs its statements on it and lets the lease return it; the pool owns connect and
// reconnect, the idle ping, rotation, the circuit breaker and the counters. Each shard still runs
// on its own single-thread pool, so a handler blocks exactly one dedicated thread and nothing
// else, and because that thread drains its mailbox in order the FIFO property §4.1 depends on (a
// match record is always ahead of the stats request that must include it) comes for free. The
// connection may be swapped between two requests without disturbing that ordering: what
// serializes them is the mailbox, not the socket.
//
// Password storage (§2.2, demo-grade by explicit decision): a per-account 16-byte random salt
// plus a single SHA-256, computed by MySQL's SHA2() so the server needs no crypto library.
// Swapping both SQL expressions for libcrypto's PBKDF2 is the documented upgrade path.

#include <mysql.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "clock.h"
#include "db.h"
#include "db_pool.h"
#include "player_actor.h"
#include "seat_endpoint.h"

namespace mahjong {
namespace {

// mysql_library_init/end are process-wide and not thread safe, so they are done exactly once by
// whoever gets here first (main() at startup, or the first DbActor::Initialize as a backstop).
std::once_flag g_library_once;
bool g_library_ready = false;

}  // namespace

bool DbLibraryInit() {
  std::call_once(g_library_once, [] { g_library_ready = (mysql_library_init(0, nullptr, nullptr) == 0); });
  return g_library_ready;
}

void DbLibraryShutdown() {
  if (g_library_ready) {
    mysql_library_end();
    g_library_ready = false;
  }
}

namespace {

// The first run of a server points at a database that does not exist yet; this is the one
// connect error the pool answers by retrying without a database (see DbPool::Impl::Connect).
constexpr unsigned int kErrBadDb = 1049;

// The C API's MYSQL_OPT_*_TIMEOUT options are in seconds while the CLI speaks milliseconds.
// Rounding up keeps a 100ms request from turning into "no timeout".
unsigned int ToTimeoutSeconds(int timeout_ms) {
  const int clamped = std::max(1, timeout_ms);
  return static_cast<unsigned int>((clamped + 999) / 1000);
}

// mysql_errno() == 0 means "no error recorded", which after a failed call is still a failure, not
// a success. Callers charge it as a per-statement error so it never feeds the circuit breaker.
DbFailureClass ClassifyOrQueryError(MYSQL* mysql) {
  const DbFailureClass reason = ClassifyMysqlError(mysql_errno(mysql));
  return reason == DbFailureClass::kNone ? DbFailureClass::kQueryError : reason;
}

// 16 random bytes as 32 hex characters (§2.2).
std::string RandomSalt() {
  std::random_device device;
  std::uniform_int_distribution<unsigned int> byte(0, 255);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string salt;
  salt.reserve(32);
  for (int i = 0; i < 16; ++i) {
    unsigned int value = byte(device);
    salt.push_back(kHex[value >> 4]);
    salt.push_back(kHex[value & 0x0F]);
  }
  return salt;
}

// The database name is interpolated into DDL (no placeholders exist there), so it is
// restricted to a plain identifier.
bool IsValidDatabaseName(const std::string& name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (char c : name) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum && c != '_') {
      return false;
    }
  }
  return true;
}

uint64_t ParseUint(const char* text) {
  if (text == nullptr) {
    return 0;
  }
  return std::strtoull(text, nullptr, 10);
}

int ParseInt(const char* text) {
  if (text == nullptr) {
    return 0;
  }
  return static_cast<int>(std::strtol(text, nullptr, 10));
}

// Posts an account outcome into the requesting PlayerActor's mailbox. The result must
// arrive as a message, not as a return value: PlayerActor's handlers never suspend
// (player_actor.h), so there is nothing there to await.
void ReplyAuth(const ex_actor::ActorRef<PlayerActor>& reply, AuthOutcome outcome) {
  if (reply.IsEmpty()) {
    return;  // the connection went away while the query ran; nobody is left to answer
  }
  ActorSendScope().spawn(reply.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnLoginResult(std::move(outcome))));
}

void ReplyRegister(const ex_actor::ActorRef<PlayerActor>& reply, AuthOutcome outcome) {
  if (reply.IsEmpty()) {
    return;
  }
  ActorSendScope().spawn(reply.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnRegisterResult(std::move(outcome))));
}

void ReplyStats(const ex_actor::ActorRef<PlayerActor>& reply, StatsOutcome outcome) {
  if (reply.IsEmpty()) {
    return;
  }
  ActorSendScope().spawn(reply.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnStatsResult(std::move(outcome))));
}

}  // namespace

// ---- DbPool (ACCOUNT_DESIGN.md §2.4) ------------------------------------------------------
//
// Two definitions of DbPool::Impl exist in the tree: this one and the empty one in db_stub.cc.
// CMake compiles exactly one of the two files into mahjong_db, so they never meet in a binary —
// the same arrangement DbActor's method bodies already use.

struct DbPool::Impl {
  // One slot per db thread. The plain fields are touched only by the owning thread; the atomics
  // below them are additionally read by main() at shutdown (DbPool::Stats).
  struct Slot {
    size_t index = 0;
    bool leased = false;
    std::thread::id owner {};
    MYSQL* handle = nullptr;
    int64_t connected_at_ms = 0;
    int64_t last_used_ms = 0;
    FailureBudget budget;

    // Relaxed: the owner thread is the only writer, and the reader wants a rough total.
    std::atomic<uint64_t> acquires {0};
    std::atomic<uint64_t> acquire_failures {0};
    std::atomic<uint64_t> circuit_fast_fails {0};
    std::atomic<uint64_t> connects {0};
    std::atomic<uint64_t> connect_failures {0};
    std::atomic<uint64_t> pings {0};
    std::atomic<uint64_t> ping_failures {0};
    std::atomic<uint64_t> recycled_lifetime {0};
    std::atomic<uint64_t> recycled_idle {0};
    std::atomic<uint64_t> closed_broken {0};
    std::atomic<bool> connected {false};
    std::atomic<uint32_t> consecutive_failures {0};
    std::atomic<int64_t> cooldown_until_ms {0};
  };

  Impl(DbConfig connection_config, DbPoolOptions pool_options)
      : connection(std::move(connection_config)), options(pool_options), ownership(pool_options.slot_count) {
    slots.reserve(options.slot_count);
    for (size_t index = 0; index < options.slot_count; ++index) {
      auto slot = std::make_unique<Slot>();
      slot->index = index;
      // A default-constructed budget would silently ignore --mysql-failure-threshold and
      // --mysql-cooldown-ms: the breaker would always run with the library defaults.
      slot->budget = FailureBudget(options.failure_budget);
      slots.push_back(std::move(slot));
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  // Mirror the breaker's state into the atomics main() reads at shutdown.
  static void SyncBudget(Slot* slot) {
    slot->consecutive_failures.store(slot->budget.consecutive_failures(), std::memory_order_relaxed);
    slot->cooldown_until_ms.store(slot->budget.cooldown_until_ms(), std::memory_order_relaxed);
  }

  // Charge one failure and report the trip at the moment it happens, whichever path found it
  // (a failed connect or a statement that killed the socket).
  static void ChargeFailure(Slot* slot, int64_t now_ms, DbFailureClass reason) {
    slot->budget.OnFailure(now_ms);
    SyncBudget(slot);
    if (slot->budget.circuit_open(now_ms)) {
      spdlog::warn("db: slot {} opened its circuit for {}ms after a {} error", slot->index,
                   slot->budget.cooldown_until_ms() - now_ms, ToString(reason));
    }
  }

  static void CloseHandle(Slot* slot) {
    if (slot->handle != nullptr) {
      mysql_close(slot->handle);
      slot->handle = nullptr;
    }
    slot->connected.store(false, std::memory_order_relaxed);
  }

  MYSQL* OpenConnection(const char* database, unsigned int* error) {
    MYSQL* handle = mysql_init(nullptr);
    if (handle == nullptr) {
      *error = 0;
      spdlog::error("db: mysql_init failed");
      return nullptr;
    }
    const unsigned int connect_timeout = ToTimeoutSeconds(options.connect_timeout_ms);
    const unsigned int read_timeout = ToTimeoutSeconds(options.read_timeout_ms);
    const unsigned int write_timeout = ToTimeoutSeconds(options.write_timeout_ms);
    mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
    mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
    mysql_options(handle, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    // MySQL 8's default auth plugin is caching_sha2_password, whose full-authentication path
    // refuses to send the password over an unencrypted TCP connection unless the client is
    // allowed to fetch the server's RSA public key. Without this, every connection fails with
    // "Authentication requires secure connection" and accounts silently degrade to unavailable —
    // a nasty thing to debug, so the option is opt-in here rather than left to the default.
    bool get_server_public_key = true;
    mysql_options(handle, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &get_server_public_key);

    // Reconnection is the pool's job. An automatic one would hide the failure from the lease
    // owner and could resurrect a connection the breaker already wrote off.
    bool reconnect = false;
    mysql_options(handle, MYSQL_OPT_RECONNECT, &reconnect);

    if (mysql_real_connect(handle, connection.host.c_str(), connection.user.c_str(), connection.password.c_str(),
                           database, static_cast<unsigned int>(connection.port), nullptr, 0) == nullptr) {
      *error = mysql_errno(handle);
      spdlog::error("db: cannot connect to {}:{} (database '{}'): {}", connection.host, connection.port,
                    database == nullptr ? "<none>" : database, mysql_error(handle));
      mysql_close(handle);
      return nullptr;
    }
    *error = 0;
    return handle;
  }

  bool Connect(Slot* slot, int64_t now_ms) {
    slot->connects.fetch_add(1, std::memory_order_relaxed);
    unsigned int error = 0;
    MYSQL* handle = OpenConnection(connection.db.c_str(), &error);
    if (handle == nullptr && error == kErrBadDb) {
      // First run: the database does not exist yet. Connect without one and let the caller's
      // EnsureSchema() create and select it on this very lease.
      spdlog::info("db: database '{}' does not exist yet; connecting without one to create it", connection.db);
      handle = OpenConnection(nullptr, &error);
    }
    if (handle == nullptr) {
      slot->connect_failures.fetch_add(1, std::memory_order_relaxed);
      // A bad password or an unknown database classifies as kConfig, a refused socket as
      // kConnectFailed; both mean "this shard cannot serve", so both feed the breaker.
      DbFailureClass reason = ClassifyMysqlError(error);
      if (!CountsAsUnavailable(reason)) {
        reason = DbFailureClass::kConnectFailed;
      }
      ChargeFailure(slot, now_ms, reason);
      return false;
    }
    slot->handle = handle;
    slot->connected_at_ms = now_ms;
    slot->last_used_ms = now_ms;
    slot->connected.store(true, std::memory_order_relaxed);
    return true;
  }

  bool EnsureUsableHandle(Slot* slot, int64_t now_ms) {
    if (slot->handle != nullptr) {
      const bool lifetime_over =
          options.max_lifetime_ms > 0 && now_ms - slot->connected_at_ms >= options.max_lifetime_ms;
      const bool idle_over = options.idle_timeout_ms > 0 && now_ms - slot->last_used_ms >= options.idle_timeout_ms;
      if (lifetime_over || idle_over) {
        if (lifetime_over) {
          slot->recycled_lifetime.fetch_add(1, std::memory_order_relaxed);
        }
        if (idle_over) {
          slot->recycled_idle.fetch_add(1, std::memory_order_relaxed);
        }
        CloseHandle(slot);
      }
    }
    if (slot->handle == nullptr) {
      return Connect(slot, now_ms);
    }
    if (options.ping_after_idle_ms > 0 && now_ms - slot->last_used_ms >= options.ping_after_idle_ms) {
      // The only cheap way to tell a healthy idle socket from one the server already dropped.
      slot->pings.fetch_add(1, std::memory_order_relaxed);
      if (mysql_ping(slot->handle) != 0) {
        slot->ping_failures.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("db: slot {} lost its connection while idle ({}); reconnecting", slot->index,
                     mysql_error(slot->handle));
        CloseHandle(slot);
        return Connect(slot, now_ms);
      }
    }
    return true;
  }

  DbConfig connection;
  DbPoolOptions options;
  SlotOwnership ownership;
  std::vector<std::unique_ptr<Slot>> slots;
  std::atomic<uint64_t> bound_slots {0};

  // The slot this thread bound itself to. A thread_local pointer is what keeps Acquire() lock
  // free: there is exactly one slot a thread is ever allowed to touch.
  static inline thread_local Slot* current_slot = nullptr;
};

DbLease::DbLease(DbPool* pool, size_t slot_index, void* handle)
    : pool_(pool), slot_index_(slot_index), handle_(handle) {}

DbLease::~DbLease() { Reset(); }

DbLease::DbLease(DbLease&& other) noexcept
    : pool_(other.pool_),
      slot_index_(other.slot_index_),
      handle_(other.handle_),
      failure_(other.failure_),
      broken_reason_(other.broken_reason_) {
  other.pool_ = nullptr;
  other.handle_ = nullptr;
  other.failure_ = DbAcquireFailure::kNone;
  other.broken_reason_ = DbFailureClass::kNone;
}

DbLease& DbLease::operator=(DbLease&& other) noexcept {
  if (this != &other) {
    Reset();
    pool_ = other.pool_;
    slot_index_ = other.slot_index_;
    handle_ = other.handle_;
    failure_ = other.failure_;
    broken_reason_ = other.broken_reason_;
    other.pool_ = nullptr;
    other.handle_ = nullptr;
    other.failure_ = DbAcquireFailure::kNone;
    other.broken_reason_ = DbFailureClass::kNone;
  }
  return *this;
}

void DbLease::MarkBroken(DbFailureClass reason) {
  // The first classification wins: a caller that already knows the connection died should not be
  // downgraded by a later "also this statement failed" note.
  if (broken_reason_ == DbFailureClass::kNone) {
    broken_reason_ = reason;
  }
}

void DbLease::Reset() {
  if (pool_ != nullptr) {
    pool_->Release(this);
  }
  pool_ = nullptr;
  slot_index_ = 0;
  handle_ = nullptr;
  failure_ = DbAcquireFailure::kNone;
  broken_reason_ = DbFailureClass::kNone;
}

DbPool::DbPool(DbConfig connection, DbPoolOptions options) {
  // An open circuit must outlast the attempt that opened it, or it protects nothing: with a 3s
  // connect timeout and the 500ms default cooldown, the next request one second later finds the
  // circuit closed again and pays the full 3s. Measured before this floor existed: a MySQL that
  // accepts the socket and then stops talking cost every single request 3.0s, forever.
  FailureBudgetOptions& budget = options.failure_budget;
  budget.cooldown_floor_ms = std::max(budget.cooldown_floor_ms, static_cast<int64_t>(options.connect_timeout_ms));
  budget.cooldown_max_ms = std::max(budget.cooldown_max_ms, budget.cooldown_floor_ms);
  impl_ = std::make_unique<Impl>(std::move(connection), options);
}

DbPool::~DbPool() {
  // Never close here: mysql_close belongs to the thread that opened the connection. main() makes
  // every shard call Stop() first, so anything left is a bug and only worth a log line.
  for (const auto& slot : impl_->slots) {
    if (slot->handle != nullptr) {
      spdlog::warn("db: pool destroyed with slot {} still connected", slot->index);
    }
  }
}

bool DbPool::BindCurrentThread(size_t slot_index) {
  if (slot_index >= impl_->slots.size()) {
    spdlog::error("db: slot index {} is out of range (the pool has {} slots)", slot_index, impl_->slots.size());
    return false;
  }
  Impl::Slot& slot = *impl_->slots[slot_index];
  if (Impl::current_slot == &slot) {
    return true;  // Initialize() retried on the same thread
  }
  const uint64_t token = std::hash<std::thread::id> {}(std::this_thread::get_id());
  if (!impl_->ownership.TryBind(slot_index, token)) {
    spdlog::error("db: slot {} is already bound to another thread", slot_index);
    return false;
  }
  // Registers this thread with the client library; every thread that issues statements needs it.
  mysql_thread_init();
  slot.owner = std::this_thread::get_id();
  Impl::current_slot = &slot;
  impl_->bound_slots.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void DbPool::CloseCurrentThread() {
  Impl::Slot* slot = Impl::current_slot;
  if (slot == nullptr) {
    return;  // never bound: nothing was registered, nothing to undo
  }
  Impl::CloseHandle(slot);
  const uint64_t token = std::hash<std::thread::id> {}(std::this_thread::get_id());
  if (impl_->ownership.TryUnbind(slot->index, token)) {
    impl_->bound_slots.fetch_sub(1, std::memory_order_relaxed);
  }
  slot->leased = false;
  slot->owner = std::thread::id {};
  Impl::current_slot = nullptr;
  mysql_thread_end();
}

DbLease DbPool::Acquire() {
  Impl::Slot* slot = Impl::current_slot;
  if (slot == nullptr) {
    return DbLease::Failure(DbAcquireFailure::kNotBound);
  }
  assert(!slot->leased && "the previous lease on this slot was not returned");

  const int64_t now_ms = clock::MonotonicMs();
  slot->acquires.fetch_add(1, std::memory_order_relaxed);

  const FailureBudget::Verdict verdict = slot->budget.BeginAttempt(now_ms);
  if (verdict == FailureBudget::Verdict::kReject) {
    // Fast failure is the whole point: a dead MySQL must not cost every request a connect timeout.
    slot->circuit_fast_fails.fetch_add(1, std::memory_order_relaxed);
    slot->acquire_failures.fetch_add(1, std::memory_order_relaxed);
    return DbLease::Failure(DbAcquireFailure::kCircuitOpen);
  }
  if (verdict == FailureBudget::Verdict::kAllowProbe) {
    spdlog::info("db: slot {} probing MySQL after the cooldown", slot->index);
  }

  if (!impl_->EnsureUsableHandle(slot, now_ms)) {
    slot->acquire_failures.fetch_add(1, std::memory_order_relaxed);
    return DbLease::Failure(DbAcquireFailure::kConnectFailed);
  }
  // The socket is up, so the streak is over even if a statement fails later: that later failure
  // goes through MarkBroken and starts a fresh count.
  slot->budget.OnSuccess(now_ms);
  Impl::SyncBudget(slot);
  slot->leased = true;
  return DbLease(this, slot->index, slot->handle);
}

void DbPool::Release(DbLease* lease) {
  Impl::Slot& slot = *impl_->slots[lease->slot_index_];
  assert(slot.owner == std::this_thread::get_id() && "a lease must be released on its own db thread");
  slot.leased = false;

  const DbFailureClass reason = lease->broken_reason_;
  const int64_t now_ms = clock::MonotonicMs();
  if (BreaksConnection(reason)) {
    Impl::CloseHandle(&slot);
    slot.closed_broken.fetch_add(1, std::memory_order_relaxed);
  }
  if (CountsAsUnavailable(reason)) {
    Impl::ChargeFailure(&slot, now_ms, reason);
  } else {
    // The connection survived whatever the caller saw (or nothing went wrong at all).
    slot.last_used_ms = now_ms;
  }
  lease->pool_ = nullptr;
  lease->handle_ = nullptr;
}

void DbPool::Drain() {
  size_t open = 0;
  for (const auto& slot : impl_->slots) {
    if (slot->handle != nullptr) {
      ++open;
      spdlog::warn("db: slot {} still holds a connection at shutdown; only its own thread could close it", slot->index);
    }
  }
  // A slot that still thinks it owns a thread means some shard never reached Stop(). Worth saying
  // out loud: it leaks the socket and hides the reason the connection count looked wrong.
  const uint64_t still_bound = impl_->bound_slots.load(std::memory_order_relaxed);
  if (still_bound != 0) {
    spdlog::warn("db: {} pool slot(s) still bound to a thread; a shard skipped Stop()", still_bound);
  }
  if (open == 0 && still_bound == 0) {
    spdlog::info("db: pool drained cleanly ({} slots)", impl_->slots.size());
  }
}

PoolStatsSnapshot DbPool::Stats() const {
  PoolStatsSnapshot snapshot;
  for (const auto& slot : impl_->slots) {
    const Impl::Slot& own = *slot;
    snapshot.acquires += own.acquires.load(std::memory_order_relaxed);
    snapshot.acquire_failures += own.acquire_failures.load(std::memory_order_relaxed);
    snapshot.circuit_fast_fails += own.circuit_fast_fails.load(std::memory_order_relaxed);
    snapshot.connects += own.connects.load(std::memory_order_relaxed);
    snapshot.connect_failures += own.connect_failures.load(std::memory_order_relaxed);
    snapshot.pings += own.pings.load(std::memory_order_relaxed);
    snapshot.ping_failures += own.ping_failures.load(std::memory_order_relaxed);
    snapshot.recycled_lifetime += own.recycled_lifetime.load(std::memory_order_relaxed);
    snapshot.recycled_idle += own.recycled_idle.load(std::memory_order_relaxed);
    snapshot.closed_broken += own.closed_broken.load(std::memory_order_relaxed);
    if (own.connected.load(std::memory_order_relaxed)) {
      ++snapshot.open_connections;
    }
    snapshot.consecutive_failures =
        std::max(snapshot.consecutive_failures, own.consecutive_failures.load(std::memory_order_relaxed));
    snapshot.cooldown_until_ms =
        std::max(snapshot.cooldown_until_ms, own.cooldown_until_ms.load(std::memory_order_relaxed));
  }
  snapshot.bound_slots = impl_->bound_slots.load(std::memory_order_relaxed);
  return snapshot;
}

// ---- DbActor -----------------------------------------------------------------------------

DbActor::DbActor(std::shared_ptr<DbPool> pool, DbConfig config, size_t slot_index)
    : pool_(std::move(pool)), config_(std::move(config)), slot_index_(slot_index) {
  // Deliberately empty: the connection belongs to the shard's thread, and this constructor runs
  // on the thread that called Spawn. Initialize() claims the slot and opens it on the right thread.
}

stdexec::task<bool> DbActor::Initialize() {
  // The library is initialised once for the whole process (DbLibraryInit); this call only
  // registers the current thread with it, which is what makes the client usable here at all.
  // Every db_pool thread runs this for itself.
  if (!DbLibraryInit()) {
    spdlog::error("db: mysql_library_init failed; account features are disabled");
    co_return false;
  }
  if (!pool_->BindCurrentThread(slot_index_)) {
    spdlog::error("db: cannot bind db slot {} to this thread; account features are disabled", slot_index_);
    co_return false;
  }

  DbLease lease = AcquireReadyLease();
  if (!lease) {
    // §2.3: the server keeps running; account frames answer INTERNAL from here on.
    spdlog::error("db: slot {} cannot reach '{}' on {}:{} ({}); accounts are disabled, the game itself is unaffected",
                  slot_index_, config_.db, config_.host, config_.port, ToString(lease.failure()));
    co_return false;
  }
  spdlog::info("db: slot {} ready (database '{}' on {}:{})", slot_index_, config_.db, config_.host, config_.port);
  co_return true;
}

DbLease DbActor::AcquireReadyLease() {
  DbLease lease = pool_->Acquire();
  if (!lease) {
    // The pool already logged the state transition; the per-request detail stays at debug level.
    spdlog::debug("db: slot {} unavailable ({})", slot_index_, ToString(lease.failure()));
    return lease;
  }
  if (schema_ready_) {
    return lease;
  }
  // A cold start can find MySQL down and the server up later. Without this retry the tables would
  // never be created and every statement would fail with "table doesn't exist" until a restart,
  // which outlives the outage itself.
  const DbFailureClass reason = EnsureSchema(lease.handle());
  if (reason == DbFailureClass::kNone) {
    schema_ready_ = true;
    return lease;
  }
  spdlog::error("db: slot {} could not create the schema ({}); retrying on a later request", slot_index_,
                ToString(reason));
  lease.MarkBroken(reason);
  return DbLease::Failure(DbAcquireFailure::kConnectFailed);
}

DbFailureClass DbActor::EnsureSchema(void* mysql_handle) {
  if (!IsValidDatabaseName(config_.db)) {
    spdlog::error("db: '{}' is not a usable database name", config_.db);
    return DbFailureClass::kConfig;
  }
  MYSQL* mysql = static_cast<MYSQL*>(mysql_handle);
  const std::string create_database = "CREATE DATABASE IF NOT EXISTS `" + config_.db + "` CHARACTER SET utf8mb4";
  if (mysql_query(mysql, create_database.c_str()) != 0) {
    spdlog::error("db: CREATE DATABASE failed: {}", mysql_error(mysql));
    return ClassifyOrQueryError(mysql);
  }
  if (mysql_select_db(mysql, config_.db.c_str()) != 0) {
    spdlog::error("db: cannot select database '{}': {}", config_.db, mysql_error(mysql));
    return ClassifyOrQueryError(mysql);
  }

  // One statement per mysql_query call (the client library does not enable multi-statements).
  const char* const kSchema[] = {
      "CREATE TABLE IF NOT EXISTS accounts ("
      "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
      "  username VARCHAR(32) NOT NULL,"
      "  password_salt CHAR(32) NOT NULL,"
      "  password_hash CHAR(64) NOT NULL,"
      "  nickname VARCHAR(64) NOT NULL,"
      "  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "  last_login_at TIMESTAMP NULL,"
      "  PRIMARY KEY (id),"
      "  UNIQUE KEY uk_accounts_username (username)"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
      "CREATE TABLE IF NOT EXISTS matches ("
      "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
      "  account_id BIGINT UNSIGNED NOT NULL,"
      "  room_id VARCHAR(16) NOT NULL,"
      "  reason VARCHAR(16) NOT NULL,"
      "  mode VARCHAR(16) NOT NULL DEFAULT 'classic',"
      "  delta_score INT NOT NULL,"
      "  win TINYINT NOT NULL,"
      "  ended_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "  PRIMARY KEY (id),"
      "  KEY idx_matches_account (account_id, id)"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
      // v1.3 新增：场次汇总（一场每账号一行），与 ACCOUNT_DESIGN.md §2.1 逐字段一致。
      "CREATE TABLE IF NOT EXISTS match_results ("
      "  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
      "  account_id BIGINT UNSIGNED NOT NULL,"
      "  room_id VARCHAR(16) NOT NULL,"
      "  hands_played INT NOT NULL,"
      "  dealer_hands INT NOT NULL,"
      "  final_score INT NOT NULL,"
      "  final_rank TINYINT NOT NULL,"
      "  mode VARCHAR(16) NOT NULL DEFAULT 'classic',"
      "  ended_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
      "  PRIMARY KEY (id),"
      "  KEY idx_match_results_account (account_id, id),"
      "  UNIQUE KEY uk_match_results_room_account (room_id, account_id)"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
  };
  for (const char* statement : kSchema) {
    if (mysql_query(mysql, statement) != 0) {
      spdlog::error("db: schema creation failed: {}", mysql_error(mysql));
      return ClassifyOrQueryError(mysql);
    }
  }
  // 迁移：日麻接入前建的库没有 mode 列。重复列（1060）是「已经迁移过」，直接忽略；
  // 其它错误只记 WARN —— 老库缺列时战绩写入会失败，但**不能**因此拒绝启动（§2.3）。
  static constexpr const char* kMigrations[] = {
      "ALTER TABLE matches ADD COLUMN mode VARCHAR(16) NOT NULL DEFAULT 'classic'",
      "ALTER TABLE match_results ADD COLUMN mode VARCHAR(16) NOT NULL DEFAULT 'classic'",
  };
  for (const char* statement : kMigrations) {
    if (mysql_query(mysql, statement) != 0 && mysql_errno(mysql) != 1060 /* ER_DUP_FIELDNAME */) {
      spdlog::warn("db: migration skipped ({}): {}", statement, mysql_error(mysql));
    }
  }
  return DbFailureClass::kNone;
}

std::string DbActor::Escape(const DbLease& lease, const std::string& value) {
  MYSQL* mysql = static_cast<MYSQL*>(lease.handle());
  if (mysql == nullptr) {
    return {};
  }
  std::vector<char> buffer(value.size() * 2 + 1);
  unsigned long length =
      mysql_real_escape_string(mysql, buffer.data(), value.c_str(), static_cast<unsigned long>(value.size()));
  return std::string(buffer.data(), length);
}

// ---- handlers (db_pool thread) -----------------------------------------------------------

stdexec::task<void> DbActor::Login(std::string username, std::string password, ex_actor::ActorRef<PlayerActor> reply) {
  ReplyAuth(reply, LoginJob(username, password));
  co_return;
}

stdexec::task<void> DbActor::Register(std::string username, std::string password, std::string nickname,
                                      ex_actor::ActorRef<PlayerActor> reply) {
  ReplyRegister(reply, RegisterJob(username, password, nickname));
  co_return;
}

stdexec::task<void> DbActor::FetchStats(uint64_t account_id, ex_actor::ActorRef<PlayerActor> reply) {
  ReplyStats(reply, StatsJob(account_id));
  co_return;
}

stdexec::task<void> DbActor::RecordMatch(MatchRecord record) {
  // Fire and forget: a failed write is logged and never propagates to the game (§2.3, §4.6).
  RecordJob(record);
  co_return;
}

stdexec::task<void> DbActor::RecordMatchResult(MatchResultRecord record) {
  // Fire and forget (§4 item 7)：场终一次性写入；失败只记日志，绝不影响 match_end 的广播。
  RecordMatchResultJob(record);
  co_return;
}

stdexec::task<void> DbActor::Stop() {
  // Closes the connection and unregisters this thread. Both must happen here: the library is
  // thread scoped, and the pool would have no safe way to close a socket from main()'s thread.
  // main() tears the library itself down afterwards (DbLibraryShutdown).
  pool_->CloseCurrentThread();
  co_return;
}

// ---- query bodies (db_pool thread) -------------------------------------------------------

AuthOutcome DbActor::LoginJob(const std::string& username, const std::string& password) {
  AuthOutcome outcome;
  DbLease lease = AcquireReadyLease();
  if (!lease) {
    return outcome;  // kUnavailable: the client answers INTERNAL (§2.3)
  }
  MYSQL* mysql = static_cast<MYSQL*>(lease.handle());
  // §3.1: an unknown account and a wrong password must be indistinguishable, so one query
  // answers both and the row's absence is simply a failure.
  const std::string sql = "SELECT id, nickname, (password_hash = SHA2(CONCAT(password_salt, '" +
                          Escape(lease, password) + "'), 256)) AS ok FROM accounts WHERE username = '" +
                          Escape(lease, username) + "'";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    const DbFailureClass reason = ClassifyOrQueryError(mysql);
    spdlog::error("db: login query failed ({}): {}", ToString(reason), mysql_error(mysql));
    lease.MarkBroken(reason);
    return outcome;
  }
  MYSQL_RES* result = mysql_store_result(mysql);
  if (result == nullptr) {
    const DbFailureClass reason = ClassifyOrQueryError(mysql);
    spdlog::error("db: login query returned no result ({}): {}", ToString(reason), mysql_error(mysql));
    lease.MarkBroken(reason);
    return outcome;
  }
  MYSQL_ROW row = mysql_fetch_row(result);
  if (row != nullptr && row[0] != nullptr && row[1] != nullptr && row[2] != nullptr && std::strcmp(row[2], "1") == 0) {
    outcome.status = AuthStatus::kOk;
    outcome.account.id = ParseUint(row[0]);
    outcome.account.username = username;
    outcome.account.nickname = row[1];
  } else {
    outcome.status = AuthStatus::kAuthFailed;
  }
  mysql_free_result(result);

  if (outcome.status == AuthStatus::kOk) {
    // Best effort: a failed timestamp update must not fail a successful login.
    const std::string touch =
        "UPDATE accounts SET last_login_at = NOW() WHERE id = " + std::to_string(outcome.account.id);
    if (mysql_query(mysql, touch.c_str()) != 0) {
      const DbFailureClass reason = ClassifyOrQueryError(mysql);
      spdlog::warn("db: could not update last_login_at ({}): {}", ToString(reason), mysql_error(mysql));
      lease.MarkBroken(reason);
    }
  }
  return outcome;
}

AuthOutcome DbActor::RegisterJob(const std::string& username, const std::string& password,
                                 const std::string& nickname) {
  AuthOutcome outcome;
  DbLease lease = AcquireReadyLease();
  if (!lease) {
    return outcome;  // kUnavailable (§2.3)
  }
  MYSQL* mysql = static_cast<MYSQL*>(lease.handle());
  const std::string salt = RandomSalt();
  const std::string stored_nickname = nickname.empty() ? username : nickname;
  const std::string sql =
      "INSERT INTO accounts (username, password_salt, password_hash, nickname, last_login_at) VALUES ('" +
      Escape(lease, username) + "', '" + salt + "', SHA2(CONCAT('" + salt + "', '" + Escape(lease, password) +
      "'), 256), '" + Escape(lease, stored_nickname) + "', NOW())";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    const DbFailureClass reason = ClassifyOrQueryError(mysql);
    if (reason == DbFailureClass::kDuplicate) {
      outcome.status = AuthStatus::kUsernameTaken;  // the UNIQUE key is the arbiter of races
    } else {
      spdlog::error("db: register failed ({}): {}", ToString(reason), mysql_error(mysql));
      lease.MarkBroken(reason);
    }
    return outcome;
  }
  outcome.status = AuthStatus::kOk;
  outcome.account.id = static_cast<uint64_t>(mysql_insert_id(mysql));
  outcome.account.username = username;
  outcome.account.nickname = stored_nickname;
  return outcome;
}

StatsOutcome DbActor::StatsJob(uint64_t account_id) {
  StatsOutcome outcome;
  DbLease lease = AcquireReadyLease();
  if (!lease) {
    return outcome;  // ok stays false (§2.3)
  }
  MYSQL* mysql = static_cast<MYSQL*>(lease.handle());
  const std::string id = std::to_string(account_id);

  // 一种玩法一套统计。两套玩法的 net_score 语义不同（零和得失分 vs 25000 起持点），
  // 合并成一个数字会直接误导玩家，所以按 mode 分桶查询、分桶返回。
  auto load_mode = [&](std::string_view mode, protocol::StatsSummary* summary) -> bool {
    const std::string filter = " AND mode = '" + std::string(mode) + "'";
    // COALESCE: an account with no finished game has SUM(x) = NULL, not 0.
    const std::string totals =
        "SELECT COUNT(*), COALESCE(SUM(delta_score), 0), COALESCE(SUM(win), 0),"
        " COALESCE(SUM(reason = 'draw_exhausted'), 0) FROM matches WHERE account_id = " +
        id + filter;
    if (mysql_query(mysql, totals.c_str()) != 0) {
      const DbFailureClass reason = ClassifyOrQueryError(mysql);
      spdlog::error("db: stats query failed ({}): {}", ToString(reason), mysql_error(mysql));
      lease.MarkBroken(reason);
      return false;
    }
    MYSQL_RES* result = mysql_store_result(mysql);
    if (result == nullptr) {
      const DbFailureClass reason = ClassifyOrQueryError(mysql);
      spdlog::error("db: stats query returned no result ({}): {}", ToString(reason), mysql_error(mysql));
      lease.MarkBroken(reason);
      return false;
    }
    if (MYSQL_ROW row = mysql_fetch_row(result)) {
      summary->games = ParseInt(row[0]);
      summary->net_score = ParseInt(row[1]);
      summary->wins = ParseInt(row[2]);
      summary->draws = ParseInt(row[3]);
    }
    mysql_free_result(result);

    // §3.4: the recent list is newest first and capped at kMaxRecentMatches.
    const std::string recent =
        "SELECT room_id, reason, mode, delta_score, win, UNIX_TIMESTAMP(ended_at) FROM matches"
        " WHERE account_id = " +
        id + filter + " ORDER BY id DESC LIMIT " + std::to_string(protocol::kMaxRecentMatches);
    if (mysql_query(mysql, recent.c_str()) != 0) {
      const DbFailureClass reason = ClassifyOrQueryError(mysql);
      spdlog::error("db: recent matches query failed ({}): {}", ToString(reason), mysql_error(mysql));
      lease.MarkBroken(reason);
      return false;
    }
    result = mysql_store_result(mysql);
    if (result == nullptr) {
      const DbFailureClass reason = ClassifyOrQueryError(mysql);
      spdlog::error("db: recent matches query returned no result ({}): {}", ToString(reason), mysql_error(mysql));
      lease.MarkBroken(reason);
      return false;
    }
    while (MYSQL_ROW entry = mysql_fetch_row(result)) {
      if (entry[0] == nullptr || entry[1] == nullptr) {
        continue;
      }
      protocol::StatsEntry item;
      item.room_id = entry[0];
      // DB 落库仍是字符串词表（v2.0 只改线格式），读出时在边界翻回线枚举。
      item.reason = protocol::FromName<protocol::RoundEndReason>(entry[1] != nullptr ? entry[1] : "")
                        .value_or(protocol::RoundEndReason::kInvalid);
      item.mode = protocol::FromName<protocol::WireMode>(entry[2] != nullptr ? entry[2] : "classic")
                      .value_or(protocol::WireMode::kClassic);
      item.delta_score = ParseInt(entry[3]);
      item.win = entry[4] != nullptr && std::strcmp(entry[4], "1") == 0;
      item.ended_at = static_cast<int64_t>(ParseUint(entry[5]));
      summary->recent.push_back(std::move(item));
    }
    mysql_free_result(result);
    return true;
  };

  if (!load_mode("classic", &outcome.snapshot.classic) || !load_mode("riichi", &outcome.snapshot.riichi)) {
    return outcome;  // ok stays false
  }
  outcome.ok = true;
  return outcome;
}

void DbActor::RecordJob(const MatchRecord& record) {
  DbLease lease = AcquireReadyLease();
  if (!lease) {
    spdlog::warn("db: dropping the match record of room {} (slot {} unavailable: {})", record.room_id, slot_index_,
                 ToString(lease.failure()));
    return;
  }
  MYSQL* mysql = static_cast<MYSQL*>(lease.handle());
  const std::string sql = "INSERT INTO matches (account_id, room_id, reason, mode, delta_score, win) VALUES (" +
                          std::to_string(record.account_id) + ", '" + Escape(lease, record.room_id) + "', '" +
                          Escape(lease, record.reason) + "', '" + Escape(lease, record.mode) + "', " +
                          std::to_string(record.delta_score) + ", " + (record.win ? "1" : "0") + ")";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    const DbFailureClass reason = ClassifyOrQueryError(mysql);
    spdlog::error("db: recording the match of room {} failed ({}): {}", record.room_id, ToString(reason),
                  mysql_error(mysql));
    // Never replayed: `matches` has no unique key, so a retry would record the same game twice.
    lease.MarkBroken(reason);
  }
}

void DbActor::RecordMatchResultJob(const MatchResultRecord& record) {
  DbLease lease = AcquireReadyLease();
  if (!lease) {
    spdlog::warn("db: dropping the match result of room {} (slot {} unavailable: {})", record.room_id, slot_index_,
                 ToString(lease.failure()));
    return;
  }
  MYSQL* mysql = static_cast<MYSQL*>(lease.handle());
  const std::string sql =
      "INSERT INTO match_results (account_id, room_id, mode, hands_played, dealer_hands, final_score, final_rank) "
      "VALUES (" +
      std::to_string(record.account_id) + ", '" + Escape(lease, record.room_id) + "', '" + Escape(lease, record.mode) +
      "', " + std::to_string(record.hands_played) + ", " + std::to_string(record.dealer_hands) + ", " +
      std::to_string(record.final_score) + ", " + std::to_string(record.final_rank) + ")";
  if (mysql_query(mysql, sql.c_str()) != 0) {
    const DbFailureClass reason = ClassifyOrQueryError(mysql);
    // v1.3 §4 第 1 条：重复写入（同 room_id + account_id 命中 UNIQUE 键）记 WARN 并忽略，绝不重试。
    if (reason == DbFailureClass::kDuplicate) {
      spdlog::warn("db: match result of room {} for account {} already recorded; ignoring the duplicate",
                   record.room_id, record.account_id);
      return;
    }
    spdlog::error("db: recording the match result of room {} failed ({}): {}", record.room_id, ToString(reason),
                  mysql_error(mysql));
    lease.MarkBroken(reason);
  }
}

}  // namespace mahjong
