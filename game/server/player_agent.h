// What the player protocol is bound with: the row type its registration table is made of, the two
// gates a row can name, and the lookup function PlayerActor calls.
//
// The table itself lives in player_agent.cc and is the only place that names a wire type next to its
// handler. The agents (player_session_agent.h and friends) are plain business objects: they know
// nothing about protocol::FrameType, gates or the wire, so adding a frame is one row plus one forward
// in that file.
//
// A row is {wire type, gate, thunk}. The thunk takes the player rather than an agent, which keeps
// dispatch generic: PlayerActor looks a frame up, applies the gate, and calls the row.

#pragma once

#include <cstddef>

#include <exec/task.hpp>

#include "json.h"
#include "protocol.h"

namespace mahjong {

class PlayerActor;

// The two accepted identity phases, stated as a property of the frame rather than of the handler:
// keeping this in the table is what lets dispatch gate a frame before its handler parses it, so the
// counter charged for a refusal is the one the gate names, not one a parse error would have.
enum class FrameGate {
  kAnonymousOnly,  // refused with BAD_REQUEST once this session has an identity or one in flight
  kLoggedInOnly,   // refused with NOT_LOGGED_IN until it has one
};

// One registered frame: its wire type, the phase it is accepted in, and the handler that answers it.
struct FrameBinding {
  protocol::FrameType type;
  FrameGate gate;
  stdexec::task<void> (*invoke)(PlayerActor& player, json::Value frame);
};

// The bridge between that table and the agents, which live behind PlayerActor's private surface. One
// static coroutine per registered frame, each a one-line forward to the agent that owns the frame's
// business. It is the friend that lets the table stay a plain constant in player_agent.cc without
// widening PlayerActor's interface or listing thirteen friends.
class PlayerFrames {
 public:
  static stdexec::task<void> Login(PlayerActor& player, json::Value frame);
  static stdexec::task<void> Register(PlayerActor& player, json::Value frame);
  static stdexec::task<void> Ping(PlayerActor& player, json::Value frame);
  static stdexec::task<void> StatsRequest(PlayerActor& player, json::Value frame);
  static stdexec::task<void> ListRooms(PlayerActor& player, json::Value frame);
  static stdexec::task<void> CreateRoom(PlayerActor& player, json::Value frame);
  static stdexec::task<void> JoinRoom(PlayerActor& player, json::Value frame);
  static stdexec::task<void> Ready(PlayerActor& player, json::Value frame);
  static stdexec::task<void> Sync(PlayerActor& player, json::Value frame);
  static stdexec::task<void> Discard(PlayerActor& player, json::Value frame);
  static stdexec::task<void> Action(PlayerActor& player, json::Value frame);
  static stdexec::task<void> Autoplay(PlayerActor& player, json::Value frame);
  static stdexec::task<void> ReplayRequest(PlayerActor& player, json::Value frame);
};

// The row for |type|, or nullptr when nothing answers it (a server→client type, or a value the
// protocol has but no handler for — both BAD_REQUEST). An array index, not a scan.
const FrameBinding* FindClientFrame(protocol::FrameType type);

}  // namespace mahjong
