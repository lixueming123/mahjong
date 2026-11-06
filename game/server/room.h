// RoomActor: the authoritative state machine of one room (t4).
//
// Responsibilities:
//   * single source of truth: wall, four hands, discards, melds, current seat and seq;
//   * turn driving: deal -> draw -> discard -> claim window -> meld or next player;
//   * action windows and the frozen "seq invalidation" timeout scheme (PROTOCOL.md §0.5/§6);
//   * three server-side AI seats (synchronous decisions, never awaiting another actor);
//   * settlement through the pure rules library (game/docs/RULESET.md, H8).
//
// Threading model: every message arrives through `Dispatch` (or the request/response
// `TakeSeat`), each holding the actor's gate for the whole handler (async_gate.h), so exactly
// one transition is in flight at a time and the state machine needs no locks of its own. That
// is what the framework does not give on its own: it starts the next message as soon as a
// handler suspends, and the room is expected to start suspending (see the Deliver note in
// room.cc), so the gate has to be taken here. The introspection getters below are deliberately
// outside that rule — nothing but the unit tests and logging reads room state from outside.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "async_gate.h"
#include "ex_actor/api.h"
#include "json.h"
#include "json_rfl.h"
#include "mahjong_rules.h"
#include "player.h"
#include "riichi_rules.h"
#include "seat_endpoint.h"
#include "timer.h"

