// 日麻规则库单测：逐档对照标准点数表（30 符 3 番 3900 / 满贯 8000 / 役满 32000 ...）。
//
// 这些断言是「算分正确性」的唯一防线：房间层只做分派，不重新算分。

#include "riichi_rules.h"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace mahjong::riichi {
namespace {

std::vector<Tile> Tiles(std::initializer_list<const char*> texts) {
  std::vector<Tile> out;
  for (const char* text : texts) {
    const std::optional<Tile> tile = ParseTile(text);
    EXPECT_TRUE(tile.has_value()) << text;
    if (tile.has_value()) {
      out.push_back(*tile);
    }
  }
  return out;
}

Meld Pong(std::initializer_list<const char*> texts) {
  Meld meld;
  meld.type = MeldType::kPong;
  meld.tiles = Tiles(texts);
  SortTiles(meld.tiles);
  return meld;
}

Meld Chow(std::initializer_list<const char*> texts) {
  Meld meld;
  meld.type = MeldType::kChow;
  meld.tiles = Tiles(texts);
  SortTiles(meld.tiles);
  return meld;
}

bool HasId(const std::vector<Yaku>& yaku, std::string_view id) {
  for (const Yaku& item : yaku) {
    if (item.id == id) {
      return true;
    }
  }
  return false;
}

WinContext Tsumo(bool dealer = false) {
  WinContext ctx;
  ctx.winner_seat = dealer ? 0 : 1;
  ctx.dealer_seat = 0;
  ctx.from_seat = -1;
  ctx.seat_wind = dealer ? 0 : 1;
  return ctx;
}

WinContext Ron(int from_seat, bool dealer = false) {
  WinContext ctx;
  ctx.winner_seat = dealer ? 0 : 1;
  ctx.dealer_seat = 0;
  ctx.from_seat = from_seat;
  ctx.seat_wind = dealer ? 0 : 1;
  return ctx;
}

// 平和基准手：123m 456m 789p 234s 99p，和 4s（两面）。含老头牌，故不成立断幺九。
std::vector<Tile> PinfuHand() {
  return Tiles({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "2s", "3s", "4s", "9p", "9p"});
}

// 指示牌 → 宝牌（数牌 9→1、风 东南西北循环、三元 白发中→发中白）
TEST(RiichiDora, IndicatorMapping) {
  EXPECT_EQ(FaceOf(*ParseTile("1m")), DoraFaceFromIndicator(*ParseTile("9m")));
  EXPECT_EQ(FaceOf(*ParseTile("4m")), DoraFaceFromIndicator(*ParseTile("3m")));
  EXPECT_EQ(FaceOf(*ParseTile("2z")), DoraFaceFromIndicator(*ParseTile("1z")));  // 东 → 南
  EXPECT_EQ(FaceOf(*ParseTile("1z")), DoraFaceFromIndicator(*ParseTile("4z")));  // 北 → 东
  EXPECT_EQ(FaceOf(*ParseTile("5z")), DoraFaceFromIndicator(*ParseTile("7z")));  // 中 → 白
  EXPECT_EQ(FaceOf(*ParseTile("6z")), DoraFaceFromIndicator(*ParseTile("5z")));  // 白 → 发
}

// 平和自摸：20 符 2 番 → 闲家 400 / 庄家 700，合计 1500
TEST(RiichiScore, PinfuTsumo20Fu2Han) {
  const ScoreResult score = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), Tsumo());

  EXPECT_TRUE(HasId(score.yaku, "pinfu"));
  EXPECT_TRUE(HasId(score.yaku, "tsumo"));
  EXPECT_FALSE(HasId(score.yaku, "tanyao"));
  EXPECT_EQ(score.han, 2);
  EXPECT_EQ(score.fu, 20);
  EXPECT_EQ(score.points, 1500);
  EXPECT_EQ(score.limit, Limit::kNone);
  int sum = 0;
  for (int delta : score.delta) {
    sum += delta;
  }
  EXPECT_EQ(sum, 0);
}

// 平和荣和：30 符 1 番 → 1000（门前荣和 +10 符）
TEST(RiichiScore, PinfuRon30Fu) {
  const ScoreResult score = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), Ron(2));

  EXPECT_TRUE(HasId(score.yaku, "pinfu"));
  EXPECT_EQ(score.han, 1);
  EXPECT_EQ(score.fu, 30);
  EXPECT_EQ(score.points, 1000);
}

// 立直 + 平和：2 番 30 符荣和 → 2000
TEST(RiichiScore, RiichiPinfuRon2000) {
  WinContext ctx = Ron(2);
  ctx.is_riichi = true;
  const ScoreResult score = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), ctx);

  EXPECT_TRUE(HasId(score.yaku, "riichi"));
  EXPECT_EQ(score.han, 2);
  EXPECT_EQ(score.fu, 30);
  EXPECT_EQ(score.points, 2000);
}

// 断幺九 + 平和 + 自摸：20 符 3 番 → 700 / 1300，合计 2700
TEST(RiichiScore, TanyaoPinfuTsumo) {
  const std::vector<Tile> hand =
      Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "5p", "6p", "7p", "8s", "8s"});
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("4m"), Tsumo());

  EXPECT_TRUE(HasId(score.yaku, "tanyao"));
  EXPECT_TRUE(HasId(score.yaku, "pinfu"));
  EXPECT_EQ(score.han, 3);
  EXPECT_EQ(score.fu, 20);
  EXPECT_EQ(score.points, 2700);
}

