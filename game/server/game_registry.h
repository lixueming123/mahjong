// game_registry: the lobby's "which game?" question, answered in one table.
//
// The lobby must not name a concrete room: `Spawn<RoomActorFor<GameT>>` needs the complete type,
// and a lobby that mentions one game is a lobby that has to be edited for the next one. So the
// only place a game is named is this file: one row per game (wire selector -> spawn function),
// and adding a game is a `Game` implementation plus one row.
//
// Two things live here besides the table, both because they are "how a room is launched":
//
//   * the process-wide pacing knobs (`--ai-delay-ms` / `--inter-hand-delay-ms`). They used to sit
//     in room.h because the monolith read them in its constructor; a launch is exactly what
//     consumes them now, and -1 ("not passed") resolves here so a game's own default still
//     applies when nothing was configured.
//   * the options string each game gets. The wire only offers a mode and a length; a game that
//     needs more would extend its own parser, not this struct.

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

// GAME_DESIGN v1.4 §5.4: a bot's own decision is executed ai_delay_ms after its window opened, so a
// human can follow the table. 0 disables the pacing (the bot decides inline, inside the same
// step). SETTLEMENT_BROADCAST_RULINGS §2.1: the interlude between rounds is a separate, longer
// pause, and the match end uses the same one.
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
// nothing about a game appears here (no wall, no hand, no option).
struct RoomLaunch {
  std::string room_id;
  uint64_t seed = 0;
  int action_timeout_ms = 0;
  std::vector<RoomSeat> seats;
  std::shared_ptr<WindowTimer> timer;
  ex_actor::ActorRef<RoomOwner> owner;
};

// A launch with everything resolved: which game, its configuration, and the pacing it should run
// at (already defaulted, so a game never sees the "not configured" sentinel -1 from here).
struct RoomPlan {
  RoomLaunch launch;
  std::string game_id;
  std::string options_json;
  // The wire's length selector as parsed. It is here (and not only inside |options_json|) because
  // the length is a *launch* choice the older build read directly; a new game reads it from its
  // options string instead.
  GameLength length = GameLength::kTonpu;
  int ai_delay_ms = 0;
  int inter_hand_delay_ms = 0;
};

// The wire's length selector, encoded the way the game reads it. Classic takes no options (C15
// freezes it at four rounds), so it answers with an empty string; riichi needs it.
using GameOptionsFn = std::string (*)(GameLength length);

// One instantiation of `RoomActorFor<GameT>`, as a plain function pointer: the table is a
// compile-time constant, so a registration cannot capture anything.
using RoomSpawnFn = stdexec::task<ex_actor::ActorRef<RoomEndpoint>> (*)(RoomPlan plan);

struct GameEntry {
  GameMode mode;  // the wire selector (create_room's "game" field)
  std::string_view id;  // stable id: the DB `mode` column and the log tag
  GameOptionsFn options;
  RoomSpawnFn spawn;
};

// nullptr when the mode is not registered: the lobby answers a protocol error instead of
// throwing, so an old client asking for a retired game sees a message, not a crash.
const GameEntry* FindGame(GameMode mode);

// Fills the plan's defaults and hands it to the entry's spawn function. The returned ref is
// already narrowed to `RoomEndpoint` — the lobby never sees the concrete room type.
stdexec::task<ex_actor::ActorRef<RoomEndpoint>> LaunchRoom(const GameEntry& entry, RoomLaunch launch, GameLength length);

}  // namespace mahjong
