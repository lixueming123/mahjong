// RiichiMahjongGame: 日麻（符+番、立直、振听、宝牌、点数制），RoomActor 的第二个插件。
//
// 与 classic 的关系：**同一条状态机骨架，不同的规则决定**。牌山、四家手牌、回合驱动、两个窗口、
// 结算、机器人这些结构两边一模一样，日麻多出来的是：
//
//   * 王牌 14 张（岭上 0..3 / 宝牌指示 4..8 / 里宝指示 9..13），可摸区只到 122；
//   * 持点 25000 起、立直棒 1000、本场 300、击飞、场终供托归一位；
//   * 立直宣言（与打牌同一条 action）、一发、振听（舍て牌 + 见逃）、抢杠窗口；
//   * 无役不能和、国士无双、荒庄听牌罚符、按持点排名。
//
// 所以这个文件**不做**任何 `mode_ == ...` 判断：它自己就是日麻。想让两种玩法行为一致的地方
// 只能靠「两份代码碰巧一样」，而不是靠一个分支——这是有意的，改一个玩法不该让另一个跟着动。
//
// Threading model：同 classic，`Validate` 是只读、`Apply` 是唯一改状态的入口，一整串连锁
// （打牌 → 没人鸣 → 下家摸牌）都在一次 `Apply` 里填进同一个 `GameStep`，房间只负责投递。

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
#include "riichi_rules.h"
#include "room_contract.h"
#include "tokens.h"

namespace mahjong {

// 日麻的节奏默认值与 classic 相同（人类看得出牌、局间有播报窗），但它们是**这个玩法自己的**
// 常量：改日麻的节奏不该牵动普通麻将。
constexpr int kRiichiDefaultAiDelayMs = 900;
constexpr int kRiichiMaxAiDelayMs = 5000;
constexpr int kRiichiDefaultInterHandDelayMs = 12000;
constexpr int kRiichiMaxInterHandDelayMs = 60000;

class RiichiMahjongGame final : public Game {
 public:
  RiichiMahjongGame() = default;
  RiichiMahjongGame(const RiichiMahjongGame&) = delete;
  RiichiMahjongGame& operator=(const RiichiMahjongGame&) = delete;
  ~RiichiMahjongGame() override = default;

  // ---- Game ---------------------------------------------------------------------------
  std::string id() const override { return "riichi"; }
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
  int seat_wind_of(int seat) const { return riichi::SeatWindOf(round_wind_, seat, dealer_); }
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
  int round_count() const { return round_count_; }      // 东场 4 / 半庄 8
  int renchan_count() const { return renchan_count_; }  // 本场累计连庄次数
  int hands_played() const { return hands_played_; }    // 已下发的 game_end 次数（含连庄）
  int match_total_of(int seat) const { return match_totals_[static_cast<size_t>(seat)]; }
  int points_of(int seat) const { return points_[static_cast<size_t>(seat)]; }
  int kyoutaku() const { return kyoutaku_; }
  int honba() const { return honba_; }
  // Test seam: 正常流程里持点只能由结算改写，测试要构造「已被打穿」或「恰好 0 点」这两种
  // 终局判定边界时没有别的入口。生产代码不得调用。
  void SetPointsForTest(const std::array<int, kSeatCount>& points) { points_ = points; }
  // Test seam: 供托只会在立直宣言时由玩家投入，房间层没有别的入口能造出「场终还剩棒」
  // 这种局面（末局荒庄得靠发牌运气）。**不扣持点**，所以调用方算守恒时要把它加回去。
  void SetKyoutakuForTest(int sticks) { kyoutaku_ = sticks; }
  // kInvalid when no window is open, otherwise the window wire enum (PROTOCOL v2.0 §1.3.1).
  protocol::Window CurrentWindow() const {
    if (discard_window_open_) return protocol::Window::kDiscard;
    if (claim_open_) return claim_chankan_ ? protocol::Window::kChankan : protocol::Window::kClaim;
    return protocol::Window::kInvalid;
  }
  const std::vector<Tile>& hand_of(int seat) const { return hands_[static_cast<size_t>(seat)]; }
  // v1.6 局种子回归用：本局发牌后的牌山（含已发位置，next_index_ 之后才是未摸牌）。
  const std::vector<Tile>& wall_of() const { return wall_; }
  const std::vector<Tile>& dead_wall_of() const { return dead_wall_; }
  const std::vector<Tile>& dora_indicators_of() const { return dora_indicators_; }
  const std::vector<Meld>& melds_of(int seat) const { return melds_[static_cast<size_t>(seat)]; }
  const std::vector<Tile>& discards_of(int seat) const { return discards_[static_cast<size_t>(seat)]; }
  bool riichi_declared(int seat) const { return riichi_[static_cast<size_t>(seat)]; }
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

