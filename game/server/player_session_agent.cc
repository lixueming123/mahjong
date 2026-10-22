// The frames the player answers itself: its own session and account, with no third actor in the
// picture. Everything that needs the account registry or the database goes through a narrow
// PlayerActor method, so this file never sees either.

#include <optional>
#include <string>
#include <utility>

#include "frame_parser.h"
#include "player_actor.h"
#include "player_agent.h"
#include "protocol.h"

namespace mahjong {

void HandlePing(const PlayerServices& services, json::Value) {
  services.player.SendFrame(protocol::PongFrame());
}

void HandleStatsRequest(const PlayerServices& services, json::Value) {
  services.player.RequestStats();
}

void HandleLogin(const PlayerServices& services, json::Value frame) {
  // Checked before parsing so a second login is refused as "already logged in"; a malformed
  // frame is charged to a different §7.2 counter (§3.1).
  if (!services.player.CanStartAuth()) {
    services.player.BadRequest("already logged in");
    return;
  }
  frame_parser::Error parse_error;
  std::optional<frame_parser::Login> intent = frame_parser::ParseLogin(frame, &parse_error);
  if (!intent.has_value()) {
    services.player.ReplyParseError(parse_error);
    return;
  }
  // ACCOUNT_DESIGN.md §3.1: the presence of `username` selected the account form; without it
  // the frame is the frozen name-only guest login.
  if (intent->account_form) {
    services.player.BeginAccountLogin(std::move(intent->username), std::move(intent->password));
    return;
  }
  services.player.LoginAsGuest(std::move(intent->name));
}

void HandleRegister(const PlayerServices& services, json::Value frame) {
  if (!services.player.CanStartAuth()) {
    services.player.BadRequest("already logged in");
    return;
  }
  frame_parser::Error parse_error;
  std::optional<frame_parser::Register> intent = frame_parser::ParseRegister(frame, &parse_error);
  if (!intent.has_value()) {
    services.player.ReplyParseError(parse_error);
    return;
  }
  services.player.BeginRegister(std::move(intent->username), std::move(intent->password),
                                std::move(intent->nickname));
}

}  // namespace mahjong
