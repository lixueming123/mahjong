// DbActor: the only part of the server that talks to MySQL, and the front of the connection pool.
//
// ACCOUNT_DESIGN.md §6.1 says an actor handler must never block the actor world's threads.
// MySQL *blocks* (a password check is a round trip), so these actors do not live on the game
// pools at all: main() gives each shard its own dedicated single-thread pool (`db_pool(1)` per
// shard) through `ex_actor::SchedulerUnion` + `ActorConfig::scheduler_index`. Blocking inside one
// of the handlers therefore parks exactly one thread, which nobody else is scheduled on, and the
// handler runs to completion before the next message starts.
//
// Since v1.6 the connection itself lives in `DbPool` (db_pool.h, §2.4): a handler takes a
// `DbLease`, runs its statements on it, and the lease's destructor gives the connection back. The
// pool owns connect/reconnect, the idle ping, rotation, the circuit breaker and the counters.
//
// Three consequences shape the interface:
//   * A shard's single thread is also its FIFO guarantee (§4.1): a match record is submitted
//     before the client can ask for the stats that must include it, and one mailbox drained in
//     order preserves that for free — *within* a shard. DbService is what makes two requests for
//     one account land on the same shard in the first place.
//   * The MySQL connection is per-thread (mysql_thread_init/mysql_thread_end are thread
//     scoped), so the pool binds one slot to one thread and the slot count equals the thread
//     count by construction. A connection may therefore be rotated or rebuilt between two
//     requests without disturbing the ordering above: what serializes them is the mailbox, not
//     the socket.
//   * The client library itself is process-wide (DbLibraryInit/DbLibraryShutdown), owned by
//     main(), because mysql_library_init is not thread safe and must not be torn down while
//     another shard's thread is still using it.
//
// Callers reach these actors through `DbService` — a key-routed set of `ActorRef<DbActor>`, never
// a pointer — and every request carries a `reply` ref for its result: PlayerActor's handlers
// must not suspend, so the answer is a message back into its mailbox rather than something to
// `co_await`.
//
// Two implementations exist and CMake picks exactly one, so no #ifdef leaks into the code:
//   * db_mysql.cc — the real one, linked against libmysqlclient (and the real DbPool)
//   * db_stub.cc  — used with -DMAHJONG_WITH_MYSQL=OFF; everything reports "unavailable",
//                   which is the same degraded behaviour as a MySQL that is down (§2.3).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <exec/task.hpp>

#include "db_policy.h"
#include "ex_actor/api.h"
#include "protocol.h"

namespace mahjong {

class PlayerActor;
// Forward declared on purpose: this header must stay usable in a build without <mysql.h>, and
// both types hide every libmysqlclient detail behind db_pool.h.
class DbLease;
class DbPool;

struct DbConfig {
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user = "root";
  std::string password;
  std::string db = "mahjong";
};

// The durable account row (§2.1). `id` becomes the account's player_id (player.h).
struct AccountRow {
  uint64_t id = 0;
  std::string username;
  std::string nickname;
};

enum class AuthStatus {
  kOk,
  // Wrong password *or* unknown account: the two are deliberately indistinguishable (§3.1).
  kAuthFailed,
  kUsernameTaken,  // register only
  // MySQL is unreachable or the schema could not be created (§2.3). The caller answers
  // INTERNAL and, unlike a client error, does not charge any quota.
  kUnavailable,
};

struct AuthOutcome {
  AuthStatus status = AuthStatus::kUnavailable;
  AccountRow account;  // meaningful only when status == kOk
};

// One finished game as it is stored in `matches` (§2.1, §4).
struct MatchRecord {
  uint64_t account_id = 0;
  std::string room_id;
  std::string reason;  // win_ron / win_tsumo / draw_exhausted
  int delta_score = 0;
  bool win = false;
  std::string mode = "classic";  // classic / riichi（stats 按玩法分桶，绝不混算）
};

// One finished MATCH per account, stored in `match_results` (ACCOUNT_DESIGN.md v1.3 §4 item 7).
// Written exactly once per match (at match_end), one row per account-backed seat; the
// UNIQUE(room_id, account_id) key is the structural defence against double writes.
struct MatchResultRecord {
  uint64_t account_id = 0;
  std::string room_id;
  std::string mode = "classic";
  int hands_played = 0;  // 实际计入局数（D2 口径：无截断 = round_count，截断场更小）
  int dealer_hands = 0;  // 该座位本场坐庄的手数（含连庄的额外手）
  int final_score = 0;   // 本场累计得失分（同 room_id 四行之和 = 0）
  int final_rank = 0;    // 1..4，并列跳号（PROTOCOL.md §5.15）
};

// A zeroed summary is a legitimate answer for a guest (§3.3), so the caller cannot tell a
// failure from "no games played" by the payload alone — hence the explicit flag.
struct StatsOutcome {
  bool ok = false;
  protocol::StatsSnapshot snapshot;  // classic 同时是 stats 帧的兼容顶层字段
};

class DbActor {
 public:
  // Defined by whichever of db_mysql.cc / db_stub.cc is compiled in. |pool| is the process-wide
  // pool shared by every shard; |slot_index| is the slot this shard's thread will claim.
  DbActor(std::shared_ptr<DbPool> pool, DbConfig config, size_t slot_index);