namespace mahjong {

inline constexpr int kSeatCount = 4;

// 玩法（mode）。日麻接入见 game/docs/GAME_DESIGN_RIICHI_DRAFT.md：
//   kClassic = 现行玩法（番数制、无立直/宝牌/振听），默认值，行为与之前逐字节一致；
//   kRiichi  = 日麻（符+番、立直、振听、宝牌、点数制）。
enum class GameMode : uint8_t { kClassic = 0, kRiichi = 1 };

// 协议里的玩法名：create_room 的 "mode" 字段。未知值返回 nullopt（调用方回 BAD_REQUEST）。
std::optional<GameMode> ParseGameMode(std::string_view name);
std::string_view GameModeName(GameMode mode);

// 局数（只对日麻有意义；classic 由 C15 冻结为 4 局，传什么都按 4 局走）：
//   kTonpu   = 东场 4 局（默认，短局）
//   kHanchan = 半庄 8 局（东场 4 + 南场 4，场风 0 → 1）
enum class GameLength : uint8_t { kTonpu = 0, kHanchan = 1 };

std::optional<GameLength> ParseGameLength(std::string_view name);
std::string_view GameLengthName(GameLength length);
// 这个玩法下的一共几局：classic 恒 4（C15），日麻按 kTonpu/kHanchan 取 4/8。
int RoundCountOf(GameMode mode, GameLength length);

// Schedules the frozen "window timeout" message: after |delay_ms| the scheduler must call
// RoomActor::OnWindowTimeout(|window_seq|). Production installs it in ExActorOnSpawned from the
// window timer and the room's own ActorRef; the unit test injects a capturing one, because a
// function cannot be serialized into a message.
using TimeoutScheduler = std::function<void(int64_t delay_ms, uint64_t window_seq)>;

// One seat's outcome of a finished round. It carries the whole identity (not just the seat) so
// the owner can record the result without asking the room anything (ACCOUNT_DESIGN.md §4).
struct SettledSeat {
  PlayerInfo player;  // account_id == 0 for a guest or a bot
  bool is_bot = false;
  bool winner = false;
  int delta_score = 0;
};

// Everything the owner of a finished round needs. v1.6（一场四局）: the callback fires once per
// ROUND (4..8 times per match), and `match_over` says whether the match just ended — only then
// does the owner start the §1.4.5 reclaim; the per-round match records are written every time.
struct GameSettlement {
  std::string room_id;
  std::string reason;              // win_ron / win_tsumo / draw_exhausted
  std::string mode;                // classic / riichi（lobby 写战绩时落库）
  std::vector<SettledSeat> seats;  // always kSeatCount entries, in seat order
  bool match_over = false;         // true = the match_end has been/will be broadcast (§5.15)
  // ---- v1.3 场终快照（match_over 时有效；lobby 据此写 match_results，B3）----
  int hands_played = 0;                         // 实际计入局数（D2 口径）
  std::array<int, kSeatCount> dealer_hands {};  // 各座位坐庄手数（含连庄）
  std::array<int, kSeatCount> totals {};        // final_score = 本场累计得失分（零和）
  std::array<int, kSeatCount> final_rank {};    // 1..4，并列跳号
};

// Test seam for the per-round notification: a production room sends the settlement to the
// LobbyActor that spawned it (a message, so it can be serialized), while the unit test injects
// this capture callback. Called once per finished round (v1.6: 4..8 times per match); the
// §1.4.5 reclaim is armed by the lobby only when `settlement.match_over` is true.
using RoomFinishedCallback = std::function<void(const GameSettlement& settlement)>;

// The room's narrow view of its owner, exactly like SeatEndpoint is the room's view of a seat:
// LobbyActor implements it, the room only depends on this interface, and the unit test links
// without the lobby. LobbyActor must derive *directly* from RoomOwner (single, non-virtual
// inheritance) so the base subobject stays at offset 0 — ActorRef casts the concrete actor
// pointer to the method pointer's class without a runtime check (seat_endpoint.h has the same
// rule, and LobbyActor's constructor asserts it).
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

// Derives one room's wall seed from the server-level seed and the room's 0-based creation
// index (RoomActor's caller, LobbyActor, passes exactly this).
//
// Why this shape:
//   * it is a pure function of (base_seed, room_index) — no clock, no RNG, no
//     std::hash<std::string> (whose value is not guaranteed stable across runs/builds), so a
//     fixed --seed replays the very same sequence of walls and the e2e statistics stay
//     reproducible;
//   * it is the identity for room_index 0: a single-room server (every test and every
//     `e2e_full_game.py --games N` run) deals exactly the wall it dealt before this change;
//   * odd multiplication + SplitMix64's finalizer are both bijections on 64 bits, so
//     distinct room indices cannot collide, and XOR-ing a fixed base seed keeps two
//     different base seeds apart for the same room index.
inline uint64_t DeriveRoomSeed(uint64_t base_seed, uint32_t room_index) {
  uint64_t mixed = static_cast<uint64_t>(room_index) * 0x9E3779B97F4A7C15ull;  // odd => bijective
  mixed ^= mixed >> 30;
  mixed *= 0xBF58476D1CE4E5B9ull;
  mixed ^= mixed >> 27;
  mixed *= 0x94D049BB133111EBull;
  mixed ^= mixed >> 31;  // finalizer(0) == 0, hence `base_seed` is returned unchanged for room 0
  return base_seed ^ mixed;
}

// v1.6（一场四局 · 局种子）：**房间种子只管「房间之间不同」，局种子管「局与局之间不同」，
// 两者缺一不可**——只有房间种子时，一场 4~8 局的牌山（连带四家起手牌）会逐局完全相同
// （B2 把「一局」改「一场」时漏掉的这一层）。
//
// 与 DeriveRoomSeed 同族（同 finalizer + 奇数乘子保证双射），但**不能沿用它的
// finalizer(0)==0 特性**：那一行会让 hand 0 退回 base_seed，与「房间 0 = 房间种子」的约定
// 撞车。这里让 `hand_index + 1` 参与混合，保证 hand 0 也与 base_seed 不同；纯函数、无时间源，
// 所以 `--seed` 固定时整场仍逐帧可复现（SameSeedReplaysTheWholeMatchFrameForFrame）。
inline uint64_t DeriveRoundSeed(uint64_t base_seed, uint32_t hand_index) {
  uint64_t mixed = (static_cast<uint64_t>(hand_index) + 1) * 0x9E3779B97F4A7C15ull;  // odd => bijective
  mixed ^= mixed >> 30;
  mixed *= 0xBF58476D1CE4E5B9ull;
  mixed ^= mixed >> 27;
  mixed *= 0x94D049BB133111EBull;
  mixed ^= mixed >> 31;  // finalizer(hand_index + 1) is non-zero for every hand_index
  return base_seed ^ mixed;
}

// GAME_DESIGN v1.4 §5.4: a bot's own decision (discard / meld / self-draw win) is executed
// ai_delay_ms after its window opened, so a human can follow the table. 0 disables the
// pacing and restores the fully synchronous decision path.
constexpr int kDefaultAiDelayMs = 900;
constexpr int kMaxAiDelayMs = 5000;

// v1.6（一场四局）: the pause between two rounds of one match (game_end -> next game_start).
// v1.8（SETTLEMENT_BROADCAST_RULINGS §2.1 / C2）: 5000 -> **12000** = 播报窗 10500 + 阅读窗 1500.
// The same value gates the *last* round too: `FinishMatch()` (match_end) is armed on the same
// delay, so the final settlement gets the same broadcast window as every other round.
constexpr int kDefaultInterHandDelayMs = 12000;
constexpr int kMaxInterHandDelayMs = 60000;

// Encoding of a scheduled-message token. A window deadline carries the plain envelope seq; an
// AI pacing deadline carries the same seq with kAiPacingTokenBit set, so one handler can serve
// both while the seq stays the invalidation key (PROTOCOL.md §0.5). Envelope seqs are small,
// so the top bits can never appear naturally.
constexpr uint64_t kAiPacingTokenBit = 1ull << 63;
inline uint64_t MakeAiPacingToken(uint64_t window_seq) { return window_seq | kAiPacingTokenBit; }
inline bool IsAiPacingToken(uint64_t token) { return (token & kAiPacingTokenBit) != 0; }

// v1.6（C6 局间推进令牌）: the round-to-round advance deadline carries the seq the round ended
// on, tagged with its own bit. It MUST be armed AFTER the round-end Transition(), so the token
// holds the brand-new seq: the last window's timeout (armed pre-transition, plain tag) fails
// the first criterion, the advance token passes it — the two never collide, and that is only
// true because of the arm-after-transition ordering.
constexpr uint64_t kRoundAdvanceTokenBit = 1ull << 62;
inline uint64_t MakeRoundAdvanceToken(uint64_t window_seq) {
  return window_seq | kRoundAdvanceTokenBit;
}
inline bool IsRoundAdvanceToken(uint64_t token) { return (token & kRoundAdvanceTokenBit) != 0; }

// v1.8（SETTLEMENT_BROADCAST_RULINGS §2.1 / C8）: the **match-end** deadline carries its own bit.
// The round-advance bit cannot be reused: its handler requires `phase_ == kRoundEnd &&
// round_advance_pending_` and only starts the next round — a match-end delay armed on that bit
// would silently do nothing (the "静默失效" trap). Same arm-after-Transition() ordering applies:
// the token must hold the post-round seq so the last window's timeout cannot collide with it.
constexpr uint64_t kMatchEndTokenBit = 1ull << 61;
inline uint64_t MakeMatchEndToken(uint64_t window_seq) { return window_seq | kMatchEndTokenBit; }
inline bool IsMatchEndToken(uint64_t token) { return (token & kMatchEndTokenBit) != 0; }
inline uint64_t WindowSeqOfToken(uint64_t token) {
  return token & ~(kAiPacingTokenBit | kRoundAdvanceTokenBit | kMatchEndTokenBit);
}

// Process-wide pacing values, set once by main() from --ai-delay-ms / --inter-hand-delay-ms.
// RoomActor reads them when the value is not passed explicitly (LobbyActor spawns rooms without
// carrying them, and lobby.* is outside this change); tests pass the delay to the constructor.
void SetAiActionDelayMs(int delay_ms);
int AiActionDelayMs();
void SetInterHandDelayMs(int delay_ms);
int InterHandDelayMs();

struct RoomSeat {
  int seat = 0;
  PlayerInfo player;
  bool is_bot = false;
  bool disconnected = false;  // entrusted: the room plays the default action for it
  // Where this seat's frames go (seat_endpoint.h). Empty for a bot, or for a seat with no wire
  // (the deal-only paths in the unit test) — that is the old `sink == nullptr` sentinel.
  ex_actor::ActorRef<SeatEndpoint> endpoint;
};

// One message into RoomActor. `Dispatch` is the room's message entry point (the only exception
// is the request/response `TakeSeat`, room.h), so every caller — LobbyActor's sends, the
// timeout scheduler and the unit tests — builds one of these instead of naming a handler.
//
// As in PlayerEnvelope (seat_endpoint.h), the kind is the variant alternative, so a message
// carries exactly its own fields. The flat version paid for a PlayerInfo, a json::Value and a
// RoomSeat on every message — the window timeout, the most frequent one, included — on each of
// the ~5 by-value moves into the mailbox. The variant charges each message only for its own
// payload (its floor is WindowTimeoutMsg), and every alternative is plain data, so the whole
// envelope can be serialized.
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
  // The frozen timeout scheme: |token| is a window seq, or the same seq with the AI pacing bit
  // set (PROTOCOL.md §0.5).
  struct WindowTimeoutMsg {
    uint64_t token;
  };
  struct PlayerDisconnectedMsg {
    PlayerInfo player;
  };
  struct IdleTimeoutMsg {
    PlayerInfo player;
  };

