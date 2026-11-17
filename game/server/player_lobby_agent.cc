// The lobby-owned half of a player's protocol: the frames that only ask LobbyActor to do
// something. Nothing here reads game state or answers on its own — each of these produces
// exactly one message to the lobby, and the lobby answers through this player's seat endpoint.

#include "player_lobby_agent.h"

#include <optional>
#include <string>
#include <utility>

#include "actor_scope.h"
#include "frame_parser.h"
#include "lobby.h"
#include "player_actor.h"
#include "protocol.h"

namespace mahjong {

stdexec::task<void> PlayerLobbyAgent::HandleListRooms(json::Value) {
  ActorSendScope().spawn(player_.lobby().Send<&LobbyActor::ListRooms>(player_.self()));
  co_return;
}

stdexec::task<void> PlayerLobbyAgent::HandleCreateRoom(json::Value frame) {
  // "mode" 可选：缺省 classic。未知值在入口就拒（BAD_REQUEST），不要把一个拼错的玩法带进房间。
  // v2.0：线值是整数（§1.3.1），{1 = classic, 2 = riichi}。
  GameMode mode = GameMode::kClassic;
  if (const json::Value* value = frame.Find("mode"); value != nullptr) {
    if (!value->IsInteger()) {
      player_.SendError(protocol::ErrorCode::kBadRequest, "\"mode\" must be an integer");
      co_return;
    }
    const std::optional<protocol::WireMode> parsed = protocol::ParseWireMode(value->AsInt());
    if (!parsed.has_value()) {
      player_.SendError(protocol::ErrorCode::kBadRequest, "unknown mode: " + std::to_string(value->AsInt()));
      co_return;
    }
    mode = *parsed == protocol::WireMode::kRiichi ? GameMode::kRiichi : GameMode::kClassic;
  }
  // "length" 可选（东场 tonpu = 1 / 半庄 hanchan = 2），**只对日麻有意义**：
  //   * 不传 ⇒ 东场（与「不传字段」的兼容承诺一致，classic 行为完全不变）；
  //   * classic + hanchan ⇒ BAD_REQUEST（classic 局数由 C15 冻结为 4，不能悄悄忽略一条明确指令）。
  GameLength length = GameLength::kTonpu;
  if (const json::Value* value = frame.Find("length"); value != nullptr) {
    if (!value->IsInteger()) {
      player_.SendError(protocol::ErrorCode::kBadRequest, "\"length\" must be an integer");
      co_return;
    }
    const std::optional<protocol::WireLength> parsed = protocol::ParseWireLength(value->AsInt());
    if (!parsed.has_value()) {
      player_.SendError(protocol::ErrorCode::kBadRequest, "unknown length: " + std::to_string(value->AsInt()));
      co_return;
    }
    length = *parsed == protocol::WireLength::kHanchan ? GameLength::kHanchan : GameLength::kTonpu;
    if (mode != GameMode::kRiichi && length != GameLength::kTonpu) {
      player_.SendError(protocol::ErrorCode::kBadRequest, "\"length\" requires mode riichi (2)");
      co_return;
    }
  }
  ActorSendScope().spawn(
      player_.lobby().Send<&LobbyActor::CreateRoomWithMode>(player_.self(), player_.session().Info(), mode, length));
  co_return;
}

stdexec::task<void> PlayerLobbyAgent::HandleReady(json::Value) {
  ActorSendScope().spawn(player_.lobby().Send<&LobbyActor::Ready>(player_.self(), player_.session().Info()));
  co_return;
}

stdexec::task<void> PlayerLobbyAgent::HandleSync(json::Value) {
  ActorSendScope().spawn(player_.lobby().Send<&LobbyActor::Sync>(player_.self(), player_.session().Info()));
  co_return;
}

stdexec::task<void> PlayerLobbyAgent::HandleJoinRoom(json::Value frame) {
  frame_parser::Error parse_error;
  std::optional<frame_parser::JoinRoom> intent = frame_parser::ParseJoinRoom(frame, &parse_error);
  if (!intent.has_value()) {
    player_.ReplyParseError(parse_error);
    co_return;
  }
  ActorSendScope().spawn(player_.lobby().Send<&LobbyActor::JoinRoom>(player_.self(), player_.session().Info(),
                                                                     std::move(intent->room_id)));
  co_return;
}

}  // namespace mahjong
