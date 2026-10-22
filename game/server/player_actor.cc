#include "connection_actor.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "actor_scope.h"
#include "protocol.h"

namespace ex = stdexec;

namespace mahjong {
namespace {

bool IsKnownActionName(const std::string& action) {
  return action == "pass" || action == "pong" || action == "chow" || action == "kong" || action == "win";
}

bool IsValidSeq(const json::Value* seq) {
  return seq != nullptr && seq->IsInteger() && seq->AsInt() >= 1;
}

// ACCOUNT_DESIGN.md §3.1: the field rules shared by `login`'s account form and `register`.
constexpr char kAccountFieldRules[] =
    "username must be 3..16 characters [A-Za-z0-9_] and password 6..64 printable ASCII characters";

}  // namespace

ConnectionActor::ConnectionActor(std::shared_ptr<Connection> conn, std::shared_ptr<PlayerIdAllocator> player_ids,
                                 std::shared_ptr<AccountStore> accounts, std::shared_ptr<SessionRegistry> sessions,
                                 ex_actor::ActorRef<LobbyActor> lobby)
    : conn_(std::move(conn)),
      player_ids_(std::move(player_ids)),
      accounts_(std::move(accounts)),
      sessions_(std::move(sessions)),
      lobby_(std::move(lobby)) {
  // The framework reaches this actor as `ActorRef<SeatEndpoint>` and invokes the method pointer
  // through a static_cast to the method's class (include/ex_actor/internal/actor.h), which is
  // only correct while SeatEndpoint is the first base — it does not check. Fail loudly here
  // rather than corrupt memory if someone ever adds a base in front of it.
  assert(static_cast<void*>(static_cast<SeatEndpoint*>(this)) == static_cast<void*>(this));
}

// The one public entry (connection_actor.h / seat_endpoint.h). No arm suspends, so this
// coroutine runs to the co_return on the mailbox turn that popped it; the next message cannot
// start in the middle.
exec::task<void> ConnectionActor::Dispatch(ConnectionEnvelope envelope) {
  switch (envelope.kind) {
    case ConnectionEnvelope::Kind::kSetSelfRef:
      SetSelfRef(std::move(envelope.self));
      break;
    case ConnectionEnvelope::Kind::kOnFrame:
      OnFrame(std::move(envelope.text));
      break;
    case ConnectionEnvelope::Kind::kOnClosed:
      OnClosed(std::move(envelope.text));
      break;
    case ConnectionEnvelope::Kind::kOnIdleTimeout:
      OnIdleTimeout();
      break;
    case ConnectionEnvelope::Kind::kOnLoginResult:
      OnLoginResult(std::move(envelope.auth));
      break;
    case ConnectionEnvelope::Kind::kOnRegisterResult:
      OnRegisterResult(std::move(envelope.auth));
      break;
    case ConnectionEnvelope::Kind::kOnStatsResult:
      OnStatsResult(std::move(envelope.stats));
      break;
    case ConnectionEnvelope::Kind::kSetRoom:
      SetRoom(std::move(envelope.room));
      break;
    case ConnectionEnvelope::Kind::kSeatFrame:
      SeatFrame(std::move(envelope.text));
      break;
    case ConnectionEnvelope::Kind::kSeatError:
      SeatError(std::move(envelope.text), envelope.quota);
      break;
    case ConnectionEnvelope::Kind::kClose:
      Close(std::move(envelope.text));
      break;
  }
  co_return;
}

void ConnectionActor::SetSelfRef(ex_actor::ActorRef<ConnectionActor> self) { self_ref_ = std::move(self); }

// SeatEndpoint view (seat_endpoint.h): the room and the lobby reach this actor only through
// these three, so their frames never touch Connection from another thread.
void ConnectionActor::SeatFrame(std::string frame) { conn_->SendFrame(frame); }

void ConnectionActor::SetRoom(ex_actor::ActorRef<RoomActor> room) {
  room_ = std::move(room);
  spdlog::info("{}: room set, gameplay frames go straight there", conn_->Describe());
}

void ConnectionActor::SeatError(std::string frame, QuotaKind quota) {
  // Order matters (PROTOCOL §7.2): the frame must be in the outbox before the quota can ask for
  // a close, or the close-flush would emit nothing.
  conn_->SendFrame(frame);
  if (quota == QuotaKind::kBadFrame) {
    conn_->RecordBadFrame();
  } else if (quota == QuotaKind::kIllegalOp) {
    conn_->RecordIllegalOp();
  }
}

void ConnectionActor::Close(std::string reason) { conn_->RequestClose(std::move(reason)); }

void ConnectionActor::SendError(std::string_view code, std::string_view message) {
  conn_->SendFrame(protocol::ErrorFrame(code, message));
}

void ConnectionActor::BadRequest(std::string_view message) {
  SendError(protocol::kCodeBadRequest, message);
  conn_->RecordBadFrame();
}

void ConnectionActor::IllegalOperation(std::string_view code, std::string_view message) {
  SendError(code, message);
  conn_->RecordIllegalOp();
}

void ConnectionActor::AccountServiceUnavailable() {
  // ACCOUNT_DESIGN.md §2.3: a server-side fault, so no quota is charged (only RecordBadFrame
  // and RecordIllegalOp would, and neither is used here).
  SendError(protocol::kCodeInternal, "account service unavailable");
}

void ConnectionActor::OnFrame(std::string line) {
  try {
    std::string parse_error;
    std::optional<json::Value> parsed = json::Value::Parse(line, &parse_error);
    if (!parsed.has_value()) {
      BadRequest("malformed JSON: " + parse_error);
      return;
    }
    if (!parsed->IsObject()) {
      BadRequest("frame must be a JSON object");
      return;
    }
    const json::Value* type = parsed->Find("type");
    if (type == nullptr || !type->IsString()) {
      BadRequest("frame must carry a string \"type\"");
      return;
    }
    const std::string& type_name = type->AsString();

    if (type_name == "login") {
      HandleLogin(std::move(*parsed));
      return;
    }
    // ACCOUNT_DESIGN.md §3.2: `register` also creates the session, so like `login` it is
    // accepted before there is one.
    if (type_name == "register") {
      HandleRegister(std::move(*parsed));
      return;
    }
    // PROTOCOL.md §4.1: besides those two, nothing is accepted before login.
    if (!logged_in_) {
      SendError(protocol::kCodeNotLoggedIn, "send login first");
      return;
    }

    if (type_name == "ping") {
      conn_->SendFrame(protocol::PongFrame());
    } else if (type_name == "stats_request") {
      HandleStatsRequest();
    } else if (type_name == "list_rooms") {
      ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::ListRooms>(self_ref_));
    } else if (type_name == "create_room") {
      ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::CreateRoom>(self_ref_, Player()));
    } else if (type_name == "join_room") {
      HandleJoinRoom(std::move(*parsed));
    } else if (type_name == "ready") {
      ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::Ready>(self_ref_, Player()));
    } else if (type_name == "sync") {
      ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::Sync>(self_ref_, Player()));
    } else if (type_name == "discard") {
      HandleDiscard(std::move(*parsed));
    } else if (type_name == "action") {
      HandleAction(std::move(*parsed));
    } else {
      BadRequest("unknown frame type: " + type_name);
    }
  } catch (const std::exception& e) {
    // A thrown handler would terminate the detached operation, so nothing escapes here.
    spdlog::error("{}: frame handling failed: {}", conn_->Describe(), e.what());
    SendError(protocol::kCodeInternal, "internal error");
  }
}