  // Everything game_end needs (PROTOCOL.md §5.10, v1.6 fields included) plus the riichi payload
  // (fu / points / limit / dora / ura / aka / kyoutaku / honba / points_after / tenpai).
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
    int multiplier = 0;                  // 日麻下用点数填充（客户端旧字段）
    int per_payer = 0;
    int total = 0;
    // ---- v1.6（C8 场次扩展）----
    int round_index = 0;    // 本局序号（0 基）
    int dealer = 0;         // 本局庄家
    int renchan_count = 0;  // 截至本局结束的连庄次数（B2 决策：含本局触发的连庄）
    bool match_will_continue = false;
    std::array<int, kSeatCount> totals {};  // match_totals（含本局）
    std::vector<Yaku> yaku;                 // 判番结果（与 classic 的 Yaku 结构同形）
    int han = 0;                            // 封顶后番数
    int dealer_bonus = 0;                   // 0/1
    bool rinshan = false;
    // ---- 日麻专用 ----
    int fu = 0;
    int points = 0;                              // 和牌点数
    riichi::Limit limit = riichi::Limit::kNone;  // 满贯分档（出站经 ToWire 转整数）
    bool riichi = false;                         // 本局和牌者是否立直
    int dora = 0;
    int ura_dora = 0;
    int aka_dora = 0;              // 赤宝牌张数
    int kyoutaku = 0;              // 结算后剩余供托（通常 0）
    int kyoutaku_award = 0;        // v1.20：场终归一位的供托棒数（0 = 无）
    int kyoutaku_award_seat = -1;  // 收下这些棒的座位（无则 -1）
    int honba = 0;                 // 本局本场数（支付已计入 delta）
    std::vector<Tile> dora_indicators;
    std::array<int, kSeatCount> points_after {};  // 本局结束后的持点（日麻排名口径）
    std::array<bool, kSeatCount> tenpai {};       // 荒庄时的听牌家
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
                       const std::array<std::vector<Option>, kSeatCount>& options, bool chankan = false);
  void ResolveClaimWindow(GameStep& step);
  void ApplyMeld(GameStep& step, int seat, protocol::Action action, const std::vector<Tile>& client_tiles,
                 int from_seat, Tile claimed_tile);
  // 自理杠（ankan / kakan）：只在本家打牌窗口；补摸 1 张后重开打牌窗口。
  void ApplySelfKan(GameStep& step, int seat, protocol::Action action, const std::vector<Tile>& client_tiles);
  // 抢杠窗口（v1.15）：加杠那张牌先给三家一次「和/过」，没人抢才真正把杠落地。
  void OpenChankanWindow(GameStep& step, int kan_seat, Tile tile);
  void EndRoundWithWin(GameStep& step, int winner_seat, int from_seat, Tile winning_tile, bool tsumo,
                       bool is_chankan = false);
  void EndRoundWithDraw(GameStep& step);
  // 局终 → 场终：totals 累加 → 连庄/轮庄 → game_end → 场终 match_end，否则 kRoundEnd +
  // 武装推进令牌（推进由房间的定时消息驱动）。
  void AdvanceAfterRound(GameStep& step, Settlement& settlement);
  void FinishMatch(GameStep& step);
  std::array<int, kSeatCount> ComputeFinalRanks() const;

  // ---- client frame handlers --------------------------------------------------------
  // Validate* are the read-only half (io: parse, check, never mutate); Apply*Frame is the
  // mutation half and assumes the matching Validate* returned kAccept.
  GameVerdict ValidateDiscard(int seat, const json::Value& frame) const;
  GameVerdict ValidateAction(int seat, const json::Value& frame) const;
  void ApplyDiscardFrame(GameStep& step, int seat, const json::Value& frame);
  void ApplyActionFrame(GameStep& step, int seat, const json::Value& frame);

  // ---- helpers ----------------------------------------------------------------------
  uint64_t Transition();
  void ResetClaimWindow();
  bool IsAutomated(int seat) const { return seats_[static_cast<size_t>(seat)].is_bot; }
  bool NeedsPlayerReply(int seat) const {
    return !IsAutomated(seat) && seats_[static_cast<size_t>(seat)].wired;
  }
  bool SeatPresent(int seat) const { return !disconnected_[static_cast<size_t>(seat)]; }
  std::optional<Tile> DefaultDiscardFor(int seat) const;
  // Takes the next tile off the live wall (a plain draw, no dead wall involved).
  Tile DrawFromWall();
  // 岭上补牌：从王牌头部取一张，并用牌山山顶牌补满王牌（王牌恒 14 张）。
  Tile DrawReplacementTile();
  // 翻开下一张宝牌指示牌（开局一张，每杠一张，最多 5 张）。
  void FlipDoraIndicator();

