// DbActor: the only part of the server that talks to MySQL, and the front of the connection pool.
//
// MySQL *blocks* (a password check is a round trip), and an actor handler must never block the
// game pools, so each shard runs on its own single-thread pool (`db_pool(1)`) via
// `SchedulerUnion` + `ActorConfig::scheduler_index`. Blocking parks exactly one thread nobody else
// is scheduled on.
//
// The connection lives in `DbPool` (db_pool.h): a handler takes a `DbLease`, runs its statements on
// it, and the lease's destructor gives the connection back. The pool owns connect/reconnect, the
// idle ping, rotation, the circuit breaker and the counters.
//
// Three consequences shape the interface:
//   * A shard's single thread is also its FIFO guarantee: one mailbox drained in order means a match
//     record is written before the stats query that must include it — *within* a shard. DbService is
//     what makes two requests for one account land on the same shard.
//   * MySQL connections are per-thread, so a pool slot is bound to a thread and the slot count equals
//     the thread count. A connection may be rotated between requests: the mailbox, not the socket, is
//     what serializes them.
//   * The client library is process-wide (DbLibraryInit/Shutdown) and owned by main(), because
//     mysql_library_init is not thread safe.
//
// Callers reach these actors through `GetDbService()`, a process-wide key-routed set of
// `ActorRef<DbActor>`. Every request carries a `reply` ref: PlayerActor's handlers must not suspend,
// so the answer is a message back into its mailbox.
//
// Two implementations exist and CMake picks exactly one, so no #ifdef leaks into the code:
//   * db_mysql.cc — the real one, linked against libmysqlclient
//   * db_stub.cc  — used with -DMAHJONG_WITH_MYSQL=OFF; everything reports "unavailable"

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
// Forward declared on purpose: this header must stay usable without <mysql.h>, and both types hide
// every libmysqlclient detail behind db_pool.h.
class DbLease;
class DbPool;

struct DbConfig {
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user = "root";
  std::string password;
  std::string db = "mahjong";
};

// The durable account row. `id` becomes the account's player_id (player.h).
struct AccountRow {
  uint64_t id = 0;
  std::string username;
  std::string nickname;
};

enum class AuthStatus {
  kOk,
  // Wrong password *or* unknown account: deliberately indistinguishable.
  kAuthFailed,
  kUsernameTaken,  // register only
  // MySQL unreachable or the schema could not be created: answer INTERNAL, charge no quota.
  kUnavailable,
};

struct AuthOutcome {
  AuthStatus status = AuthStatus::kUnavailable;
  AccountRow account;  // meaningful only when status == kOk
};

// One finished round as it is stored in `matches`.
struct MatchRecord {
  uint64_t account_id = 0;
  std::string room_id;
  std::string reason;  // win_ron / win_tsumo / draw_exhausted
  int delta_score = 0;
  bool win = false;
  std::string mode = "classic";  // classic / riichi（stats 按玩法分桶，绝不混算）
};

// One finished MATCH per account, stored in `match_results`.
// Written once per match (at match_end), one row per account-backed seat; the UNIQUE(room_id,
// account_id) key is the structural defence against double writes.
struct MatchResultRecord {
  uint64_t account_id = 0;
  std::string room_id;
  std::string mode = "classic";
  int hands_played = 0;  // 实际计入局数（无截断 = round_count）
  int dealer_hands = 0;  // 该座位本场坐庄的手数（含连庄）
  int final_score = 0;   // 本场累计得失分（同 room_id 四行之和 = 0）
  int final_rank = 0;    // 1..4，并列跳号
};

// One match's replay index row in `replays`. The file is the store's (replay_store.h); this row lets
// `stats.recent[]` mark which rooms have a recording.
struct ReplayRecord {
  std::string room_id;
  std::string mode = "classic";
};

// A zeroed summary is a legitimate answer for a guest, so a failure cannot be told from "no games
// played" by the payload alone — hence the explicit flag.
struct StatsOutcome {
  bool ok = false;
  protocol::StatsSnapshot snapshot;  // classic 同时是 stats 帧的兼容顶层字段
};

// Whether |account_id| may read |room_id|'s recording, i.e. whether that account sat in that game.
// Answered from `matches` instead of the replay file, which since v2.6 is gzip and can no longer be
// parsed to find `seats[].account_id`.
//
// Routing matters: DbService keys this by account id, so it lands in the same mailbox as the
// RecordMatch writes for that account and the FIFO guarantee covers "the match just ended, ask for
// its replay immediately".
struct ReplayAccessOutcome {
  // kOk: |allowed| is authoritative. kUnavailable: no storage, so the caller must refuse rather
  // than guess — every other refusal collapses into ROOM_NOT_FOUND.
  bool ok = false;
  bool allowed = false;
};

class DbActor {
 public:
  // Defined by whichever of db_mysql.cc / db_stub.cc is compiled in. |pool| is the process-wide
  // pool shared by every shard; |slot_index| is the slot this shard's thread will claim.
  DbActor(std::shared_ptr<DbPool> pool, DbConfig config, size_t slot_index);

  // Binds the shard's thread to its pool slot and probes MySQL through a lease. Runs on db_pool, so
  // the connect's blocking resolve/TCP/auth never touches the game pools. The server starts either
  // way.
  stdexec::task<bool> Initialize();

