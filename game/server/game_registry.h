// game_registry: the lobby's "which game?" question, answered in one table.
//
// The lobby must not name a concrete room: `Spawn<RoomActorFor<GameT>>` needs the complete type, and
// a lobby that mentions one game would have to be edited for the next one. So this file is the only
// place a game is named: one row per game (wire selector -> spawn function).
//
// The process-wide pacing knobs live here too, because they are "how a room is launched": -1 ("not
// passed") resolves here so a game's own default applies when nothing was configured.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ex_actor/api.h"
#include "room_contract.h"
#include "room_endpoint.h"
#include "timer.h"

namespace mahjong {

// A bot's own decision is executed ai_delay_ms after its window opened, so a human can follow the
// table; 0 means it decides inline, inside the same step. The interlude between rounds is a
// separate, longer pause, and the match end uses the same one.
inline constexpr int kDefaultAiDelayMs = 900;
inline constexpr int kMaxAiDelayMs = 5000;
inline constexpr int kDefaultInterHandDelayMs = 12000;
inline constexpr int kMaxInterHandDelayMs = 60000;

// Process-wide pacing, set once by main() from the command line.
void SetAiActionDelayMs(int delay_ms);
int AiActionDelayMs();
void SetInterHandDelayMs(int delay_ms);
int InterHandDelayMs();

// Everything a room needs that the lobby already holds. Deliberately the lobby's vocabulary:
// nothing about a game appears here.
struct RoomLaunch {
  std::string room_id;
  uint64_t seed = 0;
  int action_timeout_ms = 0;
  std::vector<RoomSeat> seats;
  std::shared_ptr<WindowTimer> timer;
  ex_actor::ActorRef<RoomOwner> owner;
};

// A launch with everything resolved: which game, its configuration, and the pacing it should run at
// (already defaulted, so a game never sees the sentinel -1 from here).
struct RoomPlan {
  RoomLaunch launch;
  std::string game_id;
  std::string options_json;
  // The wire's length selector as parsed; it is here as well as inside |options_json| because the
  // length is a launch choice.
  GameLength length = GameLength::kTonpu;
  int ai_delay_ms = 0;
  int inter_hand_delay_ms = 0;
};

// The wire's length selector, encoded the way the game reads it. Classic takes no options, so it
// answers with an empty string; riichi needs it.
using GameOptionsFn = std::string (*)(GameLength length);

// One instantiation of `RoomActorFor<GameT>`, as a plain function pointer: the table is a
// compile-time constant, so a registration cannot capture anything.
using RoomSpawnFn = stdexec::task<ex_actor::ActorRef<RoomEndpoint>> (*)(RoomPlan plan);

struct GameEntry {
  GameMode mode;        // the wire selector (create_room's "game" field)
  std::string_view id;  // stable id: the DB `mode` column and the log tag
  GameOptionsFn options;
  RoomSpawnFn spawn;
};

// nullptr when the mode is not registered: the lobby answers a protocol error instead of throwing, so
// an old client asking for a retired game sees a message, not a crash.
const GameEntry* FindGame(GameMode mode);

// Fills the plan's defaults and hands it to the entry's spawn function. The returned ref is already
// narrowed to `RoomEndpoint`.
stdexec::task<ex_actor::ActorRef<RoomEndpoint>> LaunchRoom(const GameEntry& entry, RoomLaunch launch,
                                                           GameLength length);

}  // namespace mahjong
