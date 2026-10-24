// The lobby-owned half of a player's protocol: the frames that only ask LobbyActor to do
// something. Nothing here reads game state or answers on its own — each of these produces
// exactly one message to the lobby, and the lobby answers through this player's seat endpoint.

#include "player_agent.h"

#include <optional>
#include <string>
#include <utility>

#include "actor_scope.h"
#include "frame_parser.h"
#include "lobby.h"
#include "player_actor.h"
#include "protocol.h"

namespace mahjong {

void HandleListRooms(const PlayerServices& services, json::Value) {
  ActorSendScope().spawn(services.lobby.Send<&LobbyActor::ListRooms>(services.self));
}

void HandleCreateRoom(const PlayerServices& services, json::Value) {
  ActorSendScope().spawn(services.lobby.Send<&LobbyActor::CreateRoom>(services.self, services.session.Info()));
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
  ActorSendScope().spawn(services.lobby.Send<&LobbyActor::JoinRoom>(services.self, services.session.Info(),
                                                                        std::move(intent->room_id)));
}

}  // namespace mahjong
