#include "player_actor.h"

#include <cassert>
#include <exception>
#include <optional>
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
                         std::shared_ptr<SessionRegistry> sessions, ex_actor::ActorRef<LobbyActor> lobby)
    : conn_(std::move(conn)),
      player_ids_(std::move(player_ids)),
      sessions_(std::move(sessions)),
      lobby_(std::move(lobby)) {
  // The framework reaches this actor as `ActorRef<SeatEndpoint>` and invokes the method pointer
  // through a static_cast to the method's class
  // (third_party/ex-actor/include/ex_actor/internal/actor.h), which is only correct while
  // SeatEndpoint is the first base — it does not check. Fail loudly here rather than corrupt
  // memory if someone ever adds a base in front of it.
  assert(static_cast<void*>(static_cast<SeatEndpoint*>(this)) == static_cast<void*>(this));
}

// The one public entry (player_actor.h / seat_endpoint.h).
//
// No gate here, and that is a rule rather than an omission: nothing in this actor locks. A handler
// that runs to completion cannot interleave with another — the mailbox gives that for free — so
// the only hazard is a handler that *suspends*, and the answer is to not depend on actor state
// after a suspension rather than to make every message pay for a lock. PlayerReplayAgent is the one
// that suspends, and it copies the connection and the account id out before it does: what it holds
// across the await is a shared_ptr and a plain integer, neither of which points back here.
//
// The visitor is a plain object rather than a coroutine per arm: the arms that never suspend stay
// allocation-free, and the one that does (OnFrame) is a coroutine of its own.
stdexec::task<void> PlayerActor::Dispatch(PlayerEnvelope envelope) {
  // Frame dispatch is the only arm that can suspend, so it is awaited directly rather than through
  // the visitor below. Keeping it out of the visitor is what leaves every other arm — the seat
  // frames a room broadcasts four times per state change, above all — a plain call with no
  // coroutine frame.
  if (PlayerEnvelope::OnFrameMsg* frame = std::get_if<PlayerEnvelope::OnFrameMsg>(&envelope.msg); frame != nullptr) {
    co_await OnFrame(std::move(frame->line));
    co_return;
  }
  std::visit(overloaded {
                 [this](PlayerEnvelope::SetSelfRefMsg&& m) { SetSelfRef(std::move(m.self)); },
                 [this](PlayerEnvelope::OnClosedMsg&& m) { OnClosed(std::move(m.reason)); },
                 [this](PlayerEnvelope::OnIdleTimeoutMsg&&) { OnIdleTimeout(); },
                 [this](PlayerEnvelope::OnLoginResultMsg&& m) {
                   --db_replies_in_flight_;
                   session_agent_.OnLoginResult(std::move(m.outcome));
                   RetireIfIdle();
                 },
                 [this](PlayerEnvelope::OnRegisterResultMsg&& m) {
                   --db_replies_in_flight_;
                   session_agent_.OnRegisterResult(std::move(m.outcome));
                   RetireIfIdle();
                 },
                 [this](PlayerEnvelope::OnStatsResultMsg&& m) {
                   --db_replies_in_flight_;
                   session_agent_.OnStatsResult(std::move(m.outcome));
                   RetireIfIdle();
                 },
                 [this](PlayerEnvelope::SetRoomMsg&& m) { SetRoom(std::move(m.room)); },
                 [this](PlayerEnvelope::SeatFrameMsg&& m) { SeatFrame(std::move(m.frame)); },
                 [this](PlayerEnvelope::SeatErrorMsg&& m) { SeatError(std::move(m.frame), m.quota); },
                 [this](PlayerEnvelope::CloseMsg&& m) { Close(std::move(m.reason)); },
                 [this](PlayerEnvelope::RetireMsg&&) { RetireRequested(); },
                 [](PlayerEnvelope::OnFrameMsg&&) {},  // handled above
             },
             std::move(envelope.msg));
  co_return;
}

void PlayerActor::SetSelfRef(ex_actor::ActorRef<PlayerActor> self) { self_ref_ = std::move(self); }

// SeatEndpoint view (seat_endpoint.h): the room and the lobby reach this actor only through
// these three, so their frames never touch Connection from another thread.
void PlayerActor::SeatFrame(std::string frame) { conn_->SendFrame(frame); }

void PlayerActor::RetireRequested() {
  // The room's copy of this ref is already gone: the lobby awaits the room's PlayerDisconnected
  // before it retires us, and that handler clears the seat's endpoint. What is left is our own
  // cache of the room, which would otherwise keep a dead room reachable from here.
  room_ = {};
  retire_requested_ = true;
  RetireIfIdle();
}