  // --- AI pacing (GAME_DESIGN v1.4 §5.4) ------------------------------------------------
  bool ScheduleAiAction(GameStep& step, int64_t delay_ms);
  bool PlayAutomatedDiscard(GameStep& step);
  bool BotDeclaresTsumo(GameStep& step, int seat);
  void ResolveAutomatedClaimAnswers();
  bool HasPendingHumanWin() const;
  Tile TsumoWinningTile(int seat) const;
  void OpenWindowTimer(GameStep& step);

  // ---- frames -----------------------------------------------------------------------
  std::array<std::string, kSeatCount> StateSyncFrames() const;
  std::array<std::string, kSeatCount> StateSyncFramesRevealed() const;
  bool SeatHoldsAnkan(int seat) const;
  json::Value BuildSeatsArray(int viewer) const;
  static constexpr int kSeatsMaskAll = -1;
  static constexpr int kSeatsRevealAll = -2;
  std::string BuildStateSync(int for_seat, const std::string& seats_json) const;
  std::string BuildStateSync(int for_seat) const;
  json::Value BuildTenpaiFrame(int seat) const;
  void DeliverTenpai(GameStep& step, int seat);
  std::string BuildGameStart(int for_seat) const;
  std::string BuildGameEnd(const Settlement& settlement) const;
  std::string BuildMatchEnd() const;
  void BroadcastSettlement(GameStep& step, const Settlement& settlement);

  // ---- 日麻规则 ----------------------------------------------------------------------
  // 打牌窗口的选项。`riichi_tiles` 非空时（只有真正构造 action_request 的调用点才传）回填
  // 立直宣言牌候选，并按候选是否为空裁剪 `riichi` 选项（v1.21 §4.4）。
  std::vector<Option> DiscardOptionsFor(int seat, std::vector<Tile>* riichi_tiles = nullptr) const;
  // 立直宣言牌候选：打出去之后仍在听牌、且不落振听的牌面，升序去重。
  std::vector<Tile> RiichiDiscardTilesFor(int seat) const;
  std::vector<Option> ClaimOptionsFor(int seat, Tile tile, bool is_next_player, int from_seat) const;
  // 没有役就不能和（宝牌不算役）。用**完整上下文**判，这样立直/一发/海底/杠上开花这类
  // 「状态役」也能算进去。
  bool HasYakuForWin(int seat, Tile winning_tile, bool tsumo, int from_seat, bool is_chankan = false) const;
  // 和牌形：日麻要额外认国士无双 —— 共享的 `IsWinningHand` 是番数制口径，没有这个役。
  bool IsWinShape(const std::vector<Tile>& concealed, const std::vector<Meld>& melds, Tile winning_tile,
                  bool tsumo) const;
  bool IsFuriten(int seat) const;
  bool RiichiDeclared(int seat) const { return riichi_[static_cast<size_t>(seat)]; }
  // 立直宣言（与打牌同一条 action）：校验后扣 1000 供托、置位，再由调用方打出该牌。
  bool CanDeclareRiichiNow(int seat, Tile discard) const;
  void DeclareRiichi(GameStep& step, int seat);
  bool BotShouldRiichi(int seat) const;
  riichi::WinContext BuildRiichiContext(int winner_seat, int from_seat, bool tsumo, Tile winning_tile,
                                        bool is_chankan = false) const;
  void ApplyDeltaToPoints(const std::array<int, kSeatCount>& delta);

