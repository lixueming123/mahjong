// ClassicMahjongGame: the 番数制 mahjong game that used to be RoomActor's only behaviour.
//
// Everything that is a *rule* lives here — the wall, the four hands, the turn machine, the two
// windows, the settlement, the bots — and nothing that is a *table* (seats, endpoints, clocks,
// delivery) does. It plugs into the generic room through game.h; `RoomActorFor<ClassicMahjongGame>`
// is the only place the two meet.
//
// This file is deliberately self-contained: no `riichi::` symbol appears anywhere in it, and the
// riichi game is a separate implementation of the same interface (riichi/riichi_mahjong_game.h).
// The two share tile primitives and nothing else.
//
// Threading model is gone with the actor: `Validate` is const, `Apply` is the only mutator, and
// a whole cascade (a discard that opens no claim window and moves to the next draw) happens
// inside one `Apply` call, appending to one `GameStep`. That is why every private helper takes
// `GameStep&` instead of awaiting a delivery — the frames are built, ordered and returned, and
// the room is what sends them.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game.h"
#include "json.h"
#include "mahjong_rules.h"
#include "room_contract.h"
#include "tokens.h"

namespace mahjong {

// GAME_DESIGN v1.4 §5.4: a bot's own decision (discard / meld / self-draw win) is executed
// ai_delay_ms after its window opened, so a human can follow the table. 0 disables the pacing
// and restores the fully synchronous decision path.
constexpr int kClassicDefaultAiDelayMs = 900;
constexpr int kClassicMaxAiDelayMs = 5000;

// v1.6（一场四局）: the pause between two rounds of one match (game_end -> next game_start).
// v1.8（SETTLEMENT_BROADCAST_RULINGS §2.1 / C2）: 5000 -> **12000** = 播报窗 10500 + 阅读窗 1500.
constexpr int kClassicDefaultInterHandDelayMs = 12000;
constexpr int kClassicMaxInterHandDelayMs = 60000;

// The deadline-token encoding (kAiPacingTokenBit / MakeRoundAdvanceToken / ...) is shared with
// every other game and lives in tokens.h.

class ClassicMahjongGame final : public Game {
 public:
  ClassicMahjongGame() = default;
  ClassicMahjongGame(const ClassicMahjongGame&) = delete;
  ClassicMahjongGame& operator=(const ClassicMahjongGame&) = delete;
  ~ClassicMahjongGame() override = default;

  // ---- Game ---------------------------------------------------------------------------
  std::string id() const override { return "classic"; }
  GameStep Setup(const GameContext& context) override;
  bool IsOver() const override { return phase_ == Phase::kMatchEnd; }
  std::vector<int> ActiveSeats() const override;
  GameVerdict Validate(int seat, const json::Value& action) const override;
  GameStep Apply(int seat, const json::Value& action) override;
  GameStep OnTimeout(uint64_t token) override;
  GameStep Sync(int seat) override;
  GameStep Reconnect(int seat) override;
  void OnSeatPresence(int seat, bool present) override;

  // Pure claim policy of an automated seat (exposed so it can be unit-tested directly with
  // crafted hands; OpenClaimWindow calls exactly this function for bots, and a seat with no
  // wire is driven by the same path).
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

  // ---- Introspection (logs and tests) -------------------------------------------------
  const std::string& room_id() const { return room_id_; }
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
  int round_index() const { return round_index_; }      // 0-based, 0..round_count-1（连庄不动）
  int round_count() const { return round_count_; }      // classic 恒 4
  int renchan_count() const { return renchan_count_; }  // 本场累计连庄次数
  int hands_played() const { return hands_played_; }    // 已下发的 game_end 次数（含连庄）
  int match_total_of(int seat) const { return match_totals_[static_cast<size_t>(seat)]; }
  // Test seam: 正常流程里持点只能由结算改写，测试要构造终局判定边界时没有别的入口。
  // classic 没有持点概念，这个入口只保留给「与 riichi 同形」的测试脚手架保持对称。
  void SetPointsForTest(const std::array<int, kSeatCount>& points) { points_ = points; }
  // kInvalid when no window is open, otherwise the window wire enum (PROTOCOL v2.0 §1.3.1).
  protocol::Window CurrentWindow() const {
    if (discard_window_open_) return protocol::Window::kDiscard;
    if (claim_open_) return protocol::Window::kClaim;
    return protocol::Window::kInvalid;
  }
  const std::vector<Tile>& hand_of(int seat) const { return hands_[static_cast<size_t>(seat)]; }
  // v1.6 局种子回归用：本局发牌后的牌山（含已发位置，next_index_ 之后才是未摸牌）。
  const std::vector<Tile>& wall_of() const { return wall_; }
  const std::vector<Meld>& melds_of(int seat) const { return melds_[static_cast<size_t>(seat)]; }
  const std::vector<Tile>& discards_of(int seat) const { return discards_[static_cast<size_t>(seat)]; }
  int SeatOf(const std::string& player_id) const;