// 七对子：25 符 2 番荣和 → 1600
TEST(RiichiScore, Chiitoitsu1600) {
  const std::vector<Tile> hand =
      Tiles({"1m", "1m", "2m", "2m", "3p", "3p", "4p", "4p", "5s", "5s", "6s", "6s", "7z", "7z"});
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("7z"), Ron(2));

  EXPECT_TRUE(HasId(score.yaku, "chiitoitsu"));
  EXPECT_EQ(score.han, 2);
  EXPECT_EQ(score.fu, 25);
  EXPECT_EQ(score.points, 1600);
}

// 副露役牌：1 番 30 符荣和 → 1000（明刻中 +4 符，副露 20 符按 30 符）
TEST(RiichiScore, OpenYakuhai1000) {
  const std::vector<Tile> concealed = Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "8s", "8s"});
  const std::vector<Meld> melds = {Pong({"7z", "7z", "7z"})};
  std::vector<Tile> hand = concealed;
  hand.push_back(*ParseTile("4p"));
  SortTiles(hand);

  const ScoreResult score = ScoreHand(hand, melds, *ParseTile("4p"), Ron(2));
  EXPECT_TRUE(HasId(score.yaku, "yakuhai"));
  EXPECT_FALSE(HasId(score.yaku, "toitoi"));  // 三组顺子不能是对对和
  EXPECT_FALSE(HasId(score.yaku, "riichi"));  // 副露不能立直
  EXPECT_EQ(score.han, 1);
  EXPECT_EQ(score.fu, 30);
  EXPECT_EQ(score.points, 1000);
}

// 满贯以上：清一色门前必然踩到分档（≥ 满贯 8000）
TEST(RiichiScore, ManganOrAbove) {
  const std::vector<Tile> hand =
      Tiles({"1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "1m", "2m", "3m", "5m", "5m"});
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("3m"), Ron(2));

  EXPECT_TRUE(HasId(score.yaku, "chinitsu"));
  EXPECT_GT(score.han, 0);
  EXPECT_GE(score.points, 8000);
  EXPECT_NE(score.limit, Limit::kNone);
}

// 役满：国士十三面（和牌张正是那张对子 ⇒ 和牌前十三种各一张）→ **双倍役满**，闲家 64000
TEST(RiichiScore, KokushiThirteenSidedDoubleYakuman) {
  const std::vector<Tile> hand =
      Tiles({"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z", "3z", "4z", "5z", "6z", "7z", "1z"});
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("1z"), Ron(2));

  ASSERT_TRUE(HasId(score.yaku, "kokushi_13"));
  EXPECT_FALSE(HasId(score.yaku, "kokushi"));
  EXPECT_EQ(score.yakuman_count, 2);
  EXPECT_EQ(score.limit, Limit::kYakuman);
  EXPECT_EQ(score.han, 26);        // 13 × 2（`han_breakdown.base` 与 `win.han` 仍相等）
  EXPECT_EQ(score.points, 64000);  // 闲家荣和 = 16000 base × 4
}

// 国士无双（单骑：和牌张是缺的那一种）→ 1 倍役满 32000
TEST(RiichiScore, KokushiSingleWaitYakuman) {
  // 和牌前：缺 7z、1z 成对；和 7z ⇒ 1z 才是对子 ⇒ 只听 7z，不构成十三面
  const std::vector<Tile> hand =
      Tiles({"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z", "3z", "4z", "5z", "6z", "1z", "7z"});
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("7z"), Ron(2));

  ASSERT_TRUE(HasId(score.yaku, "kokushi"));
  EXPECT_FALSE(HasId(score.yaku, "kokushi_13"));
  EXPECT_EQ(score.yakuman_count, 1);
  EXPECT_EQ(score.points, 32000);
}

// 宝牌：指示牌 3m ⇒ 手里每张 4m 各 +1 番
TEST(RiichiScore, DoraAddsHan) {
  WinContext ctx = Ron(2);
  ctx.dora_indicators = Tiles({"3m"});  // 宝牌 = 4m
  const ScoreResult score = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), ctx);

  EXPECT_EQ(score.dora, 1);
  EXPECT_TRUE(HasId(score.yaku, "dora"));
  EXPECT_EQ(score.han, 2);  // pinfu 1 + dora 1
  EXPECT_EQ(score.points, 2000);
}

// 里宝只在立直和牌时计入
TEST(RiichiScore, UraDoraRequiresRiichi) {
  WinContext ctx = Ron(2);
  ctx.ura_indicators = Tiles({"3m"});
  const ScoreResult without_riichi = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), ctx);
  EXPECT_EQ(without_riichi.ura_dora, 0);

  ctx.is_riichi = true;
  const ScoreResult with_riichi = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), ctx);
  EXPECT_EQ(with_riichi.ura_dora, 1);
  EXPECT_TRUE(HasId(with_riichi.yaku, "ura_dora"));
}

// 庄家与闲家的支付差异：庄家荣和 = 6×base，闲家 = 4×base
TEST(RiichiScore, DealerPayment) {
  const ScoreResult dealer = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), Ron(2, /*dealer=*/true));
  const ScoreResult non_dealer = ScoreHand(PinfuHand(), {}, *ParseTile("4s"), Ron(2));
  EXPECT_EQ(non_dealer.points, 1000);  // 1 番 30 符闲家
  EXPECT_EQ(dealer.points, 1500);      // 1 番 30 符庄家
}