  static RoomEnvelope Ready(PlayerInfo player) { return {ReadyMsg {std::move(player)}}; }
  static RoomEnvelope Sync(PlayerInfo player) { return {SyncMsg {std::move(player)}}; }
  static RoomEnvelope GameFrame(PlayerInfo player, json::Value frame) {
    return {GameFrameMsg {std::move(player), std::move(frame)}};
  }
  static RoomEnvelope WindowTimeout(uint64_t token) { return {WindowTimeoutMsg {token}}; }
  static RoomEnvelope PlayerDisconnected(PlayerInfo player) { return {PlayerDisconnectedMsg {std::move(player)}}; }
  static RoomEnvelope IdleTimeout(PlayerInfo player) { return {IdleTimeoutMsg {std::move(player)}}; }

  std::variant<ReadyMsg, SyncMsg, GameFrameMsg, WindowTimeoutMsg, PlayerDisconnectedMsg, IdleTimeoutMsg>
      msg;
};

class RoomActor {
 public:
  // |ai_delay_ms| < 0 means "use the process-wide value" (AiActionDelayMs()); the same applies
  // to |inter_hand_delay_ms| (InterHandDelayMs()).
  //
  // |owner| is the lobby that spawned this room: the room reports every finished round to it
  // (GameSettlement) and never writes match history itself. Empty in the unit test, which
  // passes |finished_for_test| instead. The two "for test" seams exist because a scheduler and
  // a capture callback are process-local behavior — they cannot be part of a serializable
  // message (the production scheduler is installed by ExActorOnSpawned from |timer|).
  RoomActor(std::string room_id, uint64_t seed, int action_timeout_ms, std::vector<RoomSeat> seats,
            std::shared_ptr<WindowTimer> timer, int ai_delay_ms = -1, int inter_hand_delay_ms = -1,
            ex_actor::ActorRef<RoomOwner> owner = {}, TimeoutScheduler timeout_scheduler = {},
            RoomFinishedCallback finished_for_test = {},
            GameMode mode = GameMode::kClassic,
            GameLength length = GameLength::kTonpu);