  // ---- state ------------------------------------------------------------------------
  std::string room_id_;
  uint64_t seed_;
  int action_timeout_ms_ = 0;
  int ai_delay_ms_ = 0;
  int inter_hand_delay_ms_ = 0;
  bool deadlines_supported_ = false;
  std::vector<GameSeat> seats_;
  std::array<bool, kSeatCount> disconnected_ {};  // fed by OnSeatPresence
  std::array<std::vector<Tile>, kSeatCount> hands_;
  std::array<std::vector<Meld>, kSeatCount> melds_;
  std::array<std::vector<Tile>, kSeatCount> discards_;
  std::vector<Tile> wall_;
  int next_index_ = 0;
  int remaining_ = 0;
  // 可摸区上界：日麻 = 136 - 14 张王牌 = 122。`remaining_` 一律派生自 drawable_end_ - next_index_，
  // 禁止自减（RULESET §1.5 红线）。
  int drawable_end_ = kTileCount;
  // 王牌 14 张：0..3 岭上牌 / 4..8 宝牌指示牌 / 9..13 里宝指示牌。
  std::vector<Tile> dead_wall_;
  int dead_draw_index_ = 0;
  std::vector<Tile> dora_indicators_;      // 已翻开的指示牌
  std::array<int, kSeatCount> points_ {};  // 持点（25000 起）
  int kyoutaku_ = 0;                       // 供托（立直棒总数 × 1000）
  int honba_ = 0;                          // 本场数
  int round_wind_ = 0;                     // 场风：0 东 / 1 南（每局开始时按 round_index_ 推导）
  std::array<bool, kSeatCount> riichi_ {};         // 本局已宣言立直
  std::array<bool, kSeatCount> ippatsu_ {};        // 一发仍然有效（鸣牌后清除）
  std::array<bool, kSeatCount> ippatsu_armed_ {};  // 宣言后尚未跨过「下一摸」
  std::array<bool, kSeatCount> furiten_ever_ {};   // 见逃振听（本局内持久）
  bool draw_dealer_tenpai_ = false;                // 荒庄时庄家是否听牌（决定连庄/轮庄）
  int dealer_ = 0;
  int current_seat_ = 0;
  uint64_t seq_ = 0;
  Phase phase_ = Phase::kIdle;
  bool game_started_ = false;

  // v1.6（C1 场次状态）。跨局存活、绝不在 StartRound 里重置：room_id_/seats_/seq_/match_totals_。
  int round_index_ = 0;    // 0-based；连庄不推进，轮庄 +1（GAME_DESIGN v1.10 §3.3）
  int round_count_ = 4;    // 东场 4 / 半庄 8（由 options_json 的 length 决定）
  int hands_played_ = 0;   // 已下发的 game_end 次数（含连庄）
  int renchan_count_ = 0;  // 本场累计连庄次数；> round_count_ 强制场终
  std::array<int, kSeatCount> match_totals_ {};
  std::array<int, kSeatCount> dealer_hands_ {};  // B3：各座位本场坐庄手数（每局 +1，含连庄）
  // v1.7 §5.16：`tenpai_seq` 是**每座位一条的独立单调计数器**（从 1 起、跨局不重置；
  // 与 room.seq 完全解耦——挂 room.seq 会把正常 action_request 判成过期）。
  std::array<uint64_t, kSeatCount> tenpai_seq_ {};

  // v1.6（C6 局间推进令牌）：true = 本局的推进消息已武装，等定时到达
  bool round_advance_pending_ = false;
  // v1.8（播报窗）：true = 场终（match_end）的延迟消息已武装，等定时到达。
  bool match_end_pending_ = false;
  // 杠后打开的打牌窗口里自摸且和牌张 == 它 ⇒ 杠上开花
  std::optional<Tile> kong_draw_tile_;

  // discard window
  bool discard_window_open_ = false;
  // Whether the open discard window allows a self-draw win at all (false right after a
  // pong/chow, where the hand is not a complete 14-tile form). Set by OpenDiscardWindow.
  bool discard_window_win_check_ = false;
  std::optional<Tile> drawn_tile_;  // draw record of the current seat (timeout default)
  // PROTOCOL v2.2 §8.5: the exact frame the reconnecting seat needs.
  std::vector<Option> discard_options_;
  std::vector<Tile> discard_riichi_tiles_;
  int64_t window_deadline_ms_ = 0;

  // claim window
  bool claim_open_ = false;
  // 本次鸣牌窗口是「抢杠」而不是「抢打出的牌」：没人抢时要回到加杠者的自理杠流程。
  bool claim_chankan_ = false;
  int claim_from_seat_ = -1;
  Tile claim_tile_ {};
  std::array<bool, kSeatCount> claim_recipient_ {};
  std::array<std::vector<Option>, kSeatCount> claim_options_;
  std::array<ClaimAnswer, kSeatCount> claim_answers_;
  int claim_pending_humans_ = 0;
  std::optional<Settlement> last_settlement_;

  // AI pacing (GAME_DESIGN v1.4 §5.4).
  bool ai_discard_pending_ = false;
  bool ai_claim_pending_ = false;
};

// 这个玩法下一共有几局：东场（length=0）4 局 / 半庄（length=1）8 局。
int RiichiRoundCountOf(int length_option);

}  // namespace mahjong