 private:
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
    protocol::Action action = protocol::Action::kPass;
    std::vector<Tile> tiles;
  };

  // Everything game_end needs (PROTOCOL.md §5.10, v1.6 fields included). Riichi-only fields
  // (fu / points / dora / limits) are not here: that settlement lives in the riichi game.
  struct Settlement {
    protocol::RoundEndReason reason = protocol::RoundEndReason::kInvalid;
    std::array<int, kSeatCount> delta {};
    int winner_seat = -1;  // -1: drawn round
    std::vector<Tile> winner_flat_hand;
    //: v4.5：结算摊牌——四家的扁平手牌（门前 + 副露），只随 game_end 下发。
    std::array<std::vector<Tile>, kSeatCount> flat_hands {};
    std::vector<protocol::YakuId> winner_types;  // tags + kWinRon/kWinTsumo
    bool tsumo = false;
    Tile winning_tile {};
    int from_seat = -1;
    std::vector<protocol::YakuId> tags;  // = yaku[].id（tags ⊆ yaku id，单一出口）
    int multiplier = 0;                  // R = 2^han
    int per_payer = 0;
    int total = 0;
    // ---- v1.6（C8 场次扩展）----
    int round_index = 0;    // 本局序号（0 基）
    int dealer = 0;         // 本局庄家
    int renchan_count = 0;  // 截至本局结束的连庄次数（B2 决策：含本局触发的连庄）
    bool match_will_continue = false;
    std::array<int, kSeatCount> totals {};  // match_totals（含本局）
    std::vector<Yaku> yaku;                 // 判番结果（B1 单一出口）
    int han = 0;                            // 封顶后番数（= han_breakdown.total）
    int dealer_bonus = 0;                   // 0/1
    bool rinshan = false;
    // ---- v1.3 场终快照（仅 match_over == true 时有意义；B3 落库用，与 match_end 同源）----
    int hands_played = 0;                         // 实际计入局数（D2 口径：含截断）
    std::array<int, kSeatCount> dealer_hands {};  // 各座位本场坐庄手数（含连庄）
    std::array<int, kSeatCount> final_rank {};    // 1..4，并列跳号（与 match_end.rankings 同源）
  };

  // ---- turn driving ----------------------------------------------------------------
  void StartMatch(GameStep& step);  // v1.6（C2）：ready 后一次；reset 场次状态
  void StartRound(GameStep& step);  // v1.6（C3）：每局一次；reset 全部局内状态
  void Pump(GameStep& step);
  void DrawForCurrentSeat(GameStep& step);
  void OpenDiscardWindow(GameStep& step, bool win_check, std::optional<Tile> drawn_tile);
  void ApplyDiscard(GameStep& step, int seat, Tile tile);
  void OpenClaimWindow(GameStep& step, int from_seat, Tile tile, const std::array<bool, kSeatCount>& recipients,
                       const std::array<std::vector<Option>, kSeatCount>& options);
  void ResolveClaimWindow(GameStep& step);
  void ApplyMeld(GameStep& step, int seat, protocol::Action action, const std::vector<Tile>& client_tiles,
                 int from_seat, Tile claimed_tile);
  // v1.6（自理杠）：ankan / kakan 只在本家打牌窗口；补摸 1 张后重开打牌窗口
  void ApplySelfKan(GameStep& step, int seat, protocol::Action action, const std::vector<Tile>& client_tiles);
  void EndRoundWithWin(GameStep& step, int winner_seat, int from_seat, Tile winning_tile, bool tsumo);
  void EndRoundWithDraw(GameStep& step);
  // v1.6（C7 局终 → 场终）：totals 累加 → 连庄/轮庄 → game_end → RecordMatch（C10）→
  // 场终 match_end，否则 kRoundEnd + 武装 C6 推进令牌（C5：返回，推进走定时消息）。
  void AdvanceAfterRound(GameStep& step, Settlement& settlement, const ScoreResult& score);
  // v1.6（C7）：match_end + kMatchEnd；只在场终调用
  void FinishMatch(GameStep& step);
  // B3：match_end.rankings 的 rank（并列跳号），场终快照与 match_end 同源。
  std::array<int, kSeatCount> ComputeFinalRanks() const;

