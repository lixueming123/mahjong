#include "player_actor.h"

#include <cassert>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <spdlog/spdlog.h>

#include "actor_scope.h"
#include "frame_parser.h"
#include "overloaded.h"
#include "protocol.h"

namespace ex = stdexec;

namespace mahjong {

PlayerActor::PlayerActor(std::shared_ptr<Connection> conn, std::shared_ptr<PlayerIdAllocator> player_ids,
                                 DbService db, std::shared_ptr<SessionRegistry> sessions,
                                 ex_actor::ActorRef<LobbyActor> lobby)
    : conn_(std::move(conn)),
      player_ids_(std::move(player_ids)),
      db_(std::move(db)),
      sessions_(std::move(sessions)),
      lobby_(std::move(lobby)) {
  // The framework reaches this actor as `ActorRef<SeatEndpoint>` and invokes the method pointer
  // through a static_cast to the method's class (include/ex_actor/internal/actor.h), which is
  // only correct while SeatEndpoint is the first base — it does not check. Fail loudly here
  // rather than corrupt memory if someone ever adds a base in front of it.
  assert(static_cast<void*>(static_cast<SeatEndpoint*>(this)) == static_cast<void*>(this));
}

// The one public entry (player_actor.h / seat_endpoint.h). No arm suspends, so this
// coroutine runs to the co_return on the mailbox turn that popped it; the next message cannot
// start in the middle. The visitor is a plain object rather than a coroutine per arm: nothing
// here awaits, and wrapping the arms in tasks would add a coroutine frame to every message.
stdexec::task<void> PlayerActor::Dispatch(PlayerEnvelope envelope) {
  std::visit(overloaded {
                 [this](PlayerEnvelope::SetSelfRefMsg&& m) { SetSelfRef(std::move(m.self)); },
                 [this](PlayerEnvelope::OnFrameMsg&& m) { OnFrame(std::move(m.line)); },
                 [this](PlayerEnvelope::OnClosedMsg&& m) { OnClosed(std::move(m.reason)); },
                 [this](PlayerEnvelope::OnIdleTimeoutMsg&&) { OnIdleTimeout(); },
                 [this](PlayerEnvelope::OnLoginResultMsg&& m) { OnLoginResult(std::move(m.outcome)); },
                 [this](PlayerEnvelope::OnRegisterResultMsg&& m) { OnRegisterResult(std::move(m.outcome)); },
                 [this](PlayerEnvelope::OnStatsResultMsg&& m) { OnStatsResult(std::move(m.outcome)); },
                 [this](PlayerEnvelope::SetRoomMsg&& m) { SetRoom(std::move(m.room)); },
                 [this](PlayerEnvelope::SeatFrameMsg&& m) { SeatFrame(std::move(m.frame)); },
                 [this](PlayerEnvelope::SeatErrorMsg&& m) { SeatError(std::move(m.frame), m.quota); },
                 [this](PlayerEnvelope::CloseMsg&& m) { Close(std::move(m.reason)); },
             },
             std::move(envelope.msg));
  co_return;
}

void PlayerActor::SetSelfRef(ex_actor::ActorRef<PlayerActor> self) { self_ref_ = std::move(self); }

// SeatEndpoint view (seat_endpoint.h): the room and the lobby reach this actor only through
// these three, so their frames never touch Connection from another thread.
void PlayerActor::SeatFrame(std::string frame) { conn_->SendFrame(frame); }

void PlayerActor::SetRoom(ex_actor::ActorRef<RoomActor> room) {
  room_ = std::move(room);
  spdlog::info("{}: room set, gameplay frames go straight there", conn_->Describe());
}

void PlayerActor::SeatError(std::string frame, QuotaKind quota) {
  // Order matters (PROTOCOL §7.2): the frame must be in the outbox before the quota can ask for
  // a close, or the close-flush would emit nothing.
  conn_->SendFrame(frame);
  if (quota == QuotaKind::kBadFrame) {
    conn_->RecordBadFrame();
  } else if (quota == QuotaKind::kIllegalOp) {
    conn_->RecordIllegalOp();
  }
}

void PlayerActor::Close(std::string reason) { conn_->RequestClose(std::move(reason)); }

void PlayerActor::SendError(std::string_view code, std::string_view message) {
  conn_->SendFrame(protocol::ErrorFrame(code, message));
}

void PlayerActor::BadRequest(std::string_view message) {
  SendError(protocol::kCodeBadRequest, message);
  conn_->RecordBadFrame();
}

void PlayerActor::IllegalOperation(std::string_view code, std::string_view message) {
  SendError(code, message);
  conn_->RecordIllegalOp();
}

// The parser only classifies (frame_parser.h); the three §7.2 counters live here, next to the
// Connection that owns them.
void PlayerActor::ReplyParseError(const frame_parser::Error& error) {
  switch (error.kind) {
    case frame_parser::ErrorKind::kBadRequest:
      BadRequest(error.message);
      return;
    case frame_parser::ErrorKind::kIllegalOp:
      IllegalOperation(error.code, error.message);
      return;
    case frame_parser::ErrorKind::kInvalidName:
      // §1.7 has its own counter and its own drop rule (the 4th invalid name closes), so this
      // is deliberately not routed through BadRequest.
      SendError(error.code, error.message);
      if (conn_->AddInvalidName() > protocol::kMaxInvalidNames) {
        conn_->RequestClose("too many invalid names");
      }
      return;
  }
}

void PlayerActor::AccountServiceUnavailable() {
  // ACCOUNT_DESIGN.md §2.3: a server-side fault, so no quota is charged (only RecordBadFrame
  // and RecordIllegalOp would, and neither is used here).
  SendError(protocol::kCodeInternal, "account service unavailable");
}

namespace {

// The whole client protocol, in the order a frame is looked up: the player's own account frames
// first, then the two agents. The tables themselves live next to the handlers they name
// (player_agent.h), so a message is spelled exactly once — earlier there was a routing table
// here *and* a handler table per module, twenty-two rows for eleven frames, and the two could
// drift apart without anything noticing.
constexpr std::span<const FrameBinding> kFrameTables[] = {kSessionFrameBindings, kLobbyFrameBindings,
                                                          kRoomFrameBindings};

constexpr size_t CountBindings() {
  size_t total = 0;
  for (const std::span<const FrameBinding>& table : kFrameTables) {
    total += table.size();
  }
  return total;
}

// Adding a frame type to protocol::kClientFrameTypes without binding a handler fails to build.
static_assert(CountBindings() == std::size(protocol::kClientFrameTypes),
              "every client frame type needs a binding in player_agent.h");

const FrameBinding* FindBinding(std::string_view type) {
  for (const std::span<const FrameBinding>& table : kFrameTables) {
    for (const FrameBinding& binding : table) {
      if (binding.type == type) {
        return &binding;
      }
    }
  }
  return nullptr;
}

}  // namespace

void PlayerActor::OnFrame(std::string line) {
  try {
    frame_parser::Error error;
    std::optional<frame_parser::Envelope> envelope = frame_parser::ParseEnvelope(line, &error);
    if (!envelope.has_value()) {
      ReplyParseError(error);
      return;
    }

    const FrameBinding* binding = FindBinding(envelope->type);
    if (binding == nullptr) {
      BadRequest("unknown frame type: " + envelope->type);
      return;
    }
    // PROTOCOL.md §4.1: besides login and register, nothing is accepted before an identity.
    if (binding->requires_login && !session_.logged_in()) {
      SendError(protocol::kCodeNotLoggedIn, "send login first");
      return;
    }
    binding->handle(Services(), std::move(envelope->frame));
  } catch (const std::exception& e) {
    // A thrown handler would terminate the detached operation, so nothing escapes here.
    spdlog::error("{}: frame handling failed: {}", conn_->Describe(), e.what());
    SendError(protocol::kCodeInternal, "internal error");
  }
}

// The account entry points handlers call. The "already logged in" guard lives in the handlers
// (they check before parsing, so §7.2 charges the right counter); what stays here is everything
// that needs the database or the identity allocator, which no handler may touch.
void PlayerActor::SendFrame(std::string frame) { conn_->SendFrame(frame); }

void PlayerActor::BeginAccountLogin(std::string username, std::string password) {
  if (db_.IsEmpty() || self_ref_.IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  // Asynchronous by necessity (§6.1): the actor submits to DbActor and returns; OnLoginResult,
  // which runs inside this actor's mailbox, is the only place the session identity changes.
  session_.BeginAuth();
  ActorSendScope().spawn(db_.Login(std::move(username), std::move(password), self_ref_));
}

void PlayerActor::LoginAsGuest(std::string name) {
  session_.LoginAsGuest(player_ids_->Next(), std::move(name));
  spdlog::info("{}: logged in as {} ({})", conn_->Describe(), session_.player_id(), session_.name());
  conn_->SendFrame(protocol::LoginOkFrame(session_.player_id(), session_.name()));
}

void PlayerActor::BeginRegister(std::string username, std::string password, std::string nickname) {
  if (db_.IsEmpty() || self_ref_.IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  session_.BeginAuth();
  ActorSendScope().spawn(db_.Register(std::move(username), std::move(password), std::move(nickname), self_ref_));
}

void PlayerActor::RequestStats() {
  if (session_.account_id() == 0) {
    // §3.3: a guest has no account, so nothing was ever recorded. A zeroed payload is not an
    // error — the client then needs no special case for guest sessions.
    conn_->SendFrame(protocol::StatsFrame(protocol::StatsSummary {}));
    return;
  }
  if (db_.IsEmpty() || self_ref_.IsEmpty()) {
    AccountServiceUnavailable();
    return;
  }
  ActorSendScope().spawn(db_.FetchStats(session_.account_id(), self_ref_));
}

// The two result handlers differ only in what a failure means, and both must leave the session
// retryable: a rejected login cannot strand the connection in kAuthPending, or the client could
// never send a second one (§3.1: retry on a typo is the common case).
void PlayerActor::OnLoginResult(AuthOutcome outcome) {
  switch (outcome.status) {
    case AuthStatus::kOk:
      AcceptAccount(outcome);  // clears the pending phase itself when it cannot take the account
      return;
    case AuthStatus::kAuthFailed:
      // §3.1: an unknown account and a wrong password answer the same thing, and the failure
      // counts toward the frozen illegal-operation quota (§7.2).
      session_.FailAuth();
      IllegalOperation(protocol::kCodeAuthFailed, "wrong username or password");
      return;
    case AuthStatus::kUsernameTaken:  // not produced by a login
    case AuthStatus::kUnavailable:
      session_.FailAuth();
      AccountServiceUnavailable();
      return;
  }
}

void PlayerActor::OnRegisterResult(AuthOutcome outcome) {
  switch (outcome.status) {
    case AuthStatus::kOk:
      AcceptAccount(outcome);  // §3.2: a successful register is a login, answer login_ok
      return;
    case AuthStatus::kUsernameTaken:
      session_.FailAuth();  // retryable, no quota: the client picks another name
      SendError(protocol::kCodeUsernameTaken, "username already taken");
      return;
    case AuthStatus::kAuthFailed:  // not produced by a register
    case AuthStatus::kUnavailable:
      session_.FailAuth();
      AccountServiceUnavailable();
      return;
  }
}

void PlayerActor::OnStatsResult(StatsOutcome outcome) {
  if (!outcome.ok) {
    AccountServiceUnavailable();
    return;
  }
  conn_->SendFrame(protocol::StatsFrame(outcome.summary));
}

bool PlayerActor::AcceptAccount(const AuthOutcome& outcome) {
  // The socket died while the query ran (§6.3). Both checks are needed: the poll thread marks
  // the connection closed before it queues OnClosed, so a result can arrive in the window
  // where closed() is already true but the session is not yet closed. Claiming here would
  // park the account as "online" with nobody left to release it.
  if (conn_->closed() || session_.closed()) {
    return false;
  }
  if (!sessions_->TryAcquire(outcome.account.id)) {
    session_.FailAuth();  // back to kAnonymous so the client may log in as someone else
    SendError(protocol::kCodeAlreadyOnline, "this account is already online");
    return false;
  }
  const std::string player_id = AccountPlayerId(outcome.account.id);
  // Unreachable in practice: the only caller runs in kAuthPending. Rolling back the claim rather
  // than asserting keeps a bad build from parking an account as online forever.
  if (!session_.LoginAsAccount(outcome.account.id, player_id, outcome.account.nickname)) {
    session_.FailAuth();  // ... and from stranding the client in kAuthPending either
    sessions_->Release(outcome.account.id);
    return false;
  }
  spdlog::info("{}: logged in as {} ({}), account {}", conn_->Describe(), session_.player_id(), session_.name(),
               outcome.account.username);
  conn_->SendFrame(protocol::LoginOkFrame(session_.player_id(), session_.name()));
  return true;
}

void PlayerActor::OnClosed(std::string reason) {
  if (session_.closed()) {
    return;
  }
  // Read everything the close path needs before the phase changes: a guest has nobody to tell
  // the lobby about, and only a claimed account has a registry entry to hand back (§1.3).
  const bool was_logged_in = session_.logged_in();
  const bool held_account = session_.holds_account();
  const PlayerInfo player = session_.Info();
  const uint64_t account_id = session_.account_id();
  session_.Close();
  spdlog::info("{}: closed ({})", conn_->Describe(), reason);
  if (was_logged_in) {
    // PROTOCOL.md §1.4: only this connection's state is released; rooms of other players are
    // untouched. LobbyActor drops the room of a disconnecting owner (§8.5).
    ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::OnConnectionClosed>(player));
  }
  // ACCOUNT_DESIGN.md §1.3: the account becomes available again for a new connection.
  if (held_account) {
    sessions_->Release(account_id);
  }
}

void PlayerActor::OnIdleTimeout() {
  // PROTOCOL.md §1.6: lobby -> close, in game -> entrusted (stay connected). The lobby owns
  // that decision because only it knows whether the player joined a room.
  ActorSendScope().spawn(lobby_.SendLocal<&LobbyActor::OnIdleTimeout>(self_ref_, Player()));
}

}  // namespace mahjong
