// The protocol a player can speak: one binding per client frame type, grouped by who owns it.
//
// A binding is the whole story of a message in one line — its wire type, whether it is accepted
// before the player has an identity, and the function that handles it. That is deliberate: the
// earlier shape had the type in a routing table *and* again in the owning module's handler
// table, so eleven messages cost twenty-two rows and the two could drift apart silently.
//
//   kSessionFrameBindings   login / register / ping / stats_request   the player's own account
//   kLobbyFrameBindings     list_rooms / create_room / join_room / ready / sync   -> LobbyActor
//   kRoomFrameBindings      discard / action                          -> RoomActor
//
// Handlers are free functions rather than members of an agent class: they are stateless, and a
// free function cannot quietly accumulate player state the way a class can. Everything they may
// touch arrives in |PlayerServices|; anything else (the account registry, the database) is
// reached only through the narrow methods PlayerActor exposes, which is what keeps an agent from
// turning into a second god object.

#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "ex_actor/api.h"
#include "json.h"
#include "protocol.h"
#include "session.h"

namespace mahjong {

class PlayerActor;
class LobbyActor;
class RoomActor;

// What a handler is allowed to touch. |player| is for answering this client (frames, errors and
// their §7.2 quota); the rest are the destinations the handler forwards to.
struct PlayerServices {
  PlayerActor& player;
  Session& session;
  ex_actor::ActorRef<LobbyActor>& lobby;
  ex_actor::ActorRef<RoomActor>& room;
  // This player's own address, which every lobby call carries so the lobby can answer it.
  ex_actor::ActorRef<PlayerActor>& self;
};

// ---- session-owned frames (player_session_agent.cc) -----------------------------------
void HandleLogin(const PlayerServices& services, json::Value frame);
void HandleRegister(const PlayerServices& services, json::Value frame);
void HandlePing(const PlayerServices& services, json::Value frame);
void HandleStatsRequest(const PlayerServices& services, json::Value frame);

// ---- lobby-owned frames (player_lobby_agent.cc) ---------------------------------------
void HandleListRooms(const PlayerServices& services, json::Value frame);
void HandleCreateRoom(const PlayerServices& services, json::Value frame);
void HandleJoinRoom(const PlayerServices& services, json::Value frame);
void HandleReady(const PlayerServices& services, json::Value frame);
void HandleSync(const PlayerServices& services, json::Value frame);

// ---- room-owned frames (player_room_agent.cc) -----------------------------------------
void HandleDiscard(const PlayerServices& services, json::Value frame);
void HandleAction(const PlayerServices& services, json::Value frame);

// One client frame type: its wire enum value (PROTOCOL v2.0 §1.3.1), whether an identity is
// required (PROTOCOL.md §4.1), and who handles it.
struct FrameBinding {
  protocol::FrameType type;
  bool requires_login;
  void (*handle)(const PlayerServices&, json::Value);
};

// Declared next to the handlers they name, so a message is spelled exactly once. PlayerActor
// unions the three tables (player_actor.cc) and asserts the total against
// protocol::kClientFrameTypes, so a frame type without a binding does not build.
inline constexpr FrameBinding kSessionFrameBindings[] = {
    {.type = protocol::FrameType::kLogin, .requires_login = false, .handle = &HandleLogin},
    {.type = protocol::FrameType::kRegister, .requires_login = false, .handle = &HandleRegister},
    {.type = protocol::FrameType::kPing, .requires_login = true, .handle = &HandlePing},
    {.type = protocol::FrameType::kStatsRequest, .requires_login = true, .handle = &HandleStatsRequest},
};

inline constexpr FrameBinding kLobbyFrameBindings[] = {
    {.type = protocol::FrameType::kListRooms, .requires_login = true, .handle = &HandleListRooms},
    {.type = protocol::FrameType::kCreateRoom, .requires_login = true, .handle = &HandleCreateRoom},
    {.type = protocol::FrameType::kJoinRoom, .requires_login = true, .handle = &HandleJoinRoom},
    {.type = protocol::FrameType::kReady, .requires_login = true, .handle = &HandleReady},
    {.type = protocol::FrameType::kSync, .requires_login = true, .handle = &HandleSync},
};

inline constexpr FrameBinding kRoomFrameBindings[] = {
    {.type = protocol::FrameType::kDiscard, .requires_login = true, .handle = &HandleDiscard},
    {.type = protocol::FrameType::kAction, .requires_login = true, .handle = &HandleAction},
};

}  // namespace mahjong