  // ---- client frame handlers --------------------------------------------------------
  // Validate* are the read-only half (io: parse, check, never mutate); Apply*Frame is the
  // mutation half and assumes the matching Validate* returned kAccept.
  GameVerdict ValidateDiscard(int seat, const json::Value& frame) const;
  GameVerdict ValidateAction(int seat, const json::Value& frame) const;
  void ApplyDiscardFrame(GameStep& step, int seat, const json::Value& frame);
  void ApplyActionFrame(GameStep& step, int seat, const json::Value& frame);
  // Manual handoff. Never writes is_bot: a bot seat cannot be turned back into a human.
  void ApplyAutoplay(GameStep& step, int seat, bool on);
  // Re-sends the still-open action_request (same seq, same deadline) after autoplay is cancelled.
  void EmitOpenRequest(GameStep& step, int seat);

  // ---- helpers ----------------------------------------------------------------------
  uint64_t Transition();
  void ResetClaimWindow();
  // A seat is played by the server inside the message that opens its window only when it is a
  // bot, when the human turned autoplay on, or when it has no wire to ask. A disconnected human
  // seat is NOT automated: it keeps the window's normal countdown, so the deadline — not the
  // disconnect — plays the default action. is_bot itself is fixed for the match.
  bool IsAutomated(int seat) const { return seats_[static_cast<size_t>(seat)].is_bot; }
  bool PlaysWithAi(int seat) const { return IsAutomated(seat) || autoplay_[static_cast<size_t>(seat)]; }
  bool NeedsPlayerReply(int seat) const { return !PlaysWithAi(seat) && seats_[static_cast<size_t>(seat)].wired; }
  bool SeatPresent(int seat) const { return !disconnected_[static_cast<size_t>(seat)]; }
  std::optional<Tile> DefaultDiscardFor(int seat) const;
  // Takes the next tile off the live wall. Classic draws every tile the same way; the riichi
  // game is the one with a dead wall and a replacement draw.
  Tile DrawFromWall();

  // --- AI pacing (GAME_DESIGN v1.4 §5.4) ------------------------------------------------
  // Requests one deferred decision from the room. Nothing blocks: the room arms a deadline and
  // the game keeps serving human frames meanwhile; the deadline carries the window seq, so a
  // window that moved on makes it stale and it is dropped.
  // Returns false when the room has no scheduler: the caller must then decide inline, because
  // a pending flag without an armed message would wait for a reply that never comes.
  bool ScheduleAiAction(GameStep& step, int64_t delay_ms);
  // The automated seat's own-window decision (self-draw win or tile-efficiency discard).
  // Returns false when the game ended and there is nothing left to pump.
  bool PlayAutomatedDiscard(GameStep& step);
  bool BotDeclaresTsumo(GameStep& step, int seat);
  // Fills in the answers of the automated claim recipients that were deferred.
  void ResolveAutomatedClaimAnswers();
  // A human recipient already declared `win`: no pending bot answer can change that outcome.
  bool HasPendingHumanWin() const;
  // The tile that completes the current seat's hand on its own draw (a tsumo). The dealer's
  // 天和 has no draw record, so the largest tile of the hand stands in for it.
  Tile TsumoWinningTile(int seat) const;
  void OpenWindowTimer(GameStep& step);

  // ---- frames -----------------------------------------------------------------------
  // v1.6（N1 两版缓存）：普通广播用打码版（他家 ankan.tiles = []，仅暗杠持有者额外渲染一份
  // 替换自身项，次数 = 持有者数）；局终前最后一份 state_sync 用揭示版（所有 ankan 补全 4 张）。
  std::array<std::string, kSeatCount> StateSyncFrames() const;
  std::array<std::string, kSeatCount> StateSyncFramesRevealed() const;
  bool SeatHoldsAnkan(int seat) const;
  // The `seats[]` block of state_sync. |viewer|: -2 = 局终揭示（所有 ankan 4 张）、
  // -1 = 全部打码（共享版）、0..3 = 该座位视角（自家 ankan 可见）。
  json::Value BuildSeatsArray(int viewer) const;
  static constexpr int kSeatsMaskAll = -1;
  static constexpr int kSeatsRevealAll = -2;
  // Only `hand`/`melds` come from |for_seat|; |seats_json| must be BuildSeatsArray(viewer).Dump().
  std::string BuildStateSync(int for_seat, const std::string& seats_json) const;
  std::string BuildStateSync(int for_seat) const;
  // PROTOCOL v1.7 §5.16：独立 `tenpai` 帧（仅发本家、tenpai_seq 独立单调、不参与 room.seq）。
  json::Value BuildTenpaiFrame(int seat) const;
  // §5.16 下发（T1 打牌后 / T2 副露后 / T3 sync 补发）：只发本家、一份；bot 无 wire 不发。
  void DeliverTenpai(GameStep& step, int seat);
  std::string BuildGameStart(int for_seat) const;
  std::string BuildGameEnd(const Settlement& settlement) const;
  std::string BuildMatchEnd() const;
  void BroadcastSettlement(GameStep& step, const Settlement& settlement);

