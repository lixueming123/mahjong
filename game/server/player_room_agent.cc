// The room-owned half of a player's protocol: the frames that carry a move into the game.
//
// The handler decodes and shapes the frame, then hands the room the raw frame — the room still
// re-reads it for the game-level checks of §4.5/§6.3 (whose turn, is the tile in hand, is the
// window still open). That split is deliberate: the wire shape is the player's business, the
// game state is the room's.

#include "player_room_agent.h"

#include <optional>
#include <string>
#include <utility>

#include "actor_scope.h"
#include "frame_parser.h"
#include "player_actor.h"
#include "protocol.h"
#include "room_endpoint.h"

namespace mahjong {

// Answers ROOM_NOT_FOUND when this player has no room yet. seq/window matching and "is the tile
// in hand" stay in the room (PROTOCOL.md §4.5, §6.3).
bool PlayerRoomAgent::RequireRoom() {
  if (!player_.room().IsEmpty()) {
    return true;
  }
  player_.SendError(protocol::ErrorCode::kRoomNotFound, "not in a room");
  return false;
}

void PlayerRoomAgent::ForwardToRoom(json::Value frame) {
  ActorSendScope().spawn(player_.room().Send<&RoomEndpoint::Dispatch>(
      RoomEnvelope::GameFrame(player_.session().Info(), std::move(frame))));
}

stdexec::task<void> PlayerRoomAgent::HandleDiscard(json::Value frame) {
  frame_parser::Error parse_error;
  if (!frame_parser::ValidateDiscard(frame, &parse_error)) {
    player_.ReplyParseError(parse_error);
    co_return;
  }
  if (!RequireRoom()) {
    co_return;
  }
  ForwardToRoom(std::move(frame));
  co_return;
}

stdexec::task<void> PlayerRoomAgent::HandleAction(json::Value frame) {
  frame_parser::Error parse_error;
  if (!frame_parser::ValidateAction(frame, &parse_error)) {
    player_.ReplyParseError(parse_error);
    co_return;
  }
  if (!RequireRoom()) {
    co_return;
  }
  ForwardToRoom(std::move(frame));
  co_return;
}

stdexec::task<void> PlayerRoomAgent::HandleAutoplay(json::Value frame) {
  frame_parser::Error parse_error;
  if (!frame_parser::ValidateAutoplay(frame, &parse_error)) {
    player_.ReplyParseError(parse_error);
    co_return;
  }
  if (!RequireRoom()) {
    co_return;
  }
  ForwardToRoom(std::move(frame));
  co_return;
}

}  // namespace mahjong
