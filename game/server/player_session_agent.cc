// The frames the player answers itself: its own session and account, with no third actor in the
// picture — plus the database round trip that backs them.
//
// Everything that needs the account registry or the database is driven from here; PlayerActor
// supplies only what an actor can know (its connection, its ref, the retire debt counter), so the
// account flow is testable against a real PlayerActor without a socket and this file never sees
// the frame table.

#include "player_session_agent.h"

#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "frame_parser.h"
#include "player_actor.h"
#include "protocol.h"

namespace mahjong {

stdexec::task<void> PlayerSessionAgent::HandlePing(json::Value) {
  player_.SendFrame(protocol::PongFrame());
  co_return;
}

stdexec::task<void> PlayerSessionAgent::HandleStatsRequest(json::Value) {
  RequestStats();
  co_return;
}

stdexec::task<void> PlayerSessionAgent::HandleLogin(json::Value frame) {
  // The §3.1 "already logged in" guard is the binding's kAnonymousOnly gate, which runs before
  // this parse — that is what keeps a repeat login charged as such rather than as a bad frame.
  frame_parser::Error parse_error;
  std::optional<frame_parser::Login> intent = frame_parser::ParseLogin(frame, &parse_error);
  if (!intent.has_value()) {
    player_.ReplyParseError(parse_error);
    co_return;
  }
  // ACCOUNT_DESIGN.md §3.1: the presence of `username` selected the account form; without it
  // the frame is the frozen name-only guest login.
  if (intent->account_form) {
    BeginAccountLogin(std::move(intent->username), std::move(intent->password), intent->resume);
    co_return;
  }
  LoginAsGuest(std::move(intent->name));
  co_return;
}

stdexec::task<void> PlayerSessionAgent::HandleRegister(json::Value frame) {
  frame_parser::Error parse_error;
  std::optional<frame_parser::Register> intent = frame_parser::ParseRegister(frame, &parse_error);
  if (!intent.has_value()) {
    player_.ReplyParseError(parse_error);
    co_return;
  }
  BeginRegister(std::move(intent->username), std::move(intent->password), std::move(intent->nickname));
  co_return;
}

void PlayerSessionAgent::BeginAccountLogin(std::string username, std::string password, bool resume) {
  DbService& db = GetDbService();
  if (db.IsEmpty() || player_.self().IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  // Asynchronous by necessity (§6.1): the actor submits to DbActor and returns; OnLoginResult,
  // which runs inside this actor's mailbox, is the only place the session identity changes.
  pending_resume_ = resume;
  player_.writable_session().BeginAuth();
  player_.OweDatabaseReply();  // Retire() must refuse destruction until the answer is delivered
  ActorSendScope().spawn(db.Login(std::move(username), std::move(password), player_.self()));
}

void PlayerSessionAgent::LoginAsGuest(std::string name) {
  const std::string player_id = player_.player_ids()->Next();
  player_.writable_session().LoginAsGuest(player_id, std::move(name));
  spdlog::info("{}: logged in as {} ({})", player_.connection()->Describe(), player_.session().player_id(),
               player_.session().name());
  // PROTOCOL v2.2 §5.1: the guest path keeps its frozen frame shape (no in_room).
  player_.SendFrame(protocol::LoginOkFrame(player_.session().player_id(), player_.session().name()));
}

void PlayerSessionAgent::BeginRegister(std::string username, std::string password, std::string nickname) {
  DbService& db = GetDbService();
  if (db.IsEmpty() || player_.self().IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  // A brand-new account has no room to resume; the lobby will answer in_room:false.
  pending_resume_ = true;
  player_.writable_session().BeginAuth();
  player_.OweDatabaseReply();  // see BeginAccountLogin
  ActorSendScope().spawn(db.Register(std::move(username), std::move(password), std::move(nickname), player_.self()));
}

void PlayerSessionAgent::RequestStats() {
  const uint64_t account_id = player_.session().account_id();
  if (account_id == 0) {
    // §3.3: a guest has no account, so nothing was ever recorded. A zeroed payload is not an
    // error — the client then needs no special case for guest sessions.
    player_.SendFrame(protocol::StatsFrame(protocol::StatsSnapshot {}));
    return;
  }
  DbService& db = GetDbService();
  if (db.IsEmpty() || player_.self().IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  player_.OweDatabaseReply();  // OnStatsResult is owed to this mailbox
  ActorSendScope().spawn(db.FetchStats(account_id, player_.self()));
}

void PlayerSessionAgent::OnLoginResult(AuthOutcome outcome) {
  switch (outcome.status) {
    case AuthStatus::kOk:
      AcceptAccount(outcome);  // clears the pending phase itself when it cannot take the account
      return;
    case AuthStatus::kAuthFailed:
      // §3.1: an unknown account and a wrong password answer the same thing, and the failure
      // counts toward the frozen illegal-operation quota (§7.2).
      player_.writable_session().FailAuth();
      player_.IllegalOperation(protocol::ErrorCode::kAuthFailed, "wrong username or password");
      return;
    case AuthStatus::kUsernameTaken:  // not produced by a login
    case AuthStatus::kUnavailable:
      player_.writable_session().FailAuth();
      AccountServiceUnavailable();
      return;
  }
}

void PlayerSessionAgent::OnRegisterResult(AuthOutcome outcome) {
  switch (outcome.status) {
    case AuthStatus::kOk:
      AcceptAccount(outcome);  // §3.2: a successful register is a login, answer login_ok
      return;
    case AuthStatus::kUsernameTaken:
      player_.writable_session().FailAuth();  // retryable, no quota: the client picks another name
      player_.SendError(protocol::ErrorCode::kUsernameTaken, "username already taken");
      return;
    case AuthStatus::kAuthFailed:  // not produced by a register
    case AuthStatus::kUnavailable:
      player_.writable_session().FailAuth();
      AccountServiceUnavailable();
      return;
  }
}

void PlayerSessionAgent::OnStatsResult(StatsOutcome outcome) {
  if (!outcome.ok) {
    AccountServiceUnavailable();
    return;
  }
  player_.SendFrame(protocol::StatsFrame(outcome.snapshot));
}

bool PlayerSessionAgent::AcceptAccount(const AuthOutcome& outcome) {
  // The socket died while the query ran (§6.3). Both checks are needed: the poll thread marks
  // the connection closed before it queues OnClosed, so a result can arrive in the window
  // where closed() is already true but the session is not yet closed. Claiming here would
  // park the account as "online" with nobody left to release it.
  if (player_.connection()->closed() || player_.session().closed()) {
    return false;
  }
  if (!player_.sessions()->TryAcquire(outcome.account.id)) {
    player_.writable_session().FailAuth();  // back to kAnonymous so the client may log in as someone else
    player_.SendError(protocol::ErrorCode::kAlreadyOnline, "this account is already online");
    return false;
  }
  const std::string player_id = AccountPlayerId(outcome.account.id);
  // Unreachable in practice: the only caller runs in kAuthPending. Rolling back the claim rather
  // than asserting keeps a bad build from parking an account as online forever.
  if (!player_.writable_session().LoginAsAccount(outcome.account.id, player_id, outcome.account.nickname)) {
    player_.writable_session().FailAuth();  // ... and from stranding the client in kAuthPending either
    player_.sessions()->Release(outcome.account.id);
    return false;
  }
  spdlog::info("{}: logged in as {} ({}), account {}", player_.connection()->Describe(), player_.session().player_id(),
               player_.session().name(), outcome.account.username);
  // PROTOCOL v2.2 §5.1: the lobby owns room membership, so it answers `login_ok` with the
  // authoritative `in_room` and, when the account is still seated, pushes the room snapshot.
  // An empty self ref only happens in a test harness that never installed it; keep the old
  // direct answer there rather than dereferencing an empty actor ref.
  if (player_.self().IsEmpty()) {
    player_.SendFrame(protocol::LoginOkFrame(player_.session().player_id(), player_.session().name(),
                                             /*in_room=*/false));
    return true;
  }
  ActorSendScope().spawn(
      player_.lobby().Send<&LobbyActor::OnLoggedIn>(player_.self(), player_.session().Info(), pending_resume_));
  return true;
}

void PlayerSessionAgent::AccountServiceUnavailable() {
  // ACCOUNT_DESIGN.md §2.3: a server-side fault, so no quota is charged (only RecordBadFrame
  // and RecordIllegalOp would, and neither is used here).
  player_.SendError(protocol::ErrorCode::kInternal, "account service unavailable");
}

}  // namespace mahjong
