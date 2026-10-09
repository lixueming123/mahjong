// SeatEndpoint: what one seat of a room holds — a place outbound frames can be delivered to.
//
// Why it exists: RoomActor used to push its frames straight into a shared `Connection` outbox
// through a polymorphic SeatSink, which meant the room thread wrote another actor's state and
// `Connection` ended up with four producers on three different threads. Now the room only knows
// `ex_actor::ActorRef<SeatEndpoint>` and delivers by message, so no actor ever writes another
// actor's state. In production the endpoint is the seat's ConnectionActor; in the room's unit
// test it is a capturing actor, which is what keeps that test deterministic (see
// mahjong_room_test.cc).
//
// `ActorRef<SeatEndpoint>` requires the framework's ActorRef polymorphism: the concrete class
// must derive *directly* from SeatEndpoint (single, non-virtual inheritance) so the base
// subobject sits at offset 0 — the framework casts the actor's instance pointer to the method
// pointer's class (include/ex_actor/internal/actor.h), and it does not check that. The
// ConnectionActor constructor asserts it.
//
// There is deliberately ONE envelope type and ONE entry name: `Dispatch`. Two `Dispatch`
// overloads would make `&ConnectionActor::Dispatch` ambiguous the moment someone takes its
// address for SendLocal, so the room's narrow view (frame / error+quota / close) and the
// connection's own messages share this envelope.

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "ex_actor/api.h"

#include "db.h"
#include "player.h"

namespace mahjong {

class ConnectionActor;  // only referenced through ActorRef below
class RoomActor;        // ditto: the connection caches its room's ref (see kSetRoom)

// PROTOCOL.md §7.2's two client-fault counters a *room* error can be charged to. The nickname
// counter is the connection actor's own business and is not routed through here.
enum class QuotaKind {
  kNone,        // ROOM_NOT_FOUND, INTERNAL: the client did nothing wrong
  kBadFrame,    // BAD_REQUEST class
  kIllegalOp,   // NOT_YOUR_TURN / NO_PENDING_ACTION / ILLEGAL_ACTION / UNKNOWN_TILE
};

struct ConnectionEnvelope {
  enum class Kind {
    // gateway -> connection
    kSetSelfRef,
    kOnFrame,
    kOnClosed,
    kOnIdleTimeout,
    // account worker -> connection (ACCOUNT_DESIGN.md §6.1)
    kOnLoginResult,
    kOnRegisterResult,
    kOnStatsResult,
    // lobby -> connection: this connection's room, so gameplay frames skip the lobby
    kSetRoom,
    // room/lobby -> connection (the SeatEndpoint view)
    kSeatFrame,
    kSeatError,
    kClose,
  };

  static ConnectionEnvelope SetSelfRef(ex_actor::ActorRef<ConnectionActor> self) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kSetSelfRef;
    envelope.self = std::move(self);
    return envelope;
  }
  static ConnectionEnvelope OnFrame(std::string line) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kOnFrame;
    envelope.text = std::move(line);
    return envelope;
  }
  static ConnectionEnvelope OnClosed(std::string reason) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kOnClosed;
    envelope.text = std::move(reason);
    return envelope;
  }
  static ConnectionEnvelope OnIdleTimeout() { return ConnectionEnvelope {Kind::kOnIdleTimeout}; }
  static ConnectionEnvelope OnLoginResult(AuthOutcome outcome) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kOnLoginResult;
    envelope.auth = std::move(outcome);
    return envelope;
  }
  static ConnectionEnvelope OnRegisterResult(AuthOutcome outcome) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kOnRegisterResult;
    envelope.auth = std::move(outcome);
    return envelope;
  }
  static ConnectionEnvelope OnStatsResult(StatsOutcome outcome) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kOnStatsResult;
    envelope.stats = std::move(outcome);
    return envelope;
  }
  // The room this connection plays in (an empty ref never happens: the lobby sets it when it
  // registers the room). Gameplay frames go straight to it, so a busy lobby — e.g. one that is
  // suspended inside another player's create_room — cannot delay this connection's frames.
  static ConnectionEnvelope SetRoom(ex_actor::ActorRef<RoomActor> room) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kSetRoom;
    envelope.room = std::move(room);
    return envelope;
  }
  // One frame for this seat (room broadcast or lobby answer).
  static ConnectionEnvelope SeatFrame(std::string frame) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kSeatFrame;
    envelope.text = std::move(frame);
    return envelope;
  }
  // An error frame AND its §7.2 quota in one message, in that order: the frame must be in the
  // outbox before the quota can ask for a close, and one message leaves no room for a reorder.
  static ConnectionEnvelope SeatError(std::string frame, QuotaKind quota) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kSeatError;
    envelope.text = std::move(frame);
    envelope.quota = quota;
    return envelope;
  }
  static ConnectionEnvelope Close(std::string reason) {
    ConnectionEnvelope envelope;
    envelope.kind = Kind::kClose;
    envelope.text = std::move(reason);
    return envelope;
  }

  Kind kind = Kind::kSeatFrame;
  ex_actor::ActorRef<ConnectionActor> self;  // kSetSelfRef
  ex_actor::ActorRef<RoomActor> room;        // kSetRoom
  std::string text;                          // kOnFrame / kOnClosed / kSeatFrame / kSeatError / kClose
  AuthOutcome auth;                          // kOnLoginResult / kOnRegisterResult
  StatsOutcome stats;                        // kOnStatsResult
  QuotaKind quota = QuotaKind::kNone;        // kSeatError
};

class SeatEndpoint {
 public:
  SeatEndpoint() = default;
  SeatEndpoint(const SeatEndpoint&) = delete;
  SeatEndpoint& operator=(const SeatEndpoint&) = delete;
  virtual ~SeatEndpoint() = default;

  // The single door (see the file comment). Implementations take their own AsyncGate first when
  // they can suspend; ConnectionActor's arms never suspend, so it takes the gate and runs.
  virtual exec::task<void> Dispatch(ConnectionEnvelope envelope) = 0;
};

}  // namespace mahjong
