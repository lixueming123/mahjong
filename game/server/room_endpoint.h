// RoomEndpoint: the narrow room surface the lobby and the player actors depend on.
//
// The concrete room is a template instantiation (`RoomActorFor<GameT>`); this interface exists
// so the lobby registry can hold one `ActorRef<RoomEndpoint>` per room regardless of the game.
//
// ActorRef requires the concrete actor to derive *directly* from this class (single, non-virtual
// inheritance) so the base subobject stays at offset 0 — the framework casts the actor instance
// pointer to the method pointer's class without a runtime check. `RoomActor` asserts it.

#pragma once

#include "room_contract.h"

namespace mahjong {

class RoomEndpoint {
 public:
  RoomEndpoint() = default;
  RoomEndpoint(const RoomEndpoint&) = delete;
  RoomEndpoint& operator=(const RoomEndpoint&) = delete;
  virtual ~RoomEndpoint() = default;

  virtual stdexec::task<void> Dispatch(RoomEnvelope envelope) = 0;
  virtual stdexec::task<bool> TakeSeat(RoomSeat seat) = 0;
};

}  // namespace mahjong