  // ex_actor hands every actor its own ref right after construction. The room needs it to post
  // the frozen window timeout back to itself, so it installs the production scheduler here
  // instead of receiving a std::function through a message (which could never be serialized).
  void ExActorOnSpawned(const ex_actor::ActorRuntimeInfo<RoomActor>& info);

  // The room's only message entry point, and the only place the gate is taken for messages.
  // Every handler below is private, so no caller can reach room state without passing the gate
  // (async_gate.h explains why that matters). It is a coroutine, so a non-actor caller — the
  // unit test drives the room directly — has to start it (stdexec::sync_wait).
  stdexec::task<void> Dispatch(RoomEnvelope envelope);

  // Pre-deal seat replacement, request/response: the outcome travels back as the return value
  // because a `shared_ptr<bool>` reply slot is cross-thread shared state that cannot be
  // serialized. Takes the gate like Dispatch does.
  stdexec::task<bool> TakeSeat(RoomSeat seat);

  // Test seam (the unit test installs it right after construction, before any traffic):
  // production rooms report through |owner| instead. Deliberately not a message — a
  // std::function cannot be serialized.
  void SetFinishedCallbackForTest(RoomFinishedCallback callback) { finished_for_test_ = std::move(callback); }

  // Pure claim policy of an automated seat (exposed so it can be unit-tested directly with
  // crafted hands; RoomActor::OpenClaimWindow calls exactly this function for bots, and
  // OnPlayerDisconnected / OnIdleTimeout seats are driven by the same path).
  //   win                            -> always taken
  //   pong / every chow combination  -> taken only when the shanten after the meld (and its
  //                                     mandatory discard) does not get worse than the
  //                                     current shanten ("向听不劣化")
  //   several chow combinations      -> the one with the lowest post-meld shanten; ties keep
  //                                     the order the caller provided (frozen ClaimOptions
  //                                     order), and a pong wins a tie against a chow
  //   nothing acceptable             -> pass
  // An automated seat never claims a kong (and ankan/kakan never appear in a claim window).
  static Option ChooseClaimAnswer(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                  const std::vector<Option>& options, Tile claimed);

  // Introspection (logs and tests).
  const std::string& room_id() const { return room_id_; }
  GameMode mode() const { return mode_; }
  GameLength length() const { return length_; }
  int round_wind() const { return round_wind_; }
  bool game_started() const { return game_started_; }
  // v1.6（C13）: `game_finished` is the MATCH end (the room is done for good); a round that has
  // settled but is waiting for the next game_start is `round_finished`, not `game_finished`.
  bool game_finished() const { return phase_ == Phase::kMatchEnd; }
  bool match_finished() const { return phase_ == Phase::kMatchEnd; }
  bool round_finished() const { return phase_ == Phase::kRoundEnd || phase_ == Phase::kMatchEnd; }
  uint64_t seq() const { return seq_; }
  int remaining() const { return remaining_; }
  int current_seat() const { return current_seat_; }
  int dealer() const { return dealer_; }
  int next_wall_index() const { return next_index_; }
  // v1.6（C1 场次状态）
  int round_index() const { return round_index_; }        // 0-based, 0..round_count-1（连庄不动）
  int round_count() const { return round_count_; }        // classic 4 / 日麻 东场 4、半庄 8
  int renchan_count() const { return renchan_count_; }    // 本场累计连庄次数
  int hands_played() const { return hands_played_; }      // 已下发的 game_end 次数（含连庄）
  int match_total_of(int seat) const { return match_totals_[static_cast<size_t>(seat)]; }
  // "" when no window is open, otherwise "discard" or "claim" (PROTOCOL.md §5.7).
  std::string CurrentWindow() const {
    if (discard_window_open_) return "discard";
    if (claim_open_) return claim_chankan_ ? "chankan" : "claim";
    return "";
  }
  const std::vector<Tile>& hand_of(int seat) const { return hands_[static_cast<size_t>(seat)]; }
  // v1.6 局种子回归用：本局发牌后的牌山（含已发位置，next_index_ 之后才是未摸牌）。
  const std::vector<Tile>& wall_of() const { return wall_; }
  const std::vector<Meld>& melds_of(int seat) const { return melds_[static_cast<size_t>(seat)]; }
  const std::vector<Tile>& discards_of(int seat) const { return discards_[static_cast<size_t>(seat)]; }
  int SeatOf(const std::string& player_id) const;