// 振听：自家牌河里有自己的听牌张 ⇒ 永久振听
TEST(RiichiFuriten, OwnDiscardBlocksRon) {
  const std::vector<Tile> waiting =
      Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "5p", "6p", "8s", "8s"});
  const std::vector<Tile> waits = WaitingTiles(waiting, {});
  ASSERT_FALSE(waits.empty());
  EXPECT_TRUE(IsPermanentFuriten(waiting, {}, {waits.front()}));
  EXPECT_FALSE(IsPermanentFuriten(waiting, {}, Tiles({"1m"})));
}

// 振听时 ClaimOptions 不产出 win
TEST(RiichiFuriten, ClaimOptionsDropsWin) {
  const std::vector<Tile> hand = Tiles({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "2s", "3s", "9p", "9p"});
  const std::optional<Tile> winning = ParseTile("4s");
  ASSERT_TRUE(winning.has_value());
  const std::vector<Option> open_claim = ClaimOptions(hand, {}, *winning, 30, true, /*furiten=*/false);
  const std::vector<Option> furiten = ClaimOptions(hand, {}, *winning, 30, true, /*furiten=*/true);
  EXPECT_TRUE(FindOption(open_claim, protocol::Action::kWin) != nullptr);
  EXPECT_TRUE(FindOption(furiten, protocol::Action::kWin) == nullptr);
}

// 无役不能和：只有宝牌不算役（房间层会过滤 win 选项，这里验第二道防线）
TEST(RiichiScore, NoYakuCannotWin) {
  const std::vector<Tile> concealed = Tiles({"3p", "4p", "5p", "6p", "7p", "8p", "2s", "3s", "4s", "5z"});
  const std::vector<Meld> melds = {Pong({"2m", "2m", "2m"})};
  std::vector<Tile> hand = concealed;
  hand.push_back(*ParseTile("5z"));
  SortTiles(hand);
  WinContext ctx = Ron(2);
  ctx.dora_indicators = Tiles({"4p"});  // 宝牌 5p：有宝牌但无役，仍然不能和

  const ScoreResult score = ScoreHand(hand, melds, *ParseTile("5z"), ctx);
  EXPECT_TRUE(score.yaku.empty());
  EXPECT_EQ(score.points, 0);
  EXPECT_EQ(score.han, 0);
}