void ConnectionActor::HandleLogin(json::Value frame) {
  if (!CanStartAuth()) {
    BadRequest("already logged in");
    return;
  }
  // ACCOUNT_DESIGN.md §3.1: the presence of `username` selects the account form; without it
  // the frame is the frozen name-only guest login, handled exactly as before.
  const json::Value* account_name = frame.Find("username");
  if (account_name != nullptr) {
    const json::Value* password = frame.Find("password");
    if (!account_name->IsString() || password == nullptr || !password->IsString()) {
      BadRequest("account login requires a string \"username\" and a string \"password\"");
      return;
    }
    if (!protocol::IsValidAccountName(account_name->AsString()) ||
        !protocol::IsValidPassword(password->AsString())) {
      BadRequest(kAccountFieldRules);
      return;
    }
    if (accounts_ == nullptr || !accounts_->available() || self_ref_.IsEmpty()) {
      AccountServiceUnavailable();
      return;
    }
    // Asynchronous by necessity (§6.1): the actor submits and returns; OnLoginResult, which
    // runs inside this actor's mailbox, is the only place the session identity changes.
    auth_pending_ = true;
    ex_actor::ActorRef<ConnectionActor> self = self_ref_;
    accounts_->LoginAsync(account_name->AsString(), password->AsString(), [self](AuthOutcome outcome) {
      ActorSendScope().spawn(self.SendLocal<&ConnectionActor::Dispatch>(ConnectionEnvelope::OnLoginResult(std::move(outcome))));
    });
    return;
  }

  const json::Value* name = frame.Find("name");
  if (name == nullptr || !name->IsString()) {
    BadRequest("login requires a string \"name\"");
    return;
  }
  const std::string& requested = name->AsString();
  if (!protocol::IsValidNickname(requested)) {
    SendError(protocol::kCodeInvalidName, "name must be 1..16 UTF-8 characters without control characters");
    int count = conn_->AddInvalidName();
    if (count > protocol::kMaxInvalidNames) {  // the 4th invalid name drops the connection (§1.7)
      conn_->RequestClose("too many invalid names");
    }
    return;
  }

  logged_in_ = true;
  player_id_ = player_ids_->Next();
  name_ = requested;
  spdlog::info("{}: logged in as {} ({})", conn_->Describe(), player_id_, name_);
  conn_->SendFrame(protocol::LoginOkFrame(player_id_, name_));
}