 private:
  // Reached only through Dispatch, i.e. only while gate_ is held.
  // Before the deal: take a bot seat, or put a bot back. No-op once the game has started.
  bool TakeSeatLocked(RoomSeat seat);

  // Client intents, already shape-checked by PlayerActor and room-checked by LobbyActor.
  stdexec::task<void> Ready(PlayerInfo player);
  stdexec::task<void> Sync(PlayerInfo player);
  stdexec::task<void> HandleGameFrame(PlayerInfo player, json::Value frame);

  // Frozen timeout scheme: the message carries the window seq it was scheduled for.
  stdexec::task<void> OnWindowTimeout(uint64_t window_seq);
  stdexec::task<void> OnPlayerDisconnected(PlayerInfo player);
  stdexec::task<void> OnIdleTimeout(PlayerInfo player);

  enum class Phase {
    kIdle,           // created, no match yet
    kTurnStart,      // the current seat must draw (or the wall is exhausted -> drawn round)
    kDiscardWindow,  // a discard window is open for current_seat_
    kClaimWindow,    // a claim window is open for the seats holding claim options
    kRoundEnd,       // v1.6: the round settled (game_end broadcast), waiting for the next
                     // game_start; every game frame is answered BAD_REQUEST (C4)
    kMatchEnd,       // v1.6: the match is over (match_end broadcast); nothing but the reclaim
                     // remains (the narrowed kFinished of C4)
  };

  struct ClaimAnswer {
    bool answered = false;
    std::string action = "pass";
    std::vector<Tile> tiles;
  };

  // Everything game_end needs (PROTOCOL.md §5.10, v1.6 fields included).
  struct Settlement {
    std::string reason;
    std::array<int, kSeatCount> delta {};
    int winner_seat = -1;  // -1: drawn round
    std::vector<Tile> winner_flat_hand;
    //: v4.5：结算摊牌——四家的扁平手牌（门前 + 副露），只随 game_end 下发。
    std::array<std::vector<Tile>, kSeatCount> flat_hands {};
    std::vector<std::string> winner_types;  // tags + "ron"/"tsumo"
    bool tsumo = false;
    Tile winning_tile {};
    int from_seat = -1;
    std::vector<std::string> tags;  // = yaku[].id（tags ⊆ yaku id，单一出口）
    int multiplier = 0;             // R = 2^han
    int per_payer = 0;
    int total = 0;
    // ---- v1.6（C8 场次扩展）----
    int round_index = 0;                   // 本局序号（0 基）
    int dealer = 0;                        // 本局庄家
    int renchan_count = 0;                 // 截至本局结束的连庄次数（B2 决策：含本局触发的连庄）
    bool match_will_continue = false;
    std::array<int, kSeatCount> totals {};  // match_totals（含本局）
    std::vector<Yaku> yaku;                // 判番结果（B1 单一出口）
    int han = 0;                           // 封顶后番数（= han_breakdown.total）
    int dealer_bonus = 0;                  // 0/1
    bool rinshan = false;
    // ---- 日麻（mode == kRiichi）专用；classic 下全部保持默认值 ----
    int fu = 0;                              // 符（classic 恒 0）
    int points = 0;                          // 和牌点数（classic = multiplier）
    std::string limit;                       // none/mangan/haneman/baiman/sanbaiman/yakuman
    bool riichi = false;                     // 本局和牌者是否立直
    int dora = 0;
    int ura_dora = 0;
    int aka_dora = 0;                        // 赤宝牌张数（v1.18，classic 恒 0）
    int kyoutaku = 0;                        // 结算后剩余供托（通常 0）
    int honba = 0;                           // 本局本场数（支付已计入 delta）
    std::vector<Tile> dora_indicators;
    std::array<int, kSeatCount> points_after {};  // 本局结束后的持点（日麻排名口径）
    std::array<bool, kSeatCount> tenpai {};       // 荒庄时的听牌家（日麻罚符与 UI 用）
    // ---- v1.3 场终快照（仅 match_over == true 时有意义；B3 落库用，与 match_end 同源）----
    int hands_played = 0;                  // 实际计入局数（D2 口径：含截断）
    std::array<int, kSeatCount> dealer_hands {};  // 各座位本场坐庄手数（含连庄）
    std::array<int, kSeatCount> final_rank {};    // 1..4，并列跳号（与 match_end.rankings 同源）
  };

