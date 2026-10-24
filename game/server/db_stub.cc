// The DbActor used when the build has no MySQL client (-DMAHJONG_WITH_MYSQL=OFF).
//
// Every operation answers "unavailable", which is exactly the behaviour of db_mysql.cc against
// a MySQL that is down (ACCOUNT_DESIGN.md §2.3): the server runs, the game is unaffected, and
// only the account frames report INTERNAL. Keeping the same shape means main() and every call
// site compile and behave identically in both configurations — there is no #ifdef anywhere.

#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "player_actor.h"
#include "db.h"
#include "seat_endpoint.h"

namespace mahjong {

// Nothing to initialise and nothing to tear down without a client library. Reporting success
// keeps the "built without MySQL" warning (below) the one message the operator sees.
bool DbLibraryInit() { return true; }
void DbLibraryShutdown() {}

DbActor::DbActor(DbConfig config) : config_(std::move(config)) {}

stdexec::task<bool> DbActor::Initialize() {
  spdlog::warn("db: built without MySQL support; accounts are disabled, the game itself is unaffected");
  available_ = false;
  co_return false;
}

stdexec::task<void> DbActor::Login(std::string /*username*/, std::string /*password*/,
                                ex_actor::ActorRef<PlayerActor> reply) {
  if (!reply.IsEmpty()) {
    ActorSendScope().spawn(reply.Send<&PlayerActor::Dispatch>(
        PlayerEnvelope::OnLoginResult(AuthOutcome {AuthStatus::kUnavailable, AccountRow {}})));
  }
  co_return;
}

stdexec::task<void> DbActor::Register(std::string /*username*/, std::string /*password*/, std::string /*nickname*/,
                                   ex_actor::ActorRef<PlayerActor> reply) {
  if (!reply.IsEmpty()) {
    ActorSendScope().spawn(reply.Send<&PlayerActor::Dispatch>(
        PlayerEnvelope::OnRegisterResult(AuthOutcome {AuthStatus::kUnavailable, AccountRow {}})));
  }
  co_return;
}

stdexec::task<void> DbActor::FetchStats(uint64_t /*account_id*/, ex_actor::ActorRef<PlayerActor> reply) {
  if (!reply.IsEmpty()) {
    ActorSendScope().spawn(
        reply.Send<&PlayerActor::Dispatch>(PlayerEnvelope::OnStatsResult(StatsOutcome {})));
  }
  co_return;
}

stdexec::task<void> DbActor::RecordMatch(MatchRecord /*record*/) {
  co_return;  // nothing to write to, and a lost row is not a game error (§4.6)
}

stdexec::task<void> DbActor::RecordMatchResult(MatchResultRecord /*record*/) {
  co_return;  // nothing to write to (§2.3 降级语义与 RecordMatch 相同)
}

stdexec::task<void> DbActor::Stop() { co_return; }

}  // namespace mahjong