// 抢杠（v1.15）：只有 win / pass；抢杠本身是 1 番役，所以「无役」的手牌也能抢
TEST(RiichiChankan, OpenHandWithoutYakuCanStillRob) {
  const std::vector<Meld> melds = {Chow({"2m", "3m", "4m"})};
  const std::vector<Tile> concealed = Tiles({"1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "2z", "2z"});
  WinContext ctx = Ron(1);
  ctx.seat_wind = 0;

  // 同一手牌在普通荣和里无役（见 WinningWaits 用例），但抢杠 1 番让它成立
  const std::vector<Option> options = ChankanOptions(concealed, melds, *ParseTile("6s"), ctx, false);
  ASSERT_EQ(options.size(), 2u);
  EXPECT_EQ(options[0].action, protocol::Action::kWin);
  EXPECT_EQ(options[1].action, protocol::Action::kPass);

  const ScoreResult score = ScoreHand(
      [&] {
        std::vector<Tile> hand = concealed;
        hand.push_back(*ParseTile("6s"));
        SortTiles(hand);
        return hand;
      }(),
      melds, *ParseTile("6s"),
      [&] {
        WinContext chankan = ctx;
        chankan.is_chankan = true;
        return chankan;
      }());
  ASSERT_EQ(score.yaku.size(), 1u);
  EXPECT_EQ(score.yaku[0].id, "chankan");
  EXPECT_EQ(score.han, 1);
}

// 牌型不成和 / 振听 ⇒ 只有 pass
TEST(RiichiChankan, NotAWinningHandOrFuritenGivesPassOnly) {
  const std::vector<Meld> melds = {Chow({"2m", "3m", "4m"})};
  const std::vector<Tile> concealed = Tiles({"1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "2z", "2z"});
  WinContext ctx = Ron(1);
  ctx.seat_wind = 0;

  const std::vector<Option> not_winning = ChankanOptions(concealed, melds, *ParseTile("1p"), ctx, false);
  ASSERT_EQ(not_winning.size(), 1u);
  EXPECT_EQ(not_winning[0].action, protocol::Action::kPass);

  const std::vector<Option> furiten = ChankanOptions(concealed, melds, *ParseTile("6s"), ctx, true);
  ASSERT_EQ(furiten.size(), 1u);
  EXPECT_EQ(furiten[0].action, protocol::Action::kPass);
}

// ---- 国士无双（v1.17）：牌型门禁 + 等待张 ------------------------------------
// 截图里那手牌：1m 9m 1p 9p 1s 9s 东南西北白发中中
std::vector<Tile> KokushiHand() {
  return Tiles({"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z", "3z", "4z", "5z", "6z", "7z", "7z"});
}

TEST(RiichiKokushi, ShapeAndScoring) {
  const std::vector<Tile> hand = KokushiHand();
  EXPECT_TRUE(IsKokushiHand(hand, {}));
  EXPECT_TRUE(IsRiichiWinningHand(hand, {}, *ParseTile("7z"), /*tsumo=*/true));
  EXPECT_TRUE(IsRiichiWinningHand(hand, {}, *ParseTile("7z"), /*tsumo=*/false));

  // 共享层是番数制口径：classic 没有国士，这里必须保持 false（别在共享层「顺手」修）
  EXPECT_FALSE(IsWinningHand(hand, {}, *ParseTile("7z"), /*tsumo=*/true))
      << "classic 的和牌形不含国士 —— 修在 riichi 侧";

  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("7z"), Tsumo());
  ASSERT_EQ(score.yaku.size(), 1u);
  // 7z 成对 ⇒ 和牌前是十三种各一张 ⇒ 国士十三面（双倍役满）
  EXPECT_EQ(score.yaku[0].id, "kokushi_13");
  EXPECT_EQ(score.limit, Limit::kYakuman);
  EXPECT_EQ(score.points, 64000);  // 闲家自摸 16000/16000/32000

  // 混进一张非幺九字 ⇒ 不是国士
  std::vector<Tile> broke = hand;
  broke.front() = *ParseTile("2m");
  SortTiles(broke);
  EXPECT_FALSE(IsKokushiHand(broke, {}));
  // 少一种（缺 6z）⇒ 不是
  EXPECT_FALSE(
      IsKokushiHand(Tiles({"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z", "3z", "4z", "5z", "7z", "7z"}), {}));
}

// 十三面听：13 种各一张 ⇒ 任何幺九字都能和；单骑听：缺一种 + 一对 ⇒ 只听缺的那张
TEST(RiichiKokushi, WaitTiles) {
  const std::vector<Tile> thirteen_wait =
      Tiles({"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z", "3z", "4z", "5z", "6z", "7z"});
  const std::vector<Tile> all_wait = RiichiWaitingTiles(thirteen_wait, {});
  EXPECT_EQ(all_wait.size(), 13u);
  EXPECT_TRUE(WaitingTiles(thirteen_wait, {}).empty()) << "共享层认不出国士的等待";

  // 12 种 + 一对（缺 6z，7z 成对）⇒ 只听 6z
  const std::vector<Tile> hand13 =
      Tiles({"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z", "3z", "4z", "5z", "7z", "7z"});
  const std::vector<Tile> one = RiichiWaitingTiles(hand13, {});
  ASSERT_EQ(one.size(), 1u);
  EXPECT_EQ(one.front(), *ParseTile("6z"));
}

// 窗口：手里已经是国士时，打牌窗口 / 鸣牌窗口都要给 win
TEST(RiichiKokushi, WindowsOfferWin) {
  const std::vector<Tile> hand = KokushiHand();
  EXPECT_TRUE(FindOption(DiscardOptions(hand, {}, 30, /*riichi_declared=*/false), protocol::Action::kWin) != nullptr);

  std::vector<Tile> hand13 = hand;
  hand13.pop_back();
  const std::vector<Option> claim = ClaimOptions(hand13, {}, *ParseTile("7z"), /*remaining=*/30,
                                                 /*is_next_player=*/false, /*furiten=*/false);
  EXPECT_TRUE(FindOption(claim, protocol::Action::kWin) != nullptr);

  // 振听仍然封住荣和（国士也一样）
  const std::vector<Option> furiten = ClaimOptions(hand13, {}, *ParseTile("7z"), 30, false, true);
  EXPECT_TRUE(FindOption(furiten, protocol::Action::kWin) == nullptr);
}

// 天和 / 地和（v1.17）：第一巡自摸的役满，由房间按牌河/副露状态置位
TEST(RiichiYakuman, TenhouAndChiihou) {
  const std::vector<Tile> hand =
      Tiles({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "2s", "3s", "4s", "9p", "9p"});

  WinContext dealer = Tsumo(/*dealer=*/true);  // winner_seat=0, dealer_seat=0
  dealer.is_tenhou = true;
  const ScoreResult tenhou = ScoreHand(hand, {}, *ParseTile("4s"), dealer);
  ASSERT_FALSE(tenhou.yaku.empty());
  EXPECT_EQ(tenhou.yaku[0].id, "tenhou");
  EXPECT_EQ(tenhou.limit, Limit::kYakuman);
  EXPECT_EQ(tenhou.points, 48000);  // 庄家役满自摸：16000 × 3

  WinContext child = Tsumo();  // 闲家
  child.is_chiihou = true;
  const ScoreResult chiihou = ScoreHand(hand, {}, *ParseTile("4s"), child);
  ASSERT_FALSE(chiihou.yaku.empty());
  EXPECT_EQ(chiihou.yaku[0].id, "chiihou");
  EXPECT_EQ(chiihou.limit, Limit::kYakuman);
  EXPECT_EQ(chiihou.points, 32000);  // 16000 + 8000 + 8000

  // 没有置位时照常按普通手牌算（不能把每局都当日和）
  const ScoreResult plain = ScoreHand(hand, {}, *ParseTile("4s"), Tsumo());
  EXPECT_LT(plain.han, 13);
  EXPECT_NE(plain.limit, Limit::kYakuman);
}

// 场风 / 自风（v1.14 半庄）：东场庄家=东，南场庄家=南；自风随庄家逆时针。
TEST(RiichiWind, RoundAndSeatWind) {
  EXPECT_EQ(RoundWindOf(0), 0);
  EXPECT_EQ(RoundWindOf(3), 0);
  EXPECT_EQ(RoundWindOf(4), 1);  // 南场 1 局
  EXPECT_EQ(RoundWindOf(7), 1);
  EXPECT_EQ(RoundWindOf(8), 2);  // 西入（本版不做半庄以上，但公式不该崩）

  // 东场：庄家（seat 0）是东，下家南、对家西、上家北
  EXPECT_EQ(SeatWindOf(0, 0, 0), 0);
  EXPECT_EQ(SeatWindOf(0, 1, 0), 1);
  EXPECT_EQ(SeatWindOf(0, 2, 0), 2);
  EXPECT_EQ(SeatWindOf(0, 3, 0), 3);

  // 东场 2 局：庄家换到 seat 1，自风整体跟着转
  EXPECT_EQ(SeatWindOf(0, 1, 1), 0);
  EXPECT_EQ(SeatWindOf(0, 2, 1), 1);
  EXPECT_EQ(SeatWindOf(0, 0, 1), 3);

  // 南场 1 局：庄家（seat 0）是南 → 自风 1，其余 西/北/东
  EXPECT_EQ(SeatWindOf(1, 0, 0), 1);
  EXPECT_EQ(SeatWindOf(1, 1, 0), 2);
  EXPECT_EQ(SeatWindOf(1, 2, 0), 3);
  EXPECT_EQ(SeatWindOf(1, 3, 0), 0);  // 上家回到东

  // 连风牌（南场庄家的南）：场风与自风同面 ⇒ 役牌计 2 番
  EXPECT_EQ(SeatWindOf(1, 0, 0), RoundWindOf(4));
}

// 听牌提示的「有役」口径（v1.13 §12.8）：**形听 ≠ 能和**。
// 副露无役：吃 234m + 暗手 123m 456p 78s 22z，形听 6s/9s，但两张都无役 ⇒ 死听
TEST(RiichiTenpaiHint, OpenHandWithoutYakuHasNoWinningWait) {
  const std::vector<Meld> melds = {Chow({"2m", "3m", "4m"})};
  const std::vector<Tile> concealed = Tiles({"1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "2z", "2z"});

  const std::vector<Tile> shape_waits = WaitingTiles(concealed, melds);
  ASSERT_EQ(shape_waits.size(), 2u);  // 6s / 9s：形听成立（流局要收罚符）

  WinContext ctx = Ron(1);
  ctx.seat_wind = 0;  // 2z = 南，既非场风也非自风
  EXPECT_TRUE(WinningWaits(concealed, melds, ctx).empty()) << "副露无役：形听但没有任何一张能荣和";
}

// 副露但有役（白）：两张等待张都能和
TEST(RiichiTenpaiHint, OpenHandWithYakuhaiKeepsWaits) {
  const std::vector<Meld> melds = {Chow({"2m", "3m", "4m"}), Pong({"5z", "5z", "5z"})};
  const std::vector<Tile> concealed = Tiles({"4p", "5p", "6p", "7s", "8s", "2z", "2z"});

  ASSERT_EQ(WaitingTiles(concealed, melds).size(), 2u);
  WinContext ctx = Ron(1);
  EXPECT_EQ(WinningWaits(concealed, melds, ctx).size(), 2u);
}

// 门清逐张不同：123p 无役、234p 有断幺 —— 提示必须只留下 4p
TEST(RiichiTenpaiHint, PerWaitYakuDiffers) {
  const std::vector<Tile> concealed =
      Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "8s", "8s", "8s", "2p", "3p", "8p", "8p"});
  const std::vector<Tile> shape_waits = WaitingTiles(concealed, {});
  ASSERT_EQ(shape_waits.size(), 2u);  // 1p / 4p

  const std::vector<Tile> winning = WinningWaits(concealed, {}, Ron(1));
  ASSERT_EQ(winning.size(), 1u) << "1p 带老头牌、无役；4p 才是断幺";
  EXPECT_EQ(winning.front(), *ParseTile("4p"));
}

// 已立直：立直本身就是役 ⇒ 全部等待张都能和（这也是「立直治无役」的实现口径）
TEST(RiichiTenpaiHint, RiichiMakesEveryWaitLive) {
  const std::vector<Tile> concealed =
      Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "8s", "8s", "8s", "2p", "3p", "8p", "8p"});
  WinContext ctx = Ron(1);
  ctx.is_riichi = true;
  EXPECT_EQ(WinningWaits(concealed, {}, ctx).size(), 2u);
}

// 非等待态（14 张）没有「等待张」可谈
TEST(RiichiTenpaiHint, NonWaitingStateIsEmpty) {
  const std::vector<Tile> hand =
      Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "8s", "8s", "8s", "2p", "3p", "4p", "8p", "8p"});
  EXPECT_TRUE(WinningWaits(hand, {}, Ron(1)).empty());
}