  // ---- turn driving ----------------------------------------------------------------
  stdexec::task<void> StartMatch();   // v1.6（C2）：ready 后一次；reset 场次状态
  stdexec::task<void> StartRound();   // v1.6（C3）：每局一次；reset 全部局内状态
  stdexec::task<void> Pump();
  stdexec::task<void> DrawForCurrentSeat();
  stdexec::task<void> OpenDiscardWindow(bool win_check, std::optional<Tile> drawn_tile);
  stdexec::task<void> ApplyDiscard(int seat, Tile tile);
  stdexec::task<void> OpenClaimWindow(int from_seat, Tile tile, const std::array<bool, kSeatCount>& recipients,
                                   const std::array<std::vector<Option>, kSeatCount>& options,
                                   bool chankan = false);
  stdexec::task<void> ResolveClaimWindow();
  stdexec::task<void> ApplyMeld(int seat, const std::string& action, const std::vector<Tile>& client_tiles, int from_seat,
                             Tile claimed_tile);
  // v1.6（自理杠）：ankan / kakan 只在本家打牌窗口；补摸 1 张后重开打牌窗口
  stdexec::task<void> ApplySelfKan(int seat, const std::string& action, const std::vector<Tile>& client_tiles);
  // 抢杠窗口（v1.15）：加杠那张牌先给三家一次「和/过」，没人抢才真正把杠落地。
  stdexec::task<void> OpenChankanWindow(int kan_seat, Tile tile);
  stdexec::task<void> EndRoundWithWin(int winner_seat, int from_seat, Tile winning_tile, bool tsumo,
                                      bool is_chankan = false);
  stdexec::task<void> EndRoundWithDraw();
  // v1.6（C7 局终 → 场终）：totals 累加 → 连庄/轮庄 → game_end → RecordMatch（C10）→
  // 场终 match_end，否则 kRoundEnd + 武装 C6 推进令牌（C5：co_return，推进走定时消息）。
  stdexec::task<void> AdvanceAfterRound(Settlement& settlement, const ScoreResult& score);
  // v1.6（C7）：match_end + kMatchEnd；只在场终调用
  stdexec::task<void> FinishMatch();
  // B3：match_end.rankings 的 rank（并列跳号），场终快照与 match_end 同源。
  std::array<int, kSeatCount> ComputeFinalRanks() const;

  // ---- client frame handlers --------------------------------------------------------
  stdexec::task<void> HandleDiscard(int seat, json::Value frame);
  stdexec::task<void> HandleAction(int seat, json::Value frame);

  // ---- helpers ----------------------------------------------------------------------
  uint64_t Transition();
  void ResetClaimWindow();
  // A seat is played by the server when it is a bot, when it is entrusted (disconnected) or
  // when it has no wire to ask (defensive: only seat 0 ever has a sink in this milestone).
  bool IsAutomated(int seat) const;
  bool NeedsPlayerReply(int seat) const {
    return !IsAutomated(seat) && !seats_[static_cast<size_t>(seat)].endpoint.IsEmpty();
  }
  std::optional<Tile> DefaultDiscardFor(int seat) const;

  // --- AI pacing (GAME_DESIGN v1.4 §5.4) ------------------------------------------------
  // Defers one decision to the shared timeout scheduler. Nothing blocks: the actor returns
  // to its mailbox and handles human frames meanwhile; the scheduled message carries the
  // window seq, so a window that moved on makes it stale and it is dropped.
  // Returns false when no scheduler is installed: the caller must then decide inline, because
  // a pending flag without an armed message would wait for a reply that never comes.
  bool ScheduleAiAction(int64_t delay_ms);
  // The automated seat's own-window decision (self-draw win or tile-efficiency discard).
  // Returns false when the game ended and there is nothing left to pump.
  stdexec::task<bool> PlayAutomatedDiscard();
  stdexec::task<bool> BotDeclaresTsumo(int seat);
  // Fills in the answers of the automated claim recipients that were deferred.
  void ResolveAutomatedClaimAnswers();
  // A human recipient already declared `win`: no pending bot answer can change that outcome.
  bool HasPendingHumanWin() const;
  // The tile that completes the current seat's hand on its own draw (a tsumo). The dealer's
  // 天和 has no draw record, so the largest tile of the hand stands in for it.
  Tile TsumoWinningTile(int seat) const;
  void OpenWindowTimer();
  stdexec::task<void> ReplyError(int seat, std::string_view code, std::string_view message);
  // Tells the room's owner the game is over, exactly once (PROTOCOL.md §1.4.5).
  void NotifyFinished(const Settlement& settlement);
  stdexec::task<void> ReplyFrame(int seat, std::string frame);
  // One seat's share of a broadcast: empty frames and empty endpoints are skipped. Deliver
  // awaits exactly four of these in parallel, so the arity is fixed rather than looped.
  stdexec::task<void> DeliverSeat(int seat, std::string frame);
  stdexec::task<void> Deliver(std::array<std::string, kSeatCount> frames);
  // v1.6（N1 两版缓存）：普通广播用打码版（他家 ankan.tiles = []，仅暗杠持有者额外渲染一份
  // 替换自身项，次数 = 持有者数）；局终前最后一份 state_sync 用揭示版（所有 ankan 补全 4 张）。
  std::array<std::string, kSeatCount> StateSyncFrames() const;
  std::array<std::string, kSeatCount> StateSyncFramesRevealed() const;
  bool SeatHoldsAnkan(int seat) const;
  // The `seats[]` block of state_sync. |viewer|: -2 = 局终揭示（所有 ankan 4 张）、
  // -1 = 全部打码（共享版）、0..3 = 该座位视角（自家 ankan 可见）。它不再与收件人完全无关，
  // 但仍与「同一个 viewer 的多次渲染」无关——共享打码版 + 逐持有者替换的渲染次数上界见上。
  // A single recipient (the client `sync`) goes through the one-arg BuildStateSync, which
  // renders the block itself.
  json::Value BuildSeatsArray(int viewer) const;
  static constexpr int kSeatsMaskAll = -1;
  static constexpr int kSeatsRevealAll = -2;
  // Only `hand`/`melds` come from |for_seat|; |seats_json| must be BuildSeatsArray(viewer).Dump().
  std::string BuildStateSync(int for_seat, const std::string& seats_json) const;
  std::string BuildStateSync(int for_seat) const;
  // PROTOCOL v1.7 §5.16：独立 `tenpai` 帧（仅发本家、tenpai_seq 独立单调、不参与 room.seq）。
  json::Value BuildTenpaiFrame(int seat) const;
  stdexec::task<void> DeliverTenpai(int seat);
  std::string BuildGameStart(int for_seat) const;
  std::string BuildGameEnd(const Settlement& settlement) const;
  std::string BuildMatchEnd() const;
  stdexec::task<void> BroadcastSettlement(const Settlement& settlement);

