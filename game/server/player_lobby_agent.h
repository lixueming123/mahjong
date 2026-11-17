// The frames that only ask LobbyActor to do something: list rooms, create, join, ready and sync.
//
// Nothing here reads game state or answers on its own — each handler produces exactly one message
// to the lobby, and the lobby answers through this player's seat endpoint. That is why the whole
// area is five short methods and no state. Like the other agents it knows nothing about wire types;
// its rows live in player_agent.h.

#pragma once

#include <exec/task.hpp>

#include "json.h"

namespace mahjong {

class PlayerActor;

class PlayerLobbyAgent {
 public:
  explicit PlayerLobbyAgent(PlayerActor& player) : player_(player) {}

  stdexec::task<void> HandleListRooms(json::Value frame);
  stdexec::task<void> HandleCreateRoom(json::Value frame);
  stdexec::task<void> HandleJoinRoom(json::Value frame);
  stdexec::task<void> HandleReady(json::Value frame);
  stdexec::task<void> HandleSync(json::Value frame);

 private:
  PlayerActor& player_;
};

}  // namespace mahjong