// 流局听牌罚符：3000 点平摊；全听/全不听无人收付
TEST(RiichiDraw, NotenPenaltySplit) {
  const std::array<int, 4> one = {3000, -1000, -1000, -1000};
  const std::array<int, 4> two = {1500, 1500, -1500, -1500};
  const std::array<int, 4> three = {1000, 1000, 1000, -3000};
  const std::array<int, 4> none = {0, 0, 0, 0};
  EXPECT_EQ(DrawPenalty({true, false, false, false}), one);
  EXPECT_EQ(DrawPenalty({true, true, false, false}), two);
  EXPECT_EQ(DrawPenalty({true, true, true, false}), three);
  EXPECT_EQ(DrawPenalty({false, false, false, false}), none);
  EXPECT_EQ(DrawPenalty({true, true, true, true}), none);
}

// 立直条件：门前 + 行动态（3n+2）；副露、牌山不足、非听牌都不可
TEST(RiichiRiichi, CanDeclare) {
  EXPECT_TRUE(CanDeclareRiichi(PinfuHand(), {}, 30));

  const std::vector<Meld> open_meld = {Pong({"7z", "7z", "7z"})};
  const std::vector<Tile> open_hand = Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "5p", "6p"});
  EXPECT_FALSE(CanDeclareRiichi(open_hand, open_meld, 30));

  EXPECT_FALSE(CanDeclareRiichi(PinfuHand(), {}, 3));  // 牌山不足 4 张

  // 未听牌（三张孤张字牌，打掉任意一张都不听）
  const std::vector<Tile> not_tenpai =
      Tiles({"1z", "2z", "4z", "3z", "2m", "5m", "8m", "2p", "5p", "8p", "2s", "5s", "8s", "1m"});
  EXPECT_FALSE(CanDeclareRiichi(not_tenpai, {}, 30));
}