  // ---- 日麻（mode == kRiichi）--------------------------------------------------------
  // 打牌 / 鸣牌窗口的选项：classic 直接转发规则库，日麻额外叠加立直与振听过滤。
  std::vector<Option> DiscardOptionsFor(int seat) const;
  std::vector<Option> ClaimOptionsFor(int seat, Tile tile, bool is_next_player, int from_seat) const;
  // 日麻：没有役就不能和（宝牌不算役）。classic 恒 true。房间在这里用**完整上下文**判，
  // 这样立直/一发/海底/杠上开花这类「状态役」也能算进去。
  bool HasYakuForWin(int seat, Tile winning_tile, bool tsumo, int from_seat,
                     bool is_chankan = false) const;
  // 和牌形：日麻要额外认国士无双 —— 共享的 `IsWinningHand` 是番数制口径，没有这个役。
  bool IsWinShape(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                  Tile winning_tile, bool tsumo) const;
  bool IsFuriten(int seat) const;
  bool RiichiDeclared(int seat) const { return riichi_[static_cast<size_t>(seat)]; }
  // 立直宣言（与打牌同一条 action）：校验后扣 1000 供托、置位，再由调用方打出该牌。
  bool CanDeclareRiichiNow(int seat, Tile discard) const;
  stdexec::task<void> DeclareRiichi(int seat);
  // 岭上补牌：日麻从王牌取、并用手牌山顶牌补满王牌；classic 仍从牌山取。
  Tile DrawReplacementTile();
  void FlipDoraIndicator();
  riichi::WinContext BuildRiichiContext(int winner_seat, int from_seat, bool tsumo,
                                        Tile winning_tile, bool is_chankan = false) const;
  void ApplyDeltaToPoints(const std::array<int, kSeatCount>& delta);
  bool BotShouldRiichi(int seat) const;

  // ---- state ------------------------------------------------------------------------
  std::string room_id_;
  uint64_t seed_;
  int action_timeout_ms_;
  std::vector<RoomSeat> seats_;
  std::array<std::vector<Tile>, kSeatCount> hands_;
  std::array<std::vector<Meld>, kSeatCount> melds_;
  std::array<std::vector<Tile>, kSeatCount> discards_;
  std::vector<Tile> wall_;
  int next_index_ = 0;
  int remaining_ = 0;
  // 可摸区上界：classic = 136（整副牌都在牌山）；日麻 = 122（136 - 14 张王牌）。
  // `remaining_` 一律派生自 drawable_end_ - next_index_，禁止自减（RULESET §1.5 红线）。
  int drawable_end_ = kTileCount;
  GameMode mode_ = GameMode::kClassic;
  GameLength length_ = GameLength::kTonpu;  // 只影响 round_count_ 与场风轮转（classic 恒 4 局）
  // 日麻王牌 14 张：0..3 岭上牌 / 4..8 宝牌指示牌 / 9..13 里宝指示牌。
  std::vector<Tile> dead_wall_;
  int dead_draw_index_ = 0;
  std::vector<Tile> dora_indicators_;  // 已翻开的指示牌（classic 恒为空）
  std::array<int, kSeatCount> points_ {};  // 日麻持点（classic 不使用）
  int kyoutaku_ = 0;                       // 供托（立直棒总数 × 1000）
  int honba_ = 0;                          // 本场数
  int round_wind_ = 0;                     // 场风：0 东 / 1 南（每局开始时按 round_index_ 推导）
  std::array<bool, kSeatCount> riichi_ {};       // 本局已宣言立直
  std::array<bool, kSeatCount> ippatsu_ {};      // 一发仍然有效（鸣牌后清除）
  std::array<bool, kSeatCount> ippatsu_armed_ {};  // 宣言后尚未跨过「下一摸」
  std::array<bool, kSeatCount> furiten_ever_ {}; // 见逃振听（本局内持久）
  bool draw_dealer_tenpai_ = false;              // 荒庄时庄家是否听牌（决定连庄/轮庄）
  int dealer_ = 0;
  int current_seat_ = 0;
  uint64_t seq_ = 0;
  Phase phase_ = Phase::kIdle;
  bool game_started_ = false;

