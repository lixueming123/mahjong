// The room-owned half of a player's protocol: the two frames that carry a move into the game.
//
// The handler decodes and shapes the frame, then hands the room the raw frame — the room still
// re-reads it for the game-level checks of §4.5/§6.3 (whose turn, is the tile in hand, is the
// window still open). That split is deliberate: the wire shape is the player's business, the
// game state is the room's.

#include <optional>
#include <string>
#include <utility>

#include "actor_scope.h"
#include "frame_parser.h"
#include "player_actor.h"
#include "player_agent.h"
#include "protocol.h"
#include "room_endpoint.h"

namespace mahjong {
namespace {

// Answers ROOM_NOT_FOUND when this player has no room yet. seq/window matching and "is the tile
// in hand" stay in the room (PROTOCOL.md §4.5, §6.3).
bool HasRoom(const PlayerServices& services) {
  if (!services.room.IsEmpty()) {
    return true;
  }
  services.player.SendError(protocol::ErrorCode::kRoomNotFound, "not in a room");
  return false;
}

void ForwardToRoom(const PlayerServices& services, json::Value frame) {
  ActorSendScope().spawn(
      services.room.Send<&RoomEndpoint::Dispatch>(RoomEnvelope::GameFrame(services.session.Info(), std::move(frame))));
}

}  // namespace

void HandleDiscard(const PlayerServices& services, json::Value frame) {
  frame_parser::Error parse_error;
  if (!frame_parser::ValidateDiscard(frame, &parse_error)) {
    services.player.ReplyParseError(parse_error);
    return;
  }
  if (!HasRoom(services)) {
    return;
  }
  ForwardToRoom(services, std::move(frame));
}

void HandleAction(const PlayerServices& services, json::Value frame) {
  frame_parser::Error parse_error;
  if (!frame_parser::ValidateAction(frame, &parse_error)) {
    services.player.ReplyParseError(parse_error);
    return;
  }
  if (!HasRoom(services)) {
    return;
  }
  ForwardToRoom(services, std::move(frame));
}

void HandleAutoplay(const PlayerServices& services, json::Value frame) {
  frame_parser::Error parse_error;
  if (!frame_parser::ValidateAutoplay(frame, &parse_error)) {
    services.player.ReplyParseError(parse_error);
    return;
  }
  if (!HasRoom(services)) {
    return;
  }
  ForwardToRoom(services, std::move(frame));
}

}  // namespace mahjong