// 立直宣言牌候选（v1.21）：只有「打出去仍在听牌」的牌面才算，且按牌面升序去重。
TEST(RiichiRiichi, DiscardCandidatesOnlyTenpaiLeavingFaces) {
  // 123456789m 11p 34p 9s：只有打 9s 之后仍听 2p/5p，其余牌打完都不听。
  const std::vector<Tile> hand =
      Tiles({"1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "1p", "1p", "3p", "4p", "9s"});
  const std::vector<Face> faces = RiichiDiscardFaces(hand, {});
  ASSERT_EQ(faces.size(), 1u);
  EXPECT_EQ(faces.front(), FaceOf(*ParseTile("9s")));

  // 副露过（非门前）⇒ 一张宣言牌都没有
  Meld pong;
  pong.type = MeldType::kPong;
  pong.tiles = {*ParseTile("1z"), *ParseTile("1z"), *ParseTile("1z")};
  EXPECT_TRUE(RiichiDiscardFaces(hand, {pong}).empty());
}

TEST(RiichiRiichi, DiscardCandidatesSortedAndDeduplicated) {
  // 七对子 14 张：打掉任意一面都还在单骑听，且**每面两张只收一次**（去重）。
  const std::vector<Tile> hand =
      Tiles({"1z", "1z", "2z", "2z", "3z", "3z", "4z", "4z", "5z", "5z", "6z", "6z", "7z", "7z"});
  const std::vector<Face> faces = RiichiDiscardFaces(hand, {});
  ASSERT_EQ(faces.size(), 7u) << "14 张 = 7 个牌面，不因为成对就出现 14 条";
  for (size_t index = 0; index < faces.size(); ++index) {
    const std::string code = std::to_string(index + 1) + "z";
    EXPECT_EQ(faces[index], FaceOf(*ParseTile(code))) << "候选按牌面升序：" << code;
  }
}

// 立直后不在打牌窗口提供杠；宣告前提供
TEST(RiichiRiichi, DiscardOptionsDropKanAfterRiichi) {
  const std::vector<Tile> hand =
      Tiles({"1m", "1m", "1m", "1m", "2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "8s"});
  const std::vector<Option> before = DiscardOptions(hand, {}, 30, /*riichi_declared=*/false);
  const std::vector<Option> after = DiscardOptions(hand, {}, 30, /*riichi_declared=*/true);
  EXPECT_TRUE(FindOption(before, protocol::Action::kAnkan) != nullptr);
  EXPECT_TRUE(FindOption(after, protocol::Action::kAnkan) == nullptr);
  EXPECT_TRUE(FindOption(after, protocol::Action::kRiichi) == nullptr);  // 已立直，不再重复宣告
}

// 立直后：只有「不改变听牌」的暗杠被放行（v1.16）
TEST(RiichiRiichi, AnkanAfterRiichiOnlyWhenWaitsUnchanged) {
  // 1111m 234m 567m 234p 8s：听 8s 单骑
  const std::vector<Tile> hand =
      Tiles({"1m", "1m", "1m", "1m", "2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "8s"});
  // 摸到第 4 张 1m 再杠掉：听牌仍是 8s ⇒ 放行
  const std::vector<Option> legal = DiscardOptions(hand, {}, 30, /*riichi_declared=*/true, *ParseTile("1m"));
  EXPECT_TRUE(FindOption(legal, protocol::Action::kAnkan) != nullptr);
  EXPECT_TRUE(FindOption(legal, protocol::Action::kKakan) == nullptr);   // 立直手不可能有碰组
  EXPECT_TRUE(FindOption(legal, protocol::Action::kRiichi) == nullptr);  // 不重复宣告

  // 摸到的是 8s：杠掉 1m 会把单骑从 1m 变成 8s ⇒ 听牌变了，拒绝
  const std::vector<Option> changed = DiscardOptions(hand, {}, 30, /*riichi_declared=*/true, *ParseTile("8s"));
  EXPECT_TRUE(FindOption(changed, protocol::Action::kAnkan) == nullptr);

  // 不知道摸了什么 ⇒ 保守：一个杠都不给（老调用点行为不变）
  const std::vector<Option> unknown = DiscardOptions(hand, {}, 30, /*riichi_declared=*/true);
  EXPECT_TRUE(FindOption(unknown, protocol::Action::kAnkan) == nullptr);

  // 未立直：原有行为不变
  const std::vector<Option> before_riichi = DiscardOptions(hand, {}, 30, /*riichi_declared=*/false, *ParseTile("1m"));
  EXPECT_TRUE(FindOption(before_riichi, protocol::Action::kAnkan) != nullptr);
}

// ============================================================================
// 赤宝牌（v1.18）：5m/5p/5s 的第 4 张 = 赤牌，每张 +1 番；不是役，役满不加算
// ============================================================================

// 手牌里的赤 5：平和 + 赤 ⇒ 2 番 30 符 2000（同一个牌型不换赤牌时是 1000）
TEST(RiichiAka, ConcealedAkaAddsOneHan) {
  const std::vector<Tile> plain = PinfuHand();
  const std::vector<Tile> aka =
      Tiles({"1m", "2m", "3m", "4m", "0m", "6m", "7p", "8p", "9p", "2s", "3s", "4s", "9p", "9p"});
  const ScoreResult without = ScoreHand(plain, {}, *ParseTile("4s"), Ron(2));
  const ScoreResult with = ScoreHand(aka, {}, *ParseTile("4s"), Ron(2));

  EXPECT_EQ(without.aka, 0);
  EXPECT_EQ(without.points, 1000);
  EXPECT_EQ(with.aka, 1);
  EXPECT_TRUE(HasId(with.yaku, "aka_dora"));
  EXPECT_EQ(with.han, 2);  // pinfu 1 + 赤 1
  EXPECT_EQ(with.points, 2000);
  int sum = 0;
  for (const Yaku& item : with.yaku) {
    sum += item.han;
  }
  EXPECT_EQ(sum, with.han);  // yaku[].han 之和 == 总番（含宝牌条目）
}

// 副露里的赤 5 同样计入（碰 0s 5s 5s）
TEST(RiichiAka, AkaInsideMeldCounts) {
  const std::vector<Meld> melds = {Pong({"0s", "5s", "5s"})};
  // 234m 567m 234p 77s + 碰 5s，和 2p：断幺九 1 番 + 赤 1 番
  const std::vector<Tile> hand = Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "3p", "4p", "7s", "7s", "2p"});
  const ScoreResult score = ScoreHand(hand, melds, *ParseTile("2p"), Ron(2));

  EXPECT_TRUE(HasId(score.yaku, "tanyao"));
  EXPECT_EQ(score.aka, 1);
  EXPECT_TRUE(HasId(score.yaku, "aka_dora"));
  EXPECT_EQ(score.han, 2);
  EXPECT_GT(score.points, 0);
}

// 赤宝牌不是役：无役的手牌就算抓了三张赤牌也和不掉
TEST(RiichiAka, AkaIsNotAYaku) {
  const std::vector<Meld> melds = {Pong({"0s", "5s", "5s"})};
  // 123m 789p 123s 5z5z + 碰 5s、和 2s：一个役都没有（无断幺、无平和、无役牌）
  const std::vector<Tile> hand = Tiles({"1m", "2m", "3m", "7p", "8p", "9p", "1s", "3s", "5z", "5z", "2s"});
  const ScoreResult score = ScoreHand(hand, melds, *ParseTile("2s"), Ron(2));

  EXPECT_FALSE(HasId(score.yaku, "aka_dora"));  // 无役 ⇒ 整个手牌不算（与宝牌同口径）
  EXPECT_TRUE(score.yaku.empty());
  EXPECT_EQ(score.points, 0);
  EXPECT_EQ(YakuHan("aka_dora", /*closed=*/true), 1);
  EXPECT_FALSE(IsYakumanYaku("aka_dora"));
}

// ============================================================================
// 双倍役满（v1.18）：国士十三面 / 四暗刻单骑 / 纯正九莲 / 大四喜 = 2 倍
// ============================================================================

TEST(RiichiYakuman, MultiplierTable) {
  EXPECT_EQ(YakumanMultiplier("kokushi_13"), 2);
  EXPECT_EQ(YakumanMultiplier("suuankou_tanki"), 2);
  EXPECT_EQ(YakumanMultiplier("chuuren_junsei"), 2);
  EXPECT_EQ(YakumanMultiplier("daisuushii"), 2);

  EXPECT_EQ(YakumanMultiplier("kokushi"), 1);
  EXPECT_EQ(YakumanMultiplier("suuankou"), 1);
  EXPECT_EQ(YakumanMultiplier("chuuren"), 1);
  EXPECT_EQ(YakumanMultiplier("shousuushii"), 1);
  EXPECT_EQ(YakumanMultiplier("daisangen"), 1);
  EXPECT_EQ(YakumanMultiplier("tenhou"), 1);
  EXPECT_EQ(YakumanMultiplier("pinfu"), 0);

  for (const char* id : {"kokushi_13", "suuankou_tanki", "chuuren_junsei", "daisuushii"}) {
    EXPECT_TRUE(IsYakumanYaku(id)) << id;
  }
}

// 四暗刻单骑：111m 222m 333m 555s（含赤）+ 5p 单骑自摸 ⇒ 2 倍役满，闲家 64000
TEST(RiichiYakuman, SuuankouTankiIsDouble) {
  const std::vector<Tile> hand =
      Tiles({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "0s", "5s", "5s", "5p", "5p"});
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("5p"), Tsumo());

  ASSERT_TRUE(HasId(score.yaku, "suuankou_tanki"));
  EXPECT_FALSE(HasId(score.yaku, "suuankou"));
  EXPECT_EQ(score.yakuman_count, 2);
  EXPECT_EQ(score.han, 26);
  EXPECT_EQ(score.points, 64000);
  EXPECT_EQ(score.aka, 0);  // 役满不加算赤宝牌
}

// 四暗刻（双碰自摸，非单骑）⇒ 1 倍役满 32000
TEST(RiichiYakuman, SuuankouShanponIsSingle) {
  const std::vector<Tile> hand =
      Tiles({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4p", "4p", "4p", "5s", "5s"});
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("4p"), Tsumo());

  ASSERT_TRUE(HasId(score.yaku, "suuankou"));
  EXPECT_FALSE(HasId(score.yaku, "suuankou_tanki"));
  EXPECT_EQ(score.yakuman_count, 1);
  EXPECT_EQ(score.points, 32000);
}

// 纯正九莲：1112345678999 + 任意一张 ⇒ 九面听 2 倍；非纯正（本例单骑 9m）⇒ 1 倍
TEST(RiichiYakuman, ChuurenJunseiIsDouble) {
  const std::vector<Tile> junsei =
      Tiles({"1m", "1m", "1m", "2m", "3m", "4m", "5m", "5m", "6m", "7m", "8m", "9m", "9m", "9m"});
  const ScoreResult pure = ScoreHand(junsei, {}, *ParseTile("5m"), Tsumo());
  ASSERT_TRUE(HasId(pure.yaku, "chuuren_junsei"));
  EXPECT_EQ(pure.yakuman_count, 2);
  EXPECT_EQ(pure.points, 64000);

  // 1m1m1m 2m2m 3..8m 9m9m + 和 9m ⇒ 只听 9m 的九莲，不是九面听
  const std::vector<Tile> single =
      Tiles({"1m", "1m", "1m", "2m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "9m", "9m"});
  const ScoreResult plain = ScoreHand(single, {}, *ParseTile("9m"), Tsumo());
  ASSERT_TRUE(HasId(plain.yaku, "chuuren"));
  EXPECT_EQ(plain.yakuman_count, 1);
  EXPECT_EQ(plain.points, 32000);
}

// 多役满叠加：大四喜（2）+ 字一色（1）= 3 倍 ⇒ 闲家荣和 96000
TEST(RiichiYakuman, StackedYakumanAddsMultipliers) {
  const std::vector<Meld> melds = {Pong({"4z", "4z", "4z"})};  // 北 副露，排除四暗刻
  const std::vector<Tile> hand = Tiles({"1z", "1z", "1z", "2z", "2z", "2z", "3z", "3z", "3z", "5z", "5z"});
  const ScoreResult score = ScoreHand(hand, melds, *ParseTile("5z"), Ron(2));

  ASSERT_TRUE(HasId(score.yaku, "daisuushii"));
  ASSERT_TRUE(HasId(score.yaku, "tsuuiisou"));
  EXPECT_EQ(score.yakuman_count, 3);
  EXPECT_EQ(score.han, 39);
  EXPECT_EQ(score.points, 96000);
}

// 击飞（v1.19）：严格负点才算；恰好 0 点继续对局。
TEST(RiichiBust, OnlyNegativePointsBust) {
  EXPECT_FALSE(IsBusted({25000, 25000, 25000, 25000}));
  EXPECT_FALSE(IsBusted({25000, 25000, 25000, 0})) << "0 点不算击飞（雀魂口径）";
  EXPECT_FALSE(IsBusted({1, 1, 1, 0}));
  EXPECT_TRUE(IsBusted({25000, 25000, 25000, -1}));
  EXPECT_TRUE(IsBusted({-9000, 25000, 25000, 25000}));
  EXPECT_TRUE(IsBusted({0, 0, -100, 0}));
}

// 场终剩余供托归一位（v1.20，雀魂口径）：无棒可归 / 归最高持点 / 同点取起家 / 负点也算数。
TEST(RiichiKyoutakuAward, LeftoverSticksGoToFirstPlace) {
  // 没有棒：不归任何人
  const KyoutakuAward none = AwardLeftoverKyoutaku({25000, 25000, 25000, 25000}, 0);
  EXPECT_EQ(none.seat, -1);
  EXPECT_EQ(none.sticks, 0);
  EXPECT_EQ(none.points, 0);
  EXPECT_EQ(AwardLeftoverKyoutaku({25000, 25000, 25000, 25000}, -3).seat, -1) << "负棒数按无棒处理";

  // 正常：最高持点收棒，点数 = 根数 × 1000
  const KyoutakuAward award = AwardLeftoverKyoutaku({25000, 31000, 20000, 24000}, 3);
  EXPECT_EQ(award.seat, 1);
  EXPECT_EQ(award.sticks, 3);
  EXPECT_EQ(award.points, 3000);

  // 同点：取席位号小的（起家优先），与 match_end.rankings 的「同分 seat 升序」一致
  EXPECT_EQ(AwardLeftoverKyoutaku({30000, 30000, 25000, 15000}, 1).seat, 0);
  EXPECT_EQ(AwardLeftoverKyoutaku({20000, 25000, 25000, 30000}, 1).seat, 3);

  // 有人被打穿（负点）时，棒归活着的最高位；被击飞的那家不参与
  const KyoutakuAward busted = AwardLeftoverKyoutaku({-9000, 40000, 35000, 34000}, 1);
  EXPECT_EQ(busted.seat, 1);
  EXPECT_EQ(busted.points, 1000);
}

}  // namespace
}  // namespace mahjong::riichi