// The replay-owned half of a player's protocol: one frame, PROTOCOL v2.3 §4.13.
//
// It is its own file for the same reason the other two are: the frame's owner is what decides
// where it goes, and this one goes to ReplayActor rather than to the lobby or the room. Keeping it
// here also means `replay_request` is spelled exactly once — in the binding table next to it.

#include <optional>
#include <string>
#include <utility>

#include "frame_parser.h"
#include "player_actor.h"
#include "player_agent.h"

namespace mahjong {

void HandleReplayRequest(const PlayerServices& services, json::Value frame) {
  frame_parser::Error parse_error;
  std::optional<frame_parser::ReplayRequest> intent = frame_parser::ParseReplayRequest(frame, &parse_error);
  if (!intent.has_value()) {
    services.player.ReplyParseError(parse_error);
    return;
  }
  // Only the id travels: whether this player may read it is a fact about the file's seat table,
  // which PlayerActor hands to the store together with this session's account (player_actor.cc).
  services.player.RequestReplay(std::move(intent->room_id));
}

}  // namespace mahjong
