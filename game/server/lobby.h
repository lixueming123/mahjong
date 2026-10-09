// LobbyActor: owns the player -> room mapping and the room registry.
//
// It is the only actor the gateway talks to for non-login traffic (PROTOCOL.md §4.2-§4.4,
// §4.7). Gameplay intents are forwarded to the matching RoomActor; the lobby itself never
// keeps game state.
//
// Message ordering: ex_actor starts the next message as soon as the current handler suspends.
// The registry (`players_`, `rooms_`, `creating_`, `self_ref_`) is only touched while `gate_`
// is held, and the lock is dropped before any `co_await` that does not touch it (`Spawn`, a
// seat `SendLocal`). `CreateRoom` reserves the player in `creating_` under the lock, spawns
// outside it, then commits or aborts under the lock again — a disconnect or a second
// create_room during the spawn cannot both succeed (lobby_test.cc).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "async_gate.h"
#include "connection.h"
#include "db.h"
#include "ex_actor/api.h"
#include "json.h"
#include "player.h"
#include "room.h"
#include "seat_endpoint.h"
#include "timer.h"

namespace mahjong {

// PROTOCOL.md §1.4.5: after game_end the server reclaims the room and its connection unless
// the client disconnects first; this is how long it waits.
inline constexpr int kGameEndReclaimMs = 30000;

class LobbyActor {
 public:
  // |accounts| may be null (unit tests, or a server without account support): the game then
  // simply records no match history (ACCOUNT_DESIGN.md §4).
  LobbyActor(uint64_t seed, int action_timeout_ms, std::shared_ptr<WindowTimer> timer,
             int game_end_reclaim_ms = kGameEndReclaimMs, std::shared_ptr<AccountStore> accounts = nullptr);

  // Each handler locks `gate_` only around the registry access it actually does (async_gate.h).
  // Callers send straight to the method; there is no envelope switch in front of them.

  // The lobby's own address, handed to it once by main() before the gateway accepts anything.
  // It is how the delayed "reclaim this room" message comes back (§1.4.5): an empty ref would
  // make the reclaim's SendLocal throw on the timer thread, so CreateRoom refuses to build a
  // room it could never reclaim.
  exec::task<void> SetSelfRef(ex_actor::ActorRef<LobbyActor> self);

  exec::task<void> CreateRoom(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player);
  exec::task<void> JoinRoom(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player, std::string room_id);
  // Open (not yet started) rooms, so the lobby screen can offer them.
  exec::task<void> ListRooms(ex_actor::ActorRef<SeatEndpoint> conn);
  exec::task<void> Ready(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player);
  exec::task<void> Sync(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player);

  // Game-end reclaim (§1.4.5): drops the room registry entry and closes the connections that
  // belonged to it. Scheduled by the room's game-end notification; idempotent.
  exec::task<void> ReapRoom(std::string room_id);

  // Connection lifecycle (PROTOCOL.md §1.4, §1.6, §8.5).
  exec::task<void> OnConnectionClosed(PlayerInfo player);
  // Lobby: drop the connection. In a room: forward to RoomActor, the seat becomes
  // entrusted and the connection stays open.
  exec::task<void> OnIdleTimeout(ex_actor::ActorRef<SeatEndpoint> conn, PlayerInfo player);

  // Reads the registry, so it takes the gate for that read (not const: acquiring the gate is a
  // mutation of the gate).
  exec::task<size_t> room_count();

 private:
  // Seat-facing replies: the lobby's only way to answer a connection. The §7.2 quota for a
  // BAD_REQUEST is folded into the same message, so the frame is already queued before anything
  // can ask for a close.
  exec::task<void> ReplyFrame(const ex_actor::ActorRef<SeatEndpoint>& conn, std::string frame);
  exec::task<void> ReplyError(const ex_actor::ActorRef<SeatEndpoint>& conn, std::string_view code,
                              std::string_view message);

  struct PlayerState {
    std::string name;
    std::string room_id;  // empty while the player is only in the lobby
    // Kept so the post-game reclaim can close the connection (§1.4.5).
    ex_actor::ActorRef<SeatEndpoint> conn;
  };

  std::string NextRoomId();
  ex_actor::ActorRef<RoomActor> FindRoom(const std::string& room_id) const;
  const PlayerState* FindPlayer(const std::string& player_id) const;

  uint64_t seed_;
  int action_timeout_ms_;
  int game_end_reclaim_ms_;
  uint32_t next_room_number_ = 1;
  std::unordered_map<std::string, PlayerState> players_;
  // player_id -> room_id reserved by a CreateRoom whose Spawn has not committed yet.
  // OnConnectionClosed erases the entry so the spawn's commit sees it is no longer ours.
  std::unordered_map<std::string, std::string> creating_;
  std::unordered_map<std::string, ex_actor::ActorRef<RoomActor>> rooms_;
  // Seat roster of a room that has not started. Seat 0 is the owner; the rest begin as bots
  // and can be taken by JoinRoom until someone sends ready.
  std::unordered_map<std::string, std::vector<RoomSeat>> room_seats_;
  // A room whose game already started survives its owner's disconnect: the seat becomes
  // entrusted and the game keeps running (PROTOCOL.md §8.5).
  std::unordered_map<std::string, bool> room_started_;
  std::shared_ptr<WindowTimer> timer_;
  // Match history (ACCOUNT_DESIGN.md §4). Null when the server runs without accounts.
  std::shared_ptr<AccountStore> accounts_;
  ex_actor::ActorRef<LobbyActor> self_ref_;
  // Serializes registry access only (async_gate.h). Not held across Spawn or a seat send.
  AsyncGate gate_;
};

// Builds the `room_joined` frame of PROTOCOL.md §5.3.
std::string BuildRoomJoinedFrame(const std::string& room_id, int seat, const std::vector<RoomSeat>& seats);

}  // namespace mahjong
