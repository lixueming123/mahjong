// Account storage: the only part of the server that talks to MySQL.
//
// ACCOUNT_DESIGN.md §6.1 is the constraint that shapes this interface: an actor handler must
// never block and must never suspend, so every operation here is asynchronous. The callback
// runs on the store's own worker thread — exactly like WindowTimer's actions — and the caller
// is responsible for posting the outcome back into its actor as a message
// (`ex::start_detached(actor.SendLocal<&T::OnXxx>(...))`). Nothing here touches actor state.
//
// Two implementations exist and CMake picks exactly one, so no #ifdef leaks into the code:
//   * db_mysql.cc — the real one, linked against libmysqlclient
//   * db_stub.cc  — used with -DMAHJONG_WITH_MYSQL=OFF; everything reports "unavailable",
//                   which is the same degraded behaviour as a MySQL that is down (§2.3).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "protocol.h"

namespace mahjong {

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

// A zeroed summary is a legitimate answer for a guest (§3.3), so the caller cannot tell a
// failure from "no games played" by the payload alone — hence the explicit flag.
struct StatsOutcome {
  bool ok = false;
  protocol::StatsSummary summary;
};

class AccountStore {
 public:
  // Defined by whichever of db_mysql.cc / db_stub.cc is compiled in. Never returns null: a
  // store that cannot reach MySQL is still constructed, just unavailable() == true.
  static std::shared_ptr<AccountStore> Create(DbConfig config);

  AccountStore() = default;
  AccountStore(const AccountStore&) = delete;
  AccountStore& operator=(const AccountStore&) = delete;
  virtual ~AccountStore() = default;

  // Whether the startup connection + schema creation succeeded. Read from many threads.
  virtual bool available() const = 0;

  virtual void LoginAsync(std::string username, std::string password, std::function<void(AuthOutcome)> done) = 0;
  // `nickname` empty means "use the username" (§3.2).
  virtual void RegisterAsync(std::string username, std::string password, std::string nickname,
                             std::function<void(AuthOutcome)> done) = 0;
  virtual void FetchStatsAsync(uint64_t account_id, std::function<void(StatsOutcome)> done) = 0;
  // Fire and forget: a failed write is logged and never propagates to the game (§2.3, §4.6).
  virtual void RecordMatchAsync(MatchRecord record) = 0;

  // Stops the worker thread and joins it; idempotent. Must be called before the actor world
  // is torn down, because the callbacks post messages into actors (same rule as WindowTimer).
  virtual void Stop() = 0;
};

}  // namespace mahjong
