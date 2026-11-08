// The lobby-owned half of a player's protocol: the frames that only ask LobbyActor to do
// something. Nothing here reads game state or answers on its own — each of these produces
// exactly one message to the lobby, and the lobby answers through this player's seat endpoint.

#include <optional>
#include <string>
#include <utility>

#include "actor_scope.h"
#include "frame_parser.h"
#include "lobby.h"
#include "player_actor.h"
#include "player_agent.h"
#include "protocol.h"

namespace mahjong {

void HandleListRooms(const PlayerServices& services, json::Value) {
  ActorSendScope().spawn(services.lobby.Send<&LobbyActor::ListRooms>(services.self));
}

void HandleCreateRoom(const PlayerServices& services, json::Value frame) {
  // "mode" 可选：缺省 classic。未知值在入口就拒（BAD_REQUEST），不要把一个拼错的玩法带进房间。
  // v2.0：线值是整数（§1.3.1），{1 = classic, 2 = riichi}。
  GameMode mode = GameMode::kClassic;
  if (const json::Value* value = frame.Find("mode"); value != nullptr) {
    if (!value->IsInteger()) {
      services.player.SendError(protocol::ErrorCode::kBadRequest, "\"mode\" must be an integer");
      return;
    }
    const std::optional<protocol::WireMode> parsed = protocol::ParseWireMode(value->AsInt());
    if (!parsed.has_value()) {
      services.player.SendError(protocol::ErrorCode::kBadRequest, "unknown mode: " + std::to_string(value->AsInt()));
      return;
    }
    mode = *parsed == protocol::WireMode::kRiichi ? GameMode::kRiichi : GameMode::kClassic;
  }
  // "length" 可选（东场 tonpu = 1 / 半庄 hanchan = 2），**只对日麻有意义**：
  //   * 不传 ⇒ 东场（与「不传字段」的兼容承诺一致，classic 行为完全不变）；
  //   * classic + hanchan ⇒ BAD_REQUEST（classic 局数由 C15 冻结为 4，不能悄悄忽略一条明确指令）。
  GameLength length = GameLength::kTonpu;
  if (const json::Value* value = frame.Find("length"); value != nullptr) {
    if (!value->IsInteger()) {
      services.player.SendError(protocol::ErrorCode::kBadRequest, "\"length\" must be an integer");
      return;
    }
    const std::optional<protocol::WireLength> parsed = protocol::ParseWireLength(value->AsInt());
    if (!parsed.has_value()) {
      services.player.SendError(protocol::ErrorCode::kBadRequest, "unknown length: " + std::to_string(value->AsInt()));
      return;
    }
    length = *parsed == protocol::WireLength::kHanchan ? GameLength::kHanchan : GameLength::kTonpu;
    if (mode != GameMode::kRiichi && length != GameLength::kTonpu) {
      services.player.SendError(protocol::ErrorCode::kBadRequest, "\"length\" requires mode riichi (2)");
      return;
    }
  }
  ActorSendScope().spawn(
      services.lobby.Send<&LobbyActor::CreateRoomWithMode>(services.self, services.session.Info(), mode, length));
}

void HandleReady(const PlayerServices& services, json::Value) {
  ActorSendScope().spawn(services.lobby.Send<&LobbyActor::Ready>(services.self, services.session.Info()));
}

void HandleSync(const PlayerServices& services, json::Value) {
  ActorSendScope().spawn(services.lobby.Send<&LobbyActor::Sync>(services.self, services.session.Info()));
}

void HandleJoinRoom(const PlayerServices& services, json::Value frame) {
  frame_parser::Error parse_error;
  std::optional<frame_parser::JoinRoom> intent = frame_parser::ParseJoinRoom(frame, &parse_error);
  if (!intent.has_value()) {
    services.player.ReplyParseError(parse_error);
    return;
  }
  ActorSendScope().spawn(
      services.lobby.Send<&LobbyActor::JoinRoom>(services.self, services.session.Info(), std::move(intent->room_id)));
}

}  // namespace mahjong
