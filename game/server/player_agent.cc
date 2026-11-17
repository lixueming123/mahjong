// The player protocol's one registration table (player_agent.h), and the thunks its rows point at.
//
// Everything about "which wire type goes where" is in this file: the table, the array index built
// from it, and the compile-time checks that keep it a bijection over protocol::kClientFrameTypes.
// The agents themselves never see a protocol::FrameType, so a frame is added by editing the table
// below plus one thunk — nothing in the agents, nothing in PlayerActor.

#include "player_agent.h"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include "player_actor.h"

namespace mahjong {
namespace {

constexpr size_t kFrameSlots = static_cast<size_t>(protocol::kFrameTypeMax) + 1;

// ---- the table ---------------------------------------------------------------------------
//
// One row per client frame type, in wire order (PROTOCOL §3.2) so it can be checked against that
// table by eye. The gate is a property of the frame, not of the handler: login and register are
// the only frames accepted before an identity, and nothing else is accepted without one.
constexpr FrameBinding kFrameBindings[] = {
    {.type = protocol::FrameType::kLogin, .gate = FrameGate::kAnonymousOnly, .invoke = &PlayerFrames::Login},
    {.type = protocol::FrameType::kRegister, .gate = FrameGate::kAnonymousOnly, .invoke = &PlayerFrames::Register},
    {.type = protocol::FrameType::kPing, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::Ping},
    {.type = protocol::FrameType::kStatsRequest,
     .gate = FrameGate::kLoggedInOnly,
     .invoke = &PlayerFrames::StatsRequest},
    {.type = protocol::FrameType::kListRooms, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::ListRooms},
    {.type = protocol::FrameType::kCreateRoom, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::CreateRoom},
    {.type = protocol::FrameType::kJoinRoom, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::JoinRoom},
    {.type = protocol::FrameType::kReady, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::Ready},
    {.type = protocol::FrameType::kSync, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::Sync},
    {.type = protocol::FrameType::kDiscard, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::Discard},
    {.type = protocol::FrameType::kAction, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::Action},
    {.type = protocol::FrameType::kAutoplay, .gate = FrameGate::kLoggedInOnly, .invoke = &PlayerFrames::Autoplay},
    {.type = protocol::FrameType::kReplayRequest,
     .gate = FrameGate::kLoggedInOnly,
     .invoke = &PlayerFrames::ReplayRequest},
};

// ---- the index ---------------------------------------------------------------------------

struct FrameIndex {
  std::array<const FrameBinding*, kFrameSlots> by_type {};

  std::optional<const FrameBinding*> Find(protocol::FrameType type) const {
    const size_t slot = static_cast<size_t>(type);
    if (slot >= kFrameSlots) {
      return std::nullopt;
    }
    return by_type[slot];
  }
};

// Folds the table into an array indexed by wire value and refuses at compile time what a count-only
// assert would let through: a row for a server→client type, and two rows for the same type (which
// would silently shadow a missing frame). `throw` inside a constant expression is a compile error,
// and the message is what the compiler reports, so each fault names itself.
constexpr FrameIndex BuildFrameIndex() {
  FrameIndex index;
  for (const FrameBinding& binding : kFrameBindings) {
    const size_t slot = static_cast<size_t>(binding.type);
    if (slot >= kFrameSlots || !protocol::IsClientFrameType(binding.type)) {
      throw "a frame binding names a type that is not a client frame type";
    }
    if (index.by_type[slot] != nullptr) {
      throw "two frame bindings for one frame type";
    }
    index.by_type[slot] = &binding;
  }
  return index;
}

constexpr FrameIndex kFrameIndex = BuildFrameIndex();

// Every row is a distinct client type (proved above), so a count equal to the number of client
// types makes the rows exactly that set: a missing frame and a stray extra both fail here.
static_assert(std::size(kFrameBindings) == std::size(protocol::kClientFrameTypes),
              "every client frame type needs exactly one row in kFrameBindings (player_agent.cc)");

}  // namespace

const FrameBinding* FindClientFrame(protocol::FrameType type) {
  const std::optional<const FrameBinding*> binding = kFrameIndex.Find(type);
  if (!binding.has_value()) {
    return nullptr;
  }
  return *binding;
}

// ---- the thunks --------------------------------------------------------------------------
//
// One line each: the whole point of the table is that the wire type and the business call appear
// side by side exactly once, and neither knows about the other's shape.

stdexec::task<void> PlayerFrames::Login(PlayerActor& player, json::Value frame) {
  co_await player.session_agent().HandleLogin(std::move(frame));
}

stdexec::task<void> PlayerFrames::Register(PlayerActor& player, json::Value frame) {
  co_await player.session_agent().HandleRegister(std::move(frame));
}

stdexec::task<void> PlayerFrames::Ping(PlayerActor& player, json::Value frame) {
  co_await player.session_agent().HandlePing(std::move(frame));
}

stdexec::task<void> PlayerFrames::StatsRequest(PlayerActor& player, json::Value frame) {
  co_await player.session_agent().HandleStatsRequest(std::move(frame));
}

stdexec::task<void> PlayerFrames::ListRooms(PlayerActor& player, json::Value frame) {
  co_await player.lobby_agent().HandleListRooms(std::move(frame));
}

stdexec::task<void> PlayerFrames::CreateRoom(PlayerActor& player, json::Value frame) {
  co_await player.lobby_agent().HandleCreateRoom(std::move(frame));
}

stdexec::task<void> PlayerFrames::JoinRoom(PlayerActor& player, json::Value frame) {
  co_await player.lobby_agent().HandleJoinRoom(std::move(frame));
}

stdexec::task<void> PlayerFrames::Ready(PlayerActor& player, json::Value frame) {
  co_await player.lobby_agent().HandleReady(std::move(frame));
}

stdexec::task<void> PlayerFrames::Sync(PlayerActor& player, json::Value frame) {
  co_await player.lobby_agent().HandleSync(std::move(frame));
}

stdexec::task<void> PlayerFrames::Discard(PlayerActor& player, json::Value frame) {
  co_await player.room_agent().HandleDiscard(std::move(frame));
}

stdexec::task<void> PlayerFrames::Action(PlayerActor& player, json::Value frame) {
  co_await player.room_agent().HandleAction(std::move(frame));
}

stdexec::task<void> PlayerFrames::Autoplay(PlayerActor& player, json::Value frame) {
  co_await player.room_agent().HandleAutoplay(std::move(frame));
}

stdexec::task<void> PlayerFrames::ReplayRequest(PlayerActor& player, json::Value frame) {
  co_await player.replay_agent().HandleReplayRequest(std::move(frame));
}

}  // namespace mahjong