  // Binds the shard's thread to its pool slot and probes MySQL through a lease. Runs on db_pool,
  // so the connect's blocking resolve/TCP/auth never touches the game pools. Returns whether
  // accounts are usable; the server starts either way (§2.3).
  stdexec::task<bool> Initialize();

  // Login/Register/FetchStats answer by posting OnLoginResult/OnRegisterResult/OnStatsResult
  // into |reply|; they return nothing, so the caller never awaits MySQL.
  stdexec::task<void> Login(std::string username, std::string password, ex_actor::ActorRef<PlayerActor> reply);
  // `nickname` empty means "use the username" (§3.2).
  stdexec::task<void> Register(std::string username, std::string password, std::string nickname,
                               ex_actor::ActorRef<PlayerActor> reply);
  stdexec::task<void> FetchStats(uint64_t account_id, ex_actor::ActorRef<PlayerActor> reply);
  // Fire and forget: a failed write is logged and never propagates to the game (§2.3, §4.6).
  stdexec::task<void> RecordMatch(MatchRecord record);
  // Fire and forget, v1.3 (§4 item 7): one `match_results` row per account at match end.
  // A duplicate (room_id, account_id) is logged as a WARN and ignored — never retried.
  stdexec::task<void> RecordMatchResult(MatchResultRecord record);

  // Releases the slot: closes the connection and unregisters the thread. Runs on db_pool (the
  // library is thread scoped, so opening and closing must happen on the same thread); idempotent.
  stdexec::task<void> Stop();

  // The method list ex_actor needs to resolve `Send<&DbActor::X>` (reflect.h). It sits
  // after the declarations because a static data member's initializer is not a complete-class
  // context: naming the methods before they are declared does not compile.
  constexpr static auto kActorMethods =
      std::make_tuple(&DbActor::Initialize, &DbActor::Login, &DbActor::Register, &DbActor::FetchStats,
                      &DbActor::RecordMatch, &DbActor::RecordMatchResult, &DbActor::Stop);

 private:
  // Acquire plus the schema bootstrap: a cold start may find MySQL down, and the tables have to
  // be created on the first lease that actually reaches the server, not only at startup.
  DbLease AcquireReadyLease();
  // Creates the database and the tables. Returns kNone on success, otherwise the classification
  // the caller must charge to the pool.
  DbFailureClass EnsureSchema(void* mysql_handle);
  // Escapes a value for the single-quoted literal it will sit in (§2.2). The inputs are
  // already charset-validated, so this is the second line of defence, not the first.
  std::string Escape(const DbLease& lease, const std::string& value);
  AuthOutcome LoginJob(const std::string& username, const std::string& password);
  AuthOutcome RegisterJob(const std::string& username, const std::string& password, const std::string& nickname);
  StatsOutcome StatsJob(uint64_t account_id);
  void RecordJob(const MatchRecord& record);
  void RecordMatchResultJob(const MatchResultRecord& record);

