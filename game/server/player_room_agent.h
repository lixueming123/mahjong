// The frames that carry a move into the game: discard, action and autoplay.
//
// The handler decodes and shapes the frame, then hands the room the raw frame — the room still
// re-reads it for the game-level checks of §4.5/§6.3 (whose turn, is the tile in hand, is the
// window still open). That split is deliberate: the wire shape is the player's business, the game
// state is the room's. Like the other agents it knows nothing about wire types; its rows live in
// player_agent.h.

#pragma once

#include <exec/task.hpp>

#include "json.h"

namespace mahjong {

class PlayerActor;

class PlayerRoomAgent {
 public:
  explicit PlayerRoomAgent(PlayerActor& player) : player_(player) {}

  stdexec::task<void> HandleDiscard(json::Value frame);
  stdexec::task<void> HandleAction(json::Value frame);
  stdexec::task<void> HandleAutoplay(json::Value frame);

 private:
  // Answers ROOM_NOT_FOUND when the lobby has not bound a room to this player yet.
  bool RequireRoom();
  void ForwardToRoom(json::Value frame);

  PlayerActor& player_;
};

}  // namespace mahjong