  // Login/Register/FetchStats answer by posting OnLoginResult/OnRegisterResult/OnStatsResult into
  // |reply|; they return nothing, so the caller never awaits MySQL.
  stdexec::task<void> Login(std::string username, std::string password, ex_actor::ActorRef<PlayerActor> reply);
  // `nickname` empty means "use the username".
  stdexec::task<void> Register(std::string username, std::string password, std::string nickname,
                               ex_actor::ActorRef<PlayerActor> reply);
  stdexec::task<void> FetchStats(uint64_t account_id, ex_actor::ActorRef<PlayerActor> reply);
  // Fire and forget: a failed write is logged and never propagates to the game.
  stdexec::task<void> RecordMatch(MatchRecord record);
  // One `match_results` row per account at match end. A duplicate (room_id, account_id) is logged
  // as a WARN and ignored — never retried.
  stdexec::task<void> RecordMatchResult(MatchResultRecord record);
  // One `replays` row per finished match. A duplicate room_id is the ordinary "round finished
  // before the match ended" rewrite, so it is logged at DEBUG and ignored.
  stdexec::task<void> RecordReplay(ReplayRecord record);
  // Answers by return value (unlike the handlers above) because its only caller, ReplayActor::Load,
  // is a coroutine of its own — awaiting here means no reply envelope.
  stdexec::task<ReplayAccessOutcome> ReplayAccess(std::string room_id, uint64_t account_id);

  // Releases the slot: closes the connection and unregisters the thread. Runs on db_pool (the
  // library is thread scoped, so open and close must be on the same thread); idempotent.
  stdexec::task<void> Stop();

  // The method list ex_actor needs to resolve `Send<&DbActor::X>`. It sits after the declarations
  // because a static data member's initializer is not a complete-class context.
  constexpr static auto kActorMethods =
      std::make_tuple(&DbActor::Initialize, &DbActor::Login, &DbActor::Register, &DbActor::FetchStats,
                      &DbActor::RecordMatch, &DbActor::RecordMatchResult, &DbActor::RecordReplay,
                      &DbActor::ReplayAccess, &DbActor::Stop);

 private:
  // Acquire plus the schema bootstrap: a cold start may find MySQL down, so the tables are created
  // on the first lease that actually reaches the server, not only at startup.
  DbLease AcquireReadyLease();
  // Creates the database and the tables. Returns kNone on success, otherwise the classification the
  // caller must charge to the pool.
  DbFailureClass EnsureSchema(void* mysql_handle);
  // Escapes a value for the single-quoted literal it will sit in. The inputs are already
  // charset-validated, so this is the second line of defence, not the first.
  std::string Escape(const DbLease& lease, const std::string& value);
  AuthOutcome LoginJob(const std::string& username, const std::string& password);
  AuthOutcome RegisterJob(const std::string& username, const std::string& password, const std::string& nickname);
  StatsOutcome StatsJob(uint64_t account_id);
  void RecordJob(const MatchRecord& record);
  void RecordMatchResultJob(const MatchResultRecord& record);
  void RecordReplayJob(const ReplayRecord& record);
  ReplayAccessOutcome ReplayAccessJob(const std::string& room_id, uint64_t account_id);

  std::shared_ptr<DbPool> pool_;
  DbConfig config_;
  size_t slot_index_ = 0;
  // True once this shard has created the schema; set on the shard's thread only.
  bool schema_ready_ = false;
};

// mysql_library_init / mysql_library_end are process-wide and not thread safe, so they cannot live
// inside DbActor: a per-actor init/end would tear the library down under another thread's feet.
// main() calls these once around the whole run; each db thread keeps only mysql_thread_init/end.
// No-ops in the db_stub.cc build.
bool DbLibraryInit();
void DbLibraryShutdown();

// A MySQL connection is thread-scoped, so the connection count equals the thread count: every shard
// is one DbActor pinned to its own single-thread pool. This type routes each request by a key.
//
// The keying is what keeps the FIFO property alive once one mailbox no longer serializes everything:
// Register and Login both key on the username (the account id is not known before login), so they
// share a connection; a match record is keyed by account id, exactly like the stats request that must
// include it.
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

  // The replay index has no account (one row per match), so it goes to the first shard. Ordering
  // does not matter: the row is only read by a later stats query.
  [[nodiscard]] auto RecordReplay(ReplayRecord record) const {
    const ex_actor::ActorRef<DbActor> target = Pick(0);
    return target.Send<&DbActor::RecordReplay>(std::move(record));
  }

  // Keyed by the ACCOUNT, not the room, which puts this query in the same mailbox as the RecordMatch
  // writes for that account.
  [[nodiscard]] auto ReplayAccess(std::string room_id, uint64_t account_id) const {
    const ex_actor::ActorRef<DbActor> target = PickByAccount(account_id);
    return target.Send<&DbActor::ReplayAccess>(std::move(room_id), account_id);
  }

 private:
  ex_actor::ActorRef<DbActor> PickByString(std::string_view key) const {
    return Pick(std::hash<std::string_view> {}(key));
  }
  ex_actor::ActorRef<DbActor> PickByAccount(uint64_t account_id) const {
    return Pick(std::hash<uint64_t> {}(account_id));
  }
  // An empty result reproduces the old empty-ActorRef behaviour: Send throws, and callers that
  // check IsEmpty() first never get here.
  ex_actor::ActorRef<DbActor> Pick(size_t hash) const {
    if (shards_.empty()) {
      return ex_actor::ActorRef<DbActor> {};
    }
    return shards_[hash % shards_.size()];
  }

  std::vector<ex_actor::ActorRef<DbActor>> shards_;
};

// The one router for the process. Empty until main() adds the shards, which is also the degraded path
// a test or a build without MySQL runs on.
inline DbService& GetDbService() {
  static DbService service;
  return service;
}

}  // namespace mahjong