  std::shared_ptr<DbPool> pool_;
  DbConfig config_;
  size_t slot_index_ = 0;
  // True once this shard has created the schema; set on the shard's thread only.
  bool schema_ready_ = false;
};

// ---- process-level client-library lifetime -------------------------------------------------

// mysql_library_init / mysql_library_end are process-wide and not thread safe, so they can no
// longer live inside DbActor: with more than one db thread, a per-actor init/end would tear the
// library down under another thread's feet. main() calls these once around the whole run; each
// db thread keeps only mysql_thread_init / mysql_thread_end. No-ops in the db_stub.cc build.
bool DbLibraryInit();
void DbLibraryShutdown();

// ---- sharded facade -------------------------------------------------------------------------

// A MySQL connection is thread-scoped, so the connection count equals the thread count: every
// shard is one DbActor pinned to its own single-thread pool. This type replaces the bare
// ActorRef and routes each request by a key.
//
// The keying is what keeps ACCOUNT_DESIGN.md §4.1's FIFO property alive once a single mailbox no
// longer serializes everything: Register and Login both key on the username (the account id is
// not known before login), so they share a connection; a match record is keyed by account id,
// exactly like the StatsSummary request that must include it.
class DbService {
 public:
  void AddShard(ex_actor::ActorRef<DbActor> shard) { shards_.push_back(std::move(shard)); }

  // Callers keep the "no storage configured" check they had against an empty ActorRef.
  bool IsEmpty() const { return shards_.empty(); }
  size_t shard_count() const { return shards_.size(); }
  const std::vector<ex_actor::ActorRef<DbActor>>& shards() const { return shards_; }

  [[nodiscard]] auto Login(std::string username, std::string password, ex_actor::ActorRef<PlayerActor> reply) const {
    // `target` is evaluated before the arguments, so moving `username` into the call is safe.
    const ex_actor::ActorRef<DbActor> target = PickByString(username);
    return target.Send<&DbActor::Login>(std::move(username), std::move(password), std::move(reply));
  }

  [[nodiscard]] auto Register(std::string username, std::string password, std::string nickname,
                              ex_actor::ActorRef<PlayerActor> reply) const {
    const ex_actor::ActorRef<DbActor> target = PickByString(username);
    return target.Send<&DbActor::Register>(std::move(username), std::move(password), std::move(nickname),
                                           std::move(reply));
  }

  [[nodiscard]] auto FetchStats(uint64_t account_id, ex_actor::ActorRef<PlayerActor> reply) const {
    const ex_actor::ActorRef<DbActor> target = PickByAccount(account_id);
    return target.Send<&DbActor::FetchStats>(account_id, std::move(reply));
  }

  [[nodiscard]] auto RecordMatch(MatchRecord record) const {
    const ex_actor::ActorRef<DbActor> target = PickByAccount(record.account_id);
    return target.Send<&DbActor::RecordMatch>(std::move(record));
  }

  [[nodiscard]] auto RecordMatchResult(MatchResultRecord record) const {
    const ex_actor::ActorRef<DbActor> target = PickByAccount(record.account_id);
    return target.Send<&DbActor::RecordMatchResult>(std::move(record));
  }

 private:
  ex_actor::ActorRef<DbActor> PickByString(std::string_view key) const {
    return Pick(std::hash<std::string_view> {}(key));
  }
  ex_actor::ActorRef<DbActor> PickByAccount(uint64_t account_id) const {
    return Pick(std::hash<uint64_t> {}(account_id));
  }
  // An empty result reproduces the old empty-ActorRef behaviour: Send throws, and callers
  // that check IsEmpty() first never get here.
  ex_actor::ActorRef<DbActor> Pick(size_t hash) const {
    if (shards_.empty()) {
      return ex_actor::ActorRef<DbActor> {};
    }
    return shards_[hash % shards_.size()];
  }

  std::vector<ex_actor::ActorRef<DbActor>> shards_;
};

}  // namespace mahjong
