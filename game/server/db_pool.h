// DbPool: the process-wide MySQL connection pool (ACCOUNT_DESIGN.md §2.4).
//
// One slot per db thread, and a slot may only be used by the thread that bound it. A MYSQL* must
// never be touched by two threads, and libmysqlclient keeps per-thread state registered by
// mysql_thread_init(), so binding is a thread-local pointer and Acquire() stays lock free: the
// calling thread already owns the only slot it can reach. This is the same contract MySQL
// Connector/C++'s ConnectionPool implements with its thread-id keyed getConnection().
//
// The pool owns what used to be implicit in DbActor: lazy connect, ping-before-reuse, lifetime
// and idle rotation, the error classification in db_policy.h, the circuit breaker and the
// counters main() prints at shutdown. It never retries a statement — `matches` has no unique key,
// so replaying an INSERT after a dropped connection would record the same game twice. Reconnecting
// happens inside Acquire() only, before the caller's first byte goes out.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "db.h"
#include "db_policy.h"

namespace mahjong {

struct DbPoolOptions {
  // Per-socket timeouts. The C API speaks seconds, so these are rounded up (§2.3 spelled them as
  // one hard-coded 3s constant for all three).
  int connect_timeout_ms = 3000;
  int read_timeout_ms = 3000;
  int write_timeout_ms = 3000;
  // Rotation. 0 disables the corresponding check.
  int max_lifetime_ms = 30 * 60 * 1000;
  int idle_timeout_ms = 10 * 60 * 1000;
  // A connection idle longer than this is mysql_ping'ed before reuse. Pinging on every Acquire
  // would add a round trip to each query, so short idles trust the socket.
  int ping_after_idle_ms = 5000;
  FailureBudgetOptions failure_budget;
  // Number of slots; main() sets it to the db shard count. Zero is a programming error.
  size_t slot_count = 0;
};

// Why Acquire() produced a failed lease. Handlers only log it: the wire answer is always
// AuthStatus::kUnavailable / StatsOutcome{ok=false} (§2.3).
enum class DbAcquireFailure {
  kNone,
  kNotBound,       // Acquire from a thread that never called BindCurrentThread
  kCircuitOpen,    // fast fail: the cooldown window is still running
  kConnectFailed,  // mysql_real_connect failed, including the retry a dead idle socket forces
};

inline std::string_view ToString(DbAcquireFailure failure) {
  switch (failure) {
    case DbAcquireFailure::kNone:
      return "ok";
    case DbAcquireFailure::kNotBound:
      return "not_bound";
    case DbAcquireFailure::kCircuitOpen:
      return "circuit_open";
    case DbAcquireFailure::kConnectFailed:
      return "connect_failed";
  }
  return "unknown";
}

// Plain data, read atomically on the way out. main() prints it once at shutdown; nobody else reads.
struct PoolStatsSnapshot {
  uint64_t acquires = 0;
  uint64_t acquire_failures = 0;
  uint64_t circuit_fast_fails = 0;
  uint64_t connects = 0;
  uint64_t connect_failures = 0;
  uint64_t pings = 0;
  uint64_t ping_failures = 0;
  uint64_t recycled_lifetime = 0;
  uint64_t recycled_idle = 0;
  uint64_t closed_broken = 0;
  uint64_t open_connections = 0;
  uint64_t bound_slots = 0;
  uint32_t consecutive_failures = 0;  // the worst slot
  int64_t cooldown_until_ms = 0;      // the latest deadline among slots; 0 = no circuit open
};

// Move-only RAII handle on one slot's connection. The destructor returns it to the slot, or closes
// it when the caller marked it broken. It must be destroyed on the thread that produced it (the
// slot's owner), and never held across a co_await.
class DbLease {
 public:
  DbLease() = default;
  ~DbLease();
  DbLease(DbLease&& other) noexcept;
  DbLease& operator=(DbLease&& other) noexcept;
  DbLease(const DbLease&) = delete;
  DbLease& operator=(const DbLease&) = delete;

  // A lease that hands out nothing. Used for "unavailable" answers from Acquire(), and by a
  // caller that already holds one but decides the connection is unusable.
  static DbLease Failure(DbAcquireFailure reason) { return DbLease(reason); }

  bool ok() const { return pool_ != nullptr; }
  explicit operator bool() const { return ok(); }
  // The MYSQL*. Opaque so no header ever sees <mysql.h>; null on a failed lease.
  void* handle() const { return handle_; }
  DbAcquireFailure failure() const { return failure_; }
  // Report the classification of the error the caller just saw. The pool closes the connection
  // when it is broken and charges one failure to the slot's budget when it means "unavailable";
  // kDuplicate and kQueryError leave the connection alone.
  void MarkBroken(DbFailureClass reason);

 private:
  friend class DbPool;
  DbLease(DbPool* pool, size_t slot_index, void* handle);
  explicit DbLease(DbAcquireFailure failure) : failure_(failure) {}
  void Reset();

  DbPool* pool_ = nullptr;
  size_t slot_index_ = 0;
  void* handle_ = nullptr;
  DbAcquireFailure failure_ = DbAcquireFailure::kNone;
  DbFailureClass broken_reason_ = DbFailureClass::kNone;
};

// Process-wide, one object per server. Each slot belongs to exactly one db thread and the
// Acquire path takes no lock; see the file header for why that is safe.
class DbPool {
 public:
  DbPool(DbConfig connection, DbPoolOptions options);
  ~DbPool();
  DbPool(const DbPool&) = delete;
  DbPool& operator=(const DbPool&) = delete;

  // Runs on the db thread, from DbActor::Initialize(): registers the thread with the client
  // library and claims |slot_index|. Idempotent for the same (slot, thread); false for a foreign
  // or out-of-range slot.
  bool BindCurrentThread(size_t slot_index);
  // Runs on the same thread, from DbActor::Stop(): closes the connection and unregisters the
  // thread. Safe when BindCurrentThread was never called or failed.
  void CloseCurrentThread();

  // Connects lazily, applies the rotation and the idle ping, then hands out the slot's
  // connection. Never blocks while the circuit is open.
  DbLease Acquire();

  // Safety net after every actor's Stop(): reports slots that still hold a connection. It must
  // NOT close anything — mysql_close belongs to the owning thread.
  void Drain();

  // Any thread; sums the per-slot relaxed atomics.
  PoolStatsSnapshot Stats() const;

 private:
  friend class DbLease;
  void Release(DbLease* lease);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mahjong