void PlayerActor::RetireIfIdle() {
  if (!retire_requested_ || retired_ || db_replies_in_flight_ > 0) {
    return;
  }
  // Only an actor the registry owns can be destroyed by it. A test harness spawns players without
  // a lobby, but it still registers them, so the empty self ref is the only case to skip.
  if (self_ref_.IsEmpty()) {
    return;
  }
  // The destroy is a message to ActorRegistryBackend: it can only run after this activation
  // returns, so nothing is destroyed under the handler that asked for it.
  ActorSendScope().spawn(ex_actor::DestroyActor(self_ref_) | ex_actor::DiscardResult());
  retired_ = true;
}

void PlayerActor::SetRoom(ex_actor::ActorRef<RoomEndpoint> room) {
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

void PlayerActor::SendFrame(std::string frame) { conn_->SendFrame(frame); }

void PlayerActor::SendError(protocol::ErrorCode code, std::string_view message) {
  conn_->SendFrame(protocol::ErrorFrame(code, message));
}

void PlayerActor::BadRequest(std::string_view message) {
  SendError(protocol::ErrorCode::kBadRequest, message);
  conn_->RecordBadFrame();
}

void PlayerActor::IllegalOperation(protocol::ErrorCode code, std::string_view message) {
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

bool PlayerActor::AllowFrame(FrameGate gate) {
  switch (gate) {
    case FrameGate::kAnonymousOnly:
      // §3.1: a second login is refused as such rather than as a malformed frame — the two are
      // charged to different §7.2 counters.
      if (!session_.CanStartAuth()) {
        BadRequest("already logged in");
        return false;
      }
      return true;
    case FrameGate::kLoggedInOnly:
      // §4.1: besides login and register, nothing is accepted before an identity.
      if (!session_.logged_in()) {
        SendError(protocol::ErrorCode::kNotLoggedIn, "send login first");
        return false;
      }
      return true;
  }
  return false;
}

// The frame path. It is a coroutine because its handlers are: the replay lookup awaits the store,
// which awaits the account database, and doing it here means no detached coroutine and no captured
// connection. Dispatch holds the gate across this await, so the mailbox cannot start the next
// message while it is parked — that is what makes the suspension safe.
stdexec::task<void> PlayerActor::OnFrame(std::string line) {
  try {
    frame_parser::Error error;
    std::optional<frame_parser::Envelope> envelope = frame_parser::ParseEnvelope(line, &error);
    if (!envelope.has_value()) {
      ReplyParseError(error);
      co_return;
    }
    const FrameBinding* binding = FindClientFrame(envelope->type);
    if (binding == nullptr) {
      // The parse layer validated the integer range (§1.3), so reaching here means the value is
      // either a server→client type or a client type nobody routes — both BAD_REQUEST.
      BadRequest("unknown frame type: " + std::string(protocol::NameOf(envelope->type)));
      co_return;
    }
    if (!AllowFrame(binding->gate)) {
      co_return;
    }
    co_await binding->invoke(*this, std::move(envelope->frame));
  } catch (const std::exception& e) {
    // A thrown handler would terminate the detached operation, so nothing escapes here.
    spdlog::error("{}: frame handling failed: {}", conn_->Describe(), e.what());
    SendError(protocol::ErrorCode::kInternal, "internal error");
  }
  co_return;
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
  // ACCOUNT_DESIGN.md §1.3: the account becomes available again for a new connection.
  if (held_account) {
    sessions_->Release(account_id);
  }
  if (was_logged_in && !lobby_.IsEmpty()) {
    // PROTOCOL.md §1.4: only this connection's state is released; rooms of other players are
    // untouched. LobbyActor drops the room of a disconnecting owner (§8.5). The connection ref
    // carried with the message lets the lobby ignore a close that arrives after a reconnect.
    // The lobby owns the retire: it awaits the room's hand-off before telling this actor to go,
    // because the room holds an endpoint ref until then.
    ActorSendScope().spawn(lobby_.Send<&LobbyActor::OnConnectionClosed>(player, self_ref_));
    return;
  }
  // No lobby hand-off to make (a guest on a build with no lobby, or a session that never claimed
  // an identity), so nobody else will ever ask this actor to go: it retires itself. Without this
  // every accepted socket would leave one actor behind for the life of the process.
  RetireRequested();
}

void PlayerActor::OnIdleTimeout() {
  // PROTOCOL.md §1.6: lobby -> close, in game -> entrusted (stay connected). The lobby owns
  // that decision because only it knows whether the player joined a room.
  ActorSendScope().spawn(lobby_.Send<&LobbyActor::OnIdleTimeout>(self_ref_, Player()));
}

}  // namespace mahjong