  // v1.6（C1 场次状态）。跨局存活、绝不在 StartRound 里重置：room_id_/seats_/seq_/match_totals_。
  int round_index_ = 0;   // 0-based；连庄不推进，轮庄 +1（GAME_DESIGN v1.10 §3.3）
  int round_count_ = 4;   // classic 恒 4（C15）；日麻 4（东场）/ 8（半庄）
  int hands_played_ = 0;  // 已下发的 game_end 次数（含连庄），上界 8
  int renchan_count_ = 0; // 本场累计连庄次数；> kMaxRenchan 强制场终（C15）
  std::array<int, kSeatCount> match_totals_ {};
  std::array<int, kSeatCount> dealer_hands_ {};  // B3：各座位本场坐庄手数（每局 +1，含连庄）
  // v1.7 §5.16：`tenpai_seq` 是**每座位一条的独立单调计数器**（从 1 起、跨局不重置；
  // 与 room.seq 完全解耦——挂 room.seq 会把正常 action_request 判成过期）。
  std::array<uint64_t, kSeatCount> tenpai_seq_ {};
  // C15 双闸：classic 单场上界 = 4 局 + 4 连庄 = 8 次 game_end；
  // 日麻用 round_count_ 作连庄上限（东场 8 / 半庄 16 次 game_end 封顶），不吃这个 4。
  static constexpr int kMaxRenchan = 4;

  // v1.6（C6 局间推进令牌）：true = 本局的推进消息已武装，等定时到达
  bool round_advance_pending_ = false;
  // v1.8（播报窗）：true = 场终（match_end）的延迟消息已武装，等定时到达。与
  // `round_advance_pending_` 分开是因为场终那条走独立的令牌位（kMatchEndTokenBit），
  // 两者判据的 phase 相同（kRoundEnd），只能靠各自的 pending 位区分。
  bool match_end_pending_ = false;
  // v1.6（自理杠补牌记录）：杠后打开的打牌窗口里自摸且和牌张 == 它 ⇒ 杠上开花（B1 ScoreContext 接线）
  std::optional<Tile> kong_draw_tile_;

  // discard window
  bool discard_window_open_ = false;
  // Whether the open discard window allows a self-draw win at all (false right after a
  // pong/chow, where the hand is not a complete 14-tile form). Set by OpenDiscardWindow.
  bool discard_window_win_check_ = false;
  std::optional<Tile> drawn_tile_;  // draw record of the current seat (timeout default)

  // claim window
  bool claim_open_ = false;
  // 本次鸣牌窗口是「抢杠」而不是「抢打出的牌」：没人抢时要回到加杠者的自理杠流程，
  // 而不是把下家推进成庄家（ResolveClaimWindow 的两个分支就靠它区分）。
  bool claim_chankan_ = false;
  int claim_from_seat_ = -1;
  Tile claim_tile_ {};
  std::array<bool, kSeatCount> claim_recipient_ {};
  std::array<std::vector<Option>, kSeatCount> claim_options_;
  std::array<ClaimAnswer, kSeatCount> claim_answers_;
  int claim_pending_humans_ = 0;

  TimeoutScheduler timeout_scheduler_;
  // Set only by the unit test; production reports through |owner_| instead.
  RoomFinishedCallback finished_for_test_;
  // The lobby that spawned this room (empty in the unit test).
  ex_actor::ActorRef<RoomOwner> owner_;
  // Filled by ExActorOnSpawned; the scheduler posts timeouts through it.
  ex_actor::ActorRef<RoomActor> self_ref_;

  std::shared_ptr<WindowTimer> timer_;
  // AI pacing (GAME_DESIGN v1.4 §5.4). A pending flag means a decision of this window is
  // already scheduled, so Pump() never schedules it twice.
  int ai_delay_ms_ = 0;
  bool ai_discard_pending_ = false;
  bool ai_claim_pending_ = false;
  // v1.6（C6）：局间停顿长度
  int inter_hand_delay_ms_ = 0;
  // Serializes the room's handlers (async_gate.h). Held for the whole of Dispatch, so the state
  // machine keeps its single-transition semantics even once a handler suspends mid-message.
  AsyncGate gate_;
};

}  // namespace mahjong
