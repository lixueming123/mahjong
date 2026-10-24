// DbActor: the only part of the server that talks to MySQL.
//
// ACCOUNT_DESIGN.md §6.1 says an actor handler must never block the actor world's threads.
// MySQL *blocks* (a password check is a round trip), so these actors do not live on the game
// pools at all: main() gives each shard its own dedicated single-thread pool (`db_pool(1)` per
// shard) through `ex_actor::SchedulerUnion` + `ActorConfig::scheduler_index`. Blocking inside one
// of the handlers therefore parks exactly one thread, which nobody else is scheduled on, and the
// handler runs to completion before the next message starts.
//
// Three consequences shape the interface:
//   * A shard's single thread is also its FIFO guarantee (§4.1): a match record is submitted
//     before the client can ask for the stats that must include it, and one mailbox drained in
//     order preserves that for free — *within* a shard. DbService is what makes two requests for
//     one account land on the same shard in the first place.
//   * The MySQL connection is per-thread (mysql_thread_init/mysql_thread_end are thread
//     scoped), so it is opened by `Initialize()` — which runs *on the shard's thread* — and never
//     in the constructor, which the framework runs on the spawning thread. It also means the
//     shard count and the thread count are the same number by construction.
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
//   * db_mysql.cc — the real one, linked against libmysqlclient
//   * db_stub.cc  — used with -DMAHJONG_WITH_MYSQL=OFF; everything reports "unavailable",
//                   which is the same degraded behaviour as a MySQL that is down (§2.3).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <exec/task.hpp>

#include "ex_actor/api.h"
#include "protocol.h"

namespace mahjong {

class PlayerActor;

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
};

// One finished MATCH per account, stored in `match_results` (ACCOUNT_DESIGN.md v1.3 §4 item 7).
// Written exactly once per match (at match_end), one row per account-backed seat; the
// UNIQUE(room_id, account_id) key is the structural defence against double writes.
struct MatchResultRecord {
  uint64_t account_id = 0;
  std::string room_id;
  int hands_played = 0;  // 实际计入局数（D2 口径：无截断 = round_count，截断场更小）
  int dealer_hands = 0;  // 该座位本场坐庄的手数（含连庄的额外手）
  int final_score = 0;   // 本场累计得失分（同 room_id 四行之和 = 0）
  int final_rank = 0;    // 1..4，并列跳号（PROTOCOL.md §5.15）
};

// A zeroed summary is a legitimate answer for a guest (§3.3), so the caller cannot tell a
// failure from "no games played" by the payload alone — hence the explicit flag.
struct StatsOutcome {
  bool ok = false;
  protocol::StatsSummary summary;
};

class DbActor {
 public:
  // Defined by whichever of db_mysql.cc / db_stub.cc is compiled in.
  explicit DbActor(DbConfig config);

  // Opens the connection and creates the schema. Runs on db_pool, so the connect's blocking
  // resolve/TCP/auth never touches the game pools. Returns whether accounts are usable; the
  // server starts either way (§2.3).
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

  // Closes the connection and releases the client library. Runs on db_pool (the library is
  // thread scoped, so opening and closing must happen on the same thread); idempotent.
  stdexec::task<void> Stop();

  // Introspection for main()'s startup log; written only by Initialize()/Stop() on db_pool.
  bool available() const { return available_.load(std::memory_order_relaxed); }

  // The method list ex_actor needs to resolve `SendLocal<&DbActor::X>` (reflect.h). It sits
  // after the declarations because a static data member's initializer is not a complete-class
  // context: naming the methods before they are declared does not compile.
  constexpr static auto kActorMethods =
      std::make_tuple(&DbActor::Initialize, &DbActor::Login, &DbActor::Register, &DbActor::FetchStats,
                      &DbActor::RecordMatch, &DbActor::RecordMatchResult, &DbActor::Stop);

 private:
  // Everything below runs on db_pool's single thread, so no two of them ever overlap and the
  // connection needs no lock (the old worker thread made the same promise).
  bool EnsureConnected();
  bool EnsureSchema();
  void Disconnect();
  // Escapes a value for the single-quoted literal it will sit in (§2.2). The inputs are
  // already charset-validated, so this is the second line of defence, not the first.
  std::string Escape(const std::string& value);
  AuthOutcome LoginJob(const std::string& username, const std::string& password);
  AuthOutcome RegisterJob(const std::string& username, const std::string& password, const std::string& nickname);
  StatsOutcome StatsJob(uint64_t account_id);
  void RecordJob(const MatchRecord& record);
  void RecordMatchResultJob(const MatchResultRecord& record);

  DbConfig config_;
  // Opaque so db.h need not include <mysql.h>. Owned by the db_pool thread alone.
  void* mysql_ = nullptr;
  std::atomic<bool> available_ {false};
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

  [[nodiscard]] auto Login(std::string username, std::string password,
                           ex_actor::ActorRef<PlayerActor> reply) const {
    // `target` is evaluated before the arguments, so moving `username` into the call is safe.
    const ex_actor::ActorRef<DbActor> target = PickByString(username);
    return target.SendLocal<&DbActor::Login>(std::move(username), std::move(password), std::move(reply));
  }

  [[nodiscard]] auto Register(std::string username, std::string password, std::string nickname,
                              ex_actor::ActorRef<PlayerActor> reply) const {
    const ex_actor::ActorRef<DbActor> target = PickByString(username);
    return target.SendLocal<&DbActor::Register>(std::move(username), std::move(password), std::move(nickname),
                                                std::move(reply));
  }

  [[nodiscard]] auto FetchStats(uint64_t account_id, ex_actor::ActorRef<PlayerActor> reply) const {
    const ex_actor::ActorRef<DbActor> target = PickByAccount(account_id);
    return target.SendLocal<&DbActor::FetchStats>(account_id, std::move(reply));
  }

  [[nodiscard]] auto RecordMatch(MatchRecord record) const {
    const ex_actor::ActorRef<DbActor> target = PickByAccount(record.account_id);
    return target.SendLocal<&DbActor::RecordMatch>(std::move(record));
  }

  [[nodiscard]] auto RecordMatchResult(MatchResultRecord record) const {
    const ex_actor::ActorRef<DbActor> target = PickByAccount(record.account_id);
    return target.SendLocal<&DbActor::RecordMatchResult>(std::move(record));
  }

 private:
  ex_actor::ActorRef<DbActor> PickByString(std::string_view key) const {
    return Pick(std::hash<std::string_view> {}(key));
  }
  ex_actor::ActorRef<DbActor> PickByAccount(uint64_t account_id) const {
    return Pick(std::hash<uint64_t> {}(account_id));
  }
  // An empty result reproduces the old empty-ActorRef behaviour: SendLocal throws, and callers
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
