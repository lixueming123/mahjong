// Shared contract between the lobby, a room and its owner.
//
// Everything here is plain data or a narrow interface: no wall, no hands, no rule. Both the
// classic and the riichi room (and any future game) depend on this header, so it must stay
// free of game-specific fields.
//
// `ActorRef<T>` polymorphism requires the concrete actor to derive *directly* from the
// interface (single, non-virtual inheritance) so the base subobject stays at offset 0; the
// framework casts the actor instance pointer to the method pointer's class without a runtime
// check (seat_endpoint.h documents the same rule for PlayerActor).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "ex_actor/api.h"
#include "json.h"
// RoomEnvelope carries a json::Value, and every envelope must be serializable before it can be
// Send()-ed. The reflect-cpp specialisation is a header-only definition, so it has to be visible
// in every translation unit that instantiates `Send<&RoomEndpoint::Dispatch>` — including this
// one, which is why the contract header pulls it in instead of leaving it to the caller.
#include "json_rfl.h"
#include "player.h"
#include "protocol_enums.h"
#include "seat_endpoint.h"

namespace mahjong {

inline constexpr int kSeatCount = 4;

// 玩法选择器（lobby 的 create_room 解析出来、房间构造再收到）。它属于共享契约而不是某个
// 玩法：它是**选哪个玩法**的线级标识，字段来自 protocol_enums.h，房间本身怎么解释它由各自的
// Game 实现决定。日麻接入见 game/docs/GAME_DESIGN_RIICHI_DRAFT.md：
//   kClassic = 现行玩法（番数制、无立直/宝牌/振听），默认值，行为与之前逐字节一致；
//   kRiichi  = 日麻（符+番、立直、振听、宝牌、点数制）。
// v2.0：线值是整数（§1.3.1），入站解析在 player_lobby_agent.cc 用 protocol::ParseWireMode；
// 内部逻辑永远比较本枚举。落库不再经过它：GameSettlement 直接带玩法的字符串 id。
enum class GameMode : uint8_t { kClassic = 0, kRiichi = 1 };

// 局数（只对日麻有意义；classic 由 C15 冻结为 4 局，传什么都按 4 局走）：
//   kTonpu   = 东场 4 局（默认，短局）
//   kHanchan = 半庄 8 局（东场 4 + 南场 4，场风 0 → 1）
enum class GameLength : uint8_t { kTonpu = 0, kHanchan = 1 };

// Schedules the frozen "window timeout" message: after |delay_ms| the scheduler must post the
// token back to the room. Production installs it in ExActorOnSpawned from the window timer and
// the room's own ActorRef; the unit test injects a capturing one, because a function cannot be
// serialized into a message.
using TimeoutScheduler = std::function<void(int64_t delay_ms, uint64_t token)>;

// One seat's outcome of a finished round. It carries the whole identity (not just the seat) so
// the owner can record the result without asking the room anything (ACCOUNT_DESIGN.md §4).
struct SettledSeat {
  PlayerInfo player;  // account_id == 0 for a guest or a bot
  bool is_bot = false;
  bool winner = false;
  int delta_score = 0;
};

// Everything the owner of a finished round needs. The callback fires once per ROUND, and
// `match_over` says whether the match just ended — only then does the owner start the §1.4.5
// reclaim; the per-round match records are written every time.
struct GameSettlement {
  std::string room_id;
  protocol::RoundEndReason reason = protocol::RoundEndReason::kInvalid;  // 线枚举（§1.3.1）
  // Which game was played, as the game's own stable id ("classic" / "riichi" / ...). It is the
  // DB `mode` column verbatim: the lobby used to translate a protocol::WireMode here, but the
  // contract is game-neutral and a future game has no wire mode to translate.
  std::string game_id;
  std::vector<SettledSeat> seats;  // always kSeatCount entries, in seat order
  bool match_over = false;  // true = the match_end has been/will be broadcast (§5.15)
  // ---- 场终快照（match_over 时有效；lobby 据此写 match_results）----
  int hands_played = 0;                         // 实际计入局数（D2 口径）
  std::array<int, kSeatCount> dealer_hands {};  // 各座位坐庄手数（含连庄）
  std::array<int, kSeatCount> totals {};        // final_score = 本场累计得失分（零和）
  std::array<int, kSeatCount> final_rank {};    // 1..4，并列跳号
};

// Test seam for the per-round notification: a production room sends the settlement to the
// LobbyActor that spawned it (a message, so it can be serialized), while the unit test injects
// this capture callback. Called once per finished round; the §1.4.5 reclaim is armed by the
// lobby only when `settlement.match_over` is true.
using RoomFinishedCallback = std::function<void(const GameSettlement& settlement)>;

// The room's narrow view of its owner, exactly like SeatEndpoint is the room's view of a seat:
// LobbyActor implements it, the room only depends on this interface, and the unit test links
// without the lobby. LobbyActor must derive *directly* from RoomOwner (single, non-virtual
// inheritance) so the base subobject stays at offset 0 — ActorRef casts the concrete actor
// pointer to the method pointer's class without a runtime check.
class RoomOwner {
 public:
  RoomOwner() = default;
  RoomOwner(const RoomOwner&) = delete;
  RoomOwner& operator=(const RoomOwner&) = delete;
  virtual ~RoomOwner() = default;

