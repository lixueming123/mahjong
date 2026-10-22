// SeatEndpoint: what one seat of a room holds — a place outbound frames can be delivered to.
//
// Why it exists: RoomActor used to push its frames straight into a shared `Connection` outbox
// through a polymorphic SeatSink, which meant the room thread wrote another actor's state and
// `Connection` ended up with four producers on three different threads. Now the room only knows
// `ex_actor::ActorRef<SeatEndpoint>` and delivers by message, so no actor ever writes another
// actor's state. In production the endpoint is the seat's PlayerActor; in the room's unit
// test it is a capturing actor, which is what keeps that test deterministic (see
// mahjong_room_test.cc).
//
// `ActorRef<SeatEndpoint>` requires the framework's ActorRef polymorphism: the concrete class
// must derive *directly* from SeatEndpoint (single, non-virtual inheritance) so the base
// subobject sits at offset 0 — the framework casts the actor's instance pointer to the method
// pointer's class (include/ex_actor/internal/actor.h), and it does not check that. The
// PlayerActor constructor asserts it.
//
// There is deliberately ONE envelope type and ONE entry name: `Dispatch`. Two `Dispatch`
// overloads would make `&PlayerActor::Dispatch` ambiguous the moment someone takes its
// address for SendLocal, so the room's narrow view (frame / error+quota / close) and the
// connection's own messages share this envelope.
//
// Inside that one envelope the kind IS the type: each message has its own struct and they live
// in `msg` as a `std::variant`, so a message carries exactly the fields it uses. The flat
// version was 232 bytes for *every* message — a seat frame (the room's broadcast, four of them
// per state change) paid for both ActorRefs and the auth/stats payloads on each of the ~5
// by-value moves a message makes on its way into the mailbox (`ActorRef::SendLocal`). The
// variant is 72 bytes and the seat frame inside it is one std::string.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include "ex_actor/api.h"

#include "db.h"
#include "player.h"

namespace mahjong {

class PlayerActor;  // only referenced through ActorRef below
class RoomActor;        // ditto: the connection caches its room's ref (see kSetRoom)

// PROTOCOL.md §7.2's two client-fault counters a *room* error can be charged to. The nickname
// counter is the connection actor's own business and is not routed through here.
enum class QuotaKind {
  kNone,        // ROOM_NOT_FOUND, INTERNAL: the client did nothing wrong
  kBadFrame,    // BAD_REQUEST class
  kIllegalOp,   // NOT_YOUR_TURN / NO_PENDING_ACTION / ILLEGAL_ACTION / UNKNOWN_TILE
};

struct PlayerEnvelope {
  // One struct per kind; whichever one is engaged in `msg` is the kind, so there is no separate
  // tag to keep in sync with the payload (and none to fall out of step with it).
  struct SetSelfRefMsg {  // gateway -> connection
    ex_actor::ActorRef<PlayerActor> self;
  };
  struct OnFrameMsg {
    std::string line;
  };
  struct OnClosedMsg {
    std::string reason;
  };
  struct OnIdleTimeoutMsg {};
  struct OnLoginResultMsg {  // account worker -> connection (ACCOUNT_DESIGN.md §6.1)
    AuthOutcome outcome;
  };
  struct OnRegisterResultMsg {
    AuthOutcome outcome;
  };
  struct OnStatsResultMsg {
    StatsOutcome outcome;
  };
  // lobby -> connection: this connection's room, so gameplay frames skip the lobby
  struct SetRoomMsg {
    ex_actor::ActorRef<RoomActor> room;
  };
  // room/lobby -> connection (the SeatEndpoint view): one frame for this seat (room broadcast
  // or lobby answer).
  struct SeatFrameMsg {
    std::string frame;
  };
  // An error frame AND its §7.2 quota in one message, in that order: the frame must be in the
  // outbox before the quota can ask for a close, and one message leaves no room for a reorder.
  struct SeatErrorMsg {
    std::string frame;
    QuotaKind quota;
  };
  struct CloseMsg {
    std::string reason;
  };

  static PlayerEnvelope SetSelfRef(ex_actor::ActorRef<PlayerActor> self) {
    return {SetSelfRefMsg {std::move(self)}};
  }
  static PlayerEnvelope OnFrame(std::string line) { return {OnFrameMsg {std::move(line)}}; }
  static PlayerEnvelope OnClosed(std::string reason) { return {OnClosedMsg {std::move(reason)}}; }
  static PlayerEnvelope OnIdleTimeout() { return {OnIdleTimeoutMsg {}}; }
  static PlayerEnvelope OnLoginResult(AuthOutcome outcome) { return {OnLoginResultMsg {std::move(outcome)}}; }
  static PlayerEnvelope OnRegisterResult(AuthOutcome outcome) {
    return {OnRegisterResultMsg {std::move(outcome)}};
  }
  static PlayerEnvelope OnStatsResult(StatsOutcome outcome) { return {OnStatsResultMsg {std::move(outcome)}}; }
  // The room this connection plays in (an empty ref never happens: the lobby sets it when it
  // registers the room). Gameplay frames go straight to it, so a busy lobby — e.g. one that is
  // suspended inside another player's create_room — cannot delay this connection's frames.
  static PlayerEnvelope SetRoom(ex_actor::ActorRef<RoomActor> room) { return {SetRoomMsg {std::move(room)}}; }
  static PlayerEnvelope SeatFrame(std::string frame) { return {SeatFrameMsg {std::move(frame)}}; }
  static PlayerEnvelope SeatError(std::string frame, QuotaKind quota) {
    return {SeatErrorMsg {.frame = std::move(frame), .quota = quota}};
  }
  static PlayerEnvelope Close(std::string reason) { return {CloseMsg {std::move(reason)}}; }

  std::variant<SetSelfRefMsg, OnFrameMsg, OnClosedMsg, OnIdleTimeoutMsg, OnLoginResultMsg, OnRegisterResultMsg,
               OnStatsResultMsg, SetRoomMsg, SeatFrameMsg, SeatErrorMsg, CloseMsg>
      msg;
};

class SeatEndpoint {
 public:
  SeatEndpoint() = default;
  SeatEndpoint(const SeatEndpoint&) = delete;
  SeatEndpoint& operator=(const SeatEndpoint&) = delete;
  virtual ~SeatEndpoint() = default;

  // The single door (see the file comment). Implementations take their own AsyncGate first when
  // they can suspend; PlayerActor's arms never suspend, so it takes the gate and runs.
  virtual exec::task<void> Dispatch(PlayerEnvelope envelope) = 0;
};

}  // namespace mahjong