  // ---- rule options ------------------------------------------------------------------
  // 打牌 / 鸣牌窗口的选项：classic 直接转发规则库（日麻的立直与振听过滤在 riichi 实现里）。
  std::vector<Option> DiscardOptionsFor(int seat) const;
  std::vector<Option> ClaimOptionsFor(int seat, Tile tile, bool is_next_player) const;

  // ---- state ------------------------------------------------------------------------
  std::string room_id_;
  uint64_t seed_;
  int action_timeout_ms_ = 0;
  int ai_delay_ms_ = 0;
  int inter_hand_delay_ms_ = 0;
  bool deadlines_supported_ = false;
  std::vector<GameSeat> seats_;
  std::array<bool, kSeatCount> disconnected_ {};  // fed by OnSeatPresence
  std::array<bool, kSeatCount> autoplay_ {};      // human handed the seat to the AI; never set on a bot
  std::array<std::vector<Tile>, kSeatCount> hands_;
  std::array<std::vector<Meld>, kSeatCount> melds_;
  std::array<std::vector<Tile>, kSeatCount> discards_;
  std::vector<Tile> wall_;
  int next_index_ = 0;
  int remaining_ = 0;
  // 可摸区上界：classic = 136（整副牌都在牌山）。`remaining_` 一律派生自
  // drawable_end_ - next_index_，禁止自减（RULESET §1.5 红线）。
  int drawable_end_ = kTileCount;
  std::array<int, kSeatCount> points_ {};  // classic 不使用持点，恒 0（保留以与 riichi 同形）
  int round_wind_ = 0;                     // 场风：classic 恒 0（东），保留以与 riichi 同形
  int dealer_ = 0;
  int current_seat_ = 0;
  uint64_t seq_ = 0;
  Phase phase_ = Phase::kIdle;
  bool game_started_ = false;

  // v1.6（C1 场次状态）。跨局存活、绝不在 StartRound 里重置：room_id_/seats_/seq_/match_totals_。
  int round_index_ = 0;    // 0-based；连庄不推进，轮庄 +1（GAME_DESIGN v1.10 §3.3）
  int round_count_ = 4;    // classic 恒 4（C15）
  int hands_played_ = 0;   // 已下发的 game_end 次数（含连庄），上界 8
  int renchan_count_ = 0;  // 本场累计连庄次数；> kMaxRenchan 强制场终（C15）
  std::array<int, kSeatCount> match_totals_ {};
  std::array<int, kSeatCount> dealer_hands_ {};  // B3：各座位本场坐庄手数（每局 +1，含连庄）
  // v1.7 §5.16：`tenpai_seq` 是**每座位一条的独立单调计数器**（从 1 起、跨局不重置；
  // 与 room.seq 完全解耦——挂 room.seq 会把正常 action_request 判成过期）。
  std::array<uint64_t, kSeatCount> tenpai_seq_ {};
  // C15 双闸：classic 单场上界 = 4 局 + 4 连庄 = 8 次 game_end。
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
  // PROTOCOL v2.2 §8.5: the exact frame the reconnecting seat needs. Rebuilding the options at
  // reconnect time could drift if any helper is not deterministic, so the window stores them.
  std::vector<Option> discard_options_;
  int64_t window_deadline_ms_ = 0;

  // claim window
  bool claim_open_ = false;
  int claim_from_seat_ = -1;
  Tile claim_tile_ {};
  std::array<bool, kSeatCount> claim_recipient_ {};
  std::array<std::vector<Option>, kSeatCount> claim_options_;
  std::array<ClaimAnswer, kSeatCount> claim_answers_;
  int claim_pending_humans_ = 0;
  // The last settled round, kept for a reconnect during kRoundEnd/kMatchEnd. Cleared only when
  // the next settlement overwrites it; StartRound does not need it.
  std::optional<Settlement> last_settlement_;

  // AI pacing (GAME_DESIGN v1.4 §5.4). A pending flag means a decision of this window is
  // already scheduled, so Pump() never schedules it twice.
  bool ai_discard_pending_ = false;
  bool ai_claim_pending_ = false;
};

}  // namespace mahjong