void ConnectionActor::HandleRegister(json::Value frame) {
  if (!CanStartAuth()) {
    BadRequest("already logged in");
    return;
  }
  const json::Value* username = frame.Find("username");
  const json::Value* password = frame.Find("password");
  if (username == nullptr || !username->IsString() || password == nullptr || !password->IsString()) {
    BadRequest("register requires a string \"username\" and a string \"password\"");
    return;
  }
  if (!protocol::IsValidAccountName(username->AsString()) || !protocol::IsValidPassword(password->AsString())) {
    BadRequest(kAccountFieldRules);
    return;
  }
  // §3.2: the nickname is optional and defaults to the username (the DB layer fills it in).
  std::string nickname;
  const json::Value* name = frame.Find("name");
  if (name != nullptr) {
    if (!name->IsString() || !protocol::IsValidNickname(name->AsString())) {
      BadRequest("register's optional \"name\" must be 1..16 UTF-8 characters without control characters");
      return;
    }
    nickname = name->AsString();
  }
  if (accounts_ == nullptr || !accounts_->available() || self_ref_.IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  auth_pending_ = true;
  ex_actor::ActorRef<ConnectionActor> self = self_ref_;
  accounts_->RegisterAsync(username->AsString(), password->AsString(), std::move(nickname),
                           [self](AuthOutcome outcome) {
                             ActorSendScope().spawn(
                                 self.SendLocal<&ConnectionActor::Dispatch>(
                                     ConnectionEnvelope::OnRegisterResult(std::move(outcome))));
                           });
}

void ConnectionActor::HandleStatsRequest() {
  if (account_id_ == 0) {
    // §3.3: a guest has no account, so nothing was ever recorded. A zeroed payload is not an
    // error — the client then needs no special case for guest sessions.
    conn_->SendFrame(protocol::StatsFrame(protocol::StatsSummary {}));
    return;
  }
  if (accounts_ == nullptr || !accounts_->available() || self_ref_.IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  ex_actor::ActorRef<ConnectionActor> self = self_ref_;
  accounts_->FetchStatsAsync(account_id_, [self](StatsOutcome outcome) {
    ActorSendScope().spawn(self.SendLocal<&ConnectionActor::Dispatch>(ConnectionEnvelope::OnStatsResult(std::move(outcome))));
  });
}

void ConnectionActor::OnLoginResult(AuthOutcome outcome) {
  auth_pending_ = false;
  switch (outcome.status) {
    case AuthStatus::kOk:
      AcceptAccount(outcome);
      return;
    case AuthStatus::kAuthFailed:
      // §3.1: an unknown account and a wrong password answer the same thing, and the failure
      // counts toward the frozen illegal-operation quota (§7.2).
      IllegalOperation(protocol::kCodeAuthFailed, "wrong username or password");
      return;
    case AuthStatus::kUsernameTaken:  // not produced by a login
    case AuthStatus::kUnavailable:
      AccountServiceUnavailable();
      return;
  }
}

void ConnectionActor::OnRegisterResult(AuthOutcome outcome) {
  auth_pending_ = false;
  switch (outcome.status) {
    case AuthStatus::kOk:
      AcceptAccount(outcome);  // §3.2: a successful register is a login, answer login_ok
      return;
    case AuthStatus::kUsernameTaken:
      SendError(protocol::kCodeUsernameTaken, "username already taken");  // retryable, no quota
      return;
    case AuthStatus::kAuthFailed:  // not produced by a register
    case AuthStatus::kUnavailable:
      AccountServiceUnavailable();
      return;
  }
}

void ConnectionActor::OnStatsResult(StatsOutcome outcome) {
  if (!outcome.ok) {
    AccountServiceUnavailable();
    return;
  }
  conn_->SendFrame(protocol::StatsFrame(outcome.summary));
}

bool ConnectionActor::AcceptAccount(const AuthOutcome& outcome) {
  // The socket died while the query ran (§6.3). Both checks are needed: the poll thread marks
  // the connection closed before it queues OnClosed, so a result can arrive in the window
  // where closed() is already true but closed_notified_ is not yet set. Claiming here would
  // park the account as "online" with nobody left to release it.
  if (conn_->closed() || closed_notified_) {
    return false;
  }
  if (!sessions_->TryAcquire(outcome.account.id)) {
    SendError(protocol::kCodeAlreadyOnline, "this account is already online");
    return false;
  }
  session_claimed_ = true;
  account_id_ = outcome.account.id;
  player_id_ = AccountPlayerId(outcome.account.id);
  name_ = outcome.account.nickname;
  logged_in_ = true;
  spdlog::info("{}: logged in as {} ({}), account {}", conn_->Describe(), player_id_, name_,
               outcome.account.username);
  conn_->SendFrame(protocol::LoginOkFrame(player_id_, name_));
  return true;
}

void ConnectionActor::ReleaseAccount() {
  if (!session_claimed_) {
    return;
  }
  session_claimed_ = false;
  sessions_->Release(account_id_);
}

void ConnectionActor::HandleJoinRoom(json::Value frame) {
  const json::Value* room_id = frame.Find("room_id");
  if (room_id == nullptr || !room_id->IsString()) {
    BadRequest("join_room requires a string \"room_id\"");
    return;
  }
  ActorSendScope().spawn(
      lobby_.SendLocal<&LobbyActor::JoinRoom>(self_ref_, Player(), room_id->AsString()));
}

void ConnectionActor::HandleDiscard(json::Value frame) {
  const json::Value* tile = frame.Find("tile");
  const json::Value* seq = frame.Find("seq");
  if (tile == nullptr || !tile->IsString() || !IsValidSeq(seq)) {
    BadRequest("discard requires a string \"tile\" and an integer \"seq\" >= 1");
    return;
  }
  if (!protocol::IsValidTileCode(tile->AsString())) {
    IllegalOperation(protocol::kCodeUnknownTile, "invalid tile code: " + tile->AsString());
    return;
  }
  RouteGameFrame(std::move(frame));
}

void ConnectionActor::HandleAction(json::Value frame) {
  const json::Value* action = frame.Find("action");
  const json::Value* tiles = frame.Find("tiles");
  const json::Value* seq = frame.Find("seq");
  if (action == nullptr || !action->IsString() || tiles == nullptr || !tiles->IsArray() || !IsValidSeq(seq)) {
    BadRequest("action requires a string \"action\", an array \"tiles\" and an integer \"seq\" >= 1");
    return;
  }
  if (!IsKnownActionName(action->AsString())) {
    // PROTOCOL.md §4.6 freezes the five action names; an unknown one is a malformed frame.
    // Whether the action is legal *in the current window* is ILLEGAL_ACTION (t4, §4.6/§6.3).
    BadRequest("unknown action: " + action->AsString());
    return;
  }
  for (const json::Value& tile : tiles->AsArray()) {
    if (!tile.IsString()) {
      BadRequest("\"tiles\" must only contain strings");
      return;
    }
    if (!protocol::IsValidTileCode(tile.AsString())) {
      IllegalOperation(protocol::kCodeUnknownTile, "invalid tile code: " + tile.AsString());
      return;
    }
  }
  RouteGameFrame(std::move(frame));
}

// seq/window matching and "is the tile in hand" are game-level checks (PROTOCOL.md §4.5, §6.3)
// owned by RoomActor (t4). The frame goes straight to that room. Fire-and-forget keeps this
// connection answering its own frames meanwhile.
void ConnectionActor::RouteGameFrame(json::Value frame) {
  if (room_.IsEmpty()) {
    SendError(protocol::kCodeRoomNotFound, "not in a room");
    return;
  }
  ActorSendScope().spawn(room_.SendLocal<&RoomActor::Dispatch>(RoomEnvelope::GameFrame(Player(), std::move(frame))));
}

void ConnectionActor::OnClosed(std::string reason) {
  if (closed_notified_) {
    return;
  }
  closed_notified_ = true;
  spdlog::info("{}: closed ({})", conn_->Describe(), reason);
  if (logged_in_) {
    // PROTOCOL.md §1.4: only this connection's state is released; rooms of other players are
    // untouched. LobbyActor drops the room of a disconnecting owner (§8.5).
    ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::OnConnectionClosed>(Player()));
    logged_in_ = false;
  }
  // ACCOUNT_DESIGN.md §1.3: the account becomes available again for a new connection.
  ReleaseAccount();
}

void ConnectionActor::OnIdleTimeout() {
  // PROTOCOL.md §1.6: lobby -> close, in game -> entrusted (stay connected). The lobby owns
  // that decision because only it knows whether the player joined a room.
  ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::OnIdleTimeout>(self_ref_, Player()));
}

}  // namespace mahjong