  // Once per finished round: writes the per-round history and, on `match_over`, arms the
  // reclaim (PROTOCOL.md §1.4.5, ACCOUNT_DESIGN.md §4).
  virtual stdexec::task<void> OnRoomFinished(GameSettlement settlement) = 0;
};

struct RoomSeat {
  int seat = 0;
  PlayerInfo player;
  bool is_bot = false;
  // The player's socket is gone, but the seat is not automated any earlier than the window
  // deadline: a reconnect inside the countdown still owns the decision (§8.5).
  bool disconnected = false;
  // Where this seat's frames go (seat_endpoint.h). Empty for a bot, or for a seat with no wire
  // (the deal-only paths in the unit test) — that is the old `sink == nullptr` sentinel.
  ex_actor::ActorRef<SeatEndpoint> endpoint;
};

// One message into a room. `Dispatch` is the room's message entry point (the only exception is
// the request/response `TakeSeat`), so every caller — LobbyActor's sends, the timeout scheduler
// and the unit tests — builds one of these instead of naming a handler.
//
// As in PlayerEnvelope, the kind is the variant alternative, so a message carries exactly its
// own fields and every alternative is plain data, so the whole envelope can be serialized.
struct RoomEnvelope {
  struct ReadyMsg {  // lobby -> room: this player is ready; deals when the seats are full
    PlayerInfo player;
  };
  struct SyncMsg {  // lobby -> room: resend this player's current view
    PlayerInfo player;
  };
  struct GameFrameMsg {  // a seat's gameplay frame (discard / action)
    PlayerInfo player;
    json::Value frame;
  };
  // The frozen timeout scheme: |token| is a window seq, or the same seq with a pacing/advance
  // bit set (PROTOCOL.md §0.5).
  struct WindowTimeoutMsg {
    uint64_t token;
  };
  struct PlayerDisconnectedMsg {
    PlayerInfo player;
    // The connection that went away. A reconnect replaces the seat's endpoint, so a late close
    // from the old one must not entrust the new session.
    ex_actor::ActorRef<SeatEndpoint> conn;
  };
  struct IdleTimeoutMsg {
    PlayerInfo player;
  };
  // PROTOCOL v2.2 §8.5: a fresh connection has taken over an already-seated player. The room
  // swaps the seat's endpoint, clears `disconnected` and replays the current-phase snapshot.
  struct ReconnectedMsg {
    PlayerInfo player;
    ex_actor::ActorRef<SeatEndpoint> endpoint;
  };

  static RoomEnvelope Ready(PlayerInfo player) { return {ReadyMsg {std::move(player)}}; }
  static RoomEnvelope Sync(PlayerInfo player) { return {SyncMsg {std::move(player)}}; }
  static RoomEnvelope GameFrame(PlayerInfo player, json::Value frame) {
    return {GameFrameMsg {.player = std::move(player), .frame = std::move(frame)}};
  }
  static RoomEnvelope WindowTimeout(uint64_t token) { return {WindowTimeoutMsg {token}}; }
  static RoomEnvelope PlayerDisconnected(PlayerInfo player, ex_actor::ActorRef<SeatEndpoint> conn) {
    return {PlayerDisconnectedMsg {.player = std::move(player), .conn = std::move(conn)}};
  }
  static RoomEnvelope IdleTimeout(PlayerInfo player) { return {IdleTimeoutMsg {std::move(player)}}; }
  static RoomEnvelope Reconnected(PlayerInfo player, ex_actor::ActorRef<SeatEndpoint> endpoint) {
    return {ReconnectedMsg {.player = std::move(player), .endpoint = std::move(endpoint)}};
  }

  std::variant<ReadyMsg, SyncMsg, GameFrameMsg, WindowTimeoutMsg, PlayerDisconnectedMsg, IdleTimeoutMsg, ReconnectedMsg>
      msg;
};

}  // namespace mahjong
