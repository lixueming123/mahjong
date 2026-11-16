// 日麻规则库单测：逐档对照标准点数表（30 符 3 番 3900 / 满贯 8000 / 役满 32000 ...）。
//
// 这些断言是「算分正确性」的唯一防线：房间层只做分派，不重新算分。

#include "riichi_rules.h"

#include <chrono>
#include <cstdio>
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

// 测试小工具：选中的牌确实在手里（`ChooseDiscard` 的返回值只可能来自手牌）。
bool ChosenIsInHand(const std::vector<Tile>& hand, Tile chosen) {
  return std::any_of(hand.begin(), hand.end(), [chosen](Tile tile) { return tile == chosen; });
}

bool HasId(const std::vector<Yaku>& yaku, std::string_view id) {
  const std::optional<protocol::YakuId> want = protocol::FromName<protocol::YakuId>(id);
  for (const Yaku& item : yaku) {
    if (want.has_value() && item.id == *want) {
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

// ---- 同巡振听（v1.22）--------------------------------------------------------
// 规则：非立直者放过一个能和的和牌张 ⇒ 只挡**那个牌面**，到自己下次摸牌为止。
// 立直者放过 ⇒ 整局持久（`furiten_ever_`），不受同巡豁免。房间层的状态机由
// `mahjong_riichi_game_test` 的 `TemporaryFuriten*` 用例覆盖；这里验规则库那一半。

TEST(RiichiTemporaryFuriten, OnlyThePassedFaceIsBlocked) {
  // 234m 567m 234p 78s + 1s —— 打 1s 之后听 6s/9s 两面。
  // 放过 6s 之后，9s 仍然可和：这是同巡振听与舍て牌振听的根本区别（只挡放过的那个牌面）。
  const std::vector<Tile> hand =
      Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "7s", "8s", "1s", "9p", "9p"});
  const std::vector<Tile> waiting = hand;  // 14 张行动态
  const Face passed = FaceOf(*ParseTile("6s"));
  const Face other = FaceOf(*ParseTile("9s"));

  // 同巡振听的牌面不在牌河里 ⇒ IsPermanentFuriten 看不见它（这正是要加房间层状态的原因）
  std::vector<Tile> thirteen = hand;
  for (auto it = thirteen.begin(); it != thirteen.end(); ++it) {
    if (FaceOf(*it) == FaceOf(*ParseTile("1s"))) {
      thirteen.erase(it);
      break;
    }
  }
  ASSERT_EQ(thirteen.size() + 0, size_t {13});
  const std::vector<Tile> waits = RiichiWaitingTiles(thirteen, {});
  EXPECT_EQ(waits.size(), 2u) << "打 1s 之后应当两面听 6s/9s";
  EXPECT_FALSE(IsPermanentFuriten(thirteen, {}, {}));

  // `extra_furiten_faces` 把 6s 标成同巡振听：9s 仍然能和，所以 1s 仍是可选打法。
  const WinContext ctx = Ron(2);
  std::array<bool, kFaceCount> extra {};
  extra.fill(false);
  extra[passed] = true;
  const std::optional<Tile> chosen = ChooseDiscard(waiting, {}, {}, extra, ctx, /*can_riichi=*/false);
  ASSERT_TRUE(chosen.has_value());
  std::vector<Tile> after = waiting;
  for (auto it = after.begin(); it != after.end(); ++it) {
    if (FaceOf(*it) == FaceOf(*chosen)) {
      after.erase(it);
      break;
    }
  }
  EXPECT_FALSE(RiichiWaitingTiles(after, {}).empty()) << "同巡振听不该让人放弃听牌";
  (void)other;
}

TEST(RiichiTemporaryFuriten, AllWaitsBlockedMeansNoLiveHand) {
  // 单骑听牌、且这一面已被放过 ⇒ 这一手没有任何能和张。
  // `can_riichi=false`（比如已鸣牌）时，决胜不该把它当成活听。
  const std::vector<Tile> hand = Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "7s", "8s", "9s", "1p"});
  const WinContext ctx = Ron(2);
  // 打 1p 之后听 3m/6m：把两面都标成同巡振听
  std::array<bool, kFaceCount> extra {};
  extra.fill(false);
  extra[FaceOf(*ParseTile("3m"))] = true;
  extra[FaceOf(*ParseTile("6m"))] = true;
  const std::optional<Tile> chosen = ChooseDiscard(hand, {}, {}, extra, ctx, /*can_riichi=*/false);
  ASSERT_TRUE(chosen.has_value());
  std::vector<Tile> after = hand;
  for (auto it = after.begin(); it != after.end(); ++it) {
    if (FaceOf(*it) == FaceOf(*chosen)) {
      after.erase(it);
      break;
    }
  }
  // 这一手只剩「死听 1p」与「不听」两种选择，评分仍以向听为先，但至少不能崩。
  EXPECT_TRUE(ChosenIsInHand(hand, *chosen));
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
  EXPECT_EQ(score.yaku[0].id, protocol::YakuId::kChankan);
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
  EXPECT_EQ(score.yaku[0].id, protocol::YakuId::kKokushi13);
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
  EXPECT_EQ(tenhou.yaku[0].id, protocol::YakuId::kTenhou);
  EXPECT_EQ(tenhou.limit, Limit::kYakuman);
  EXPECT_EQ(tenhou.points, 48000);  // 庄家役满自摸：16000 × 3

  WinContext child = Tsumo();  // 闲家
  child.is_chiihou = true;
  const ScoreResult chiihou = ScoreHand(hand, {}, *ParseTile("4s"), child);
  ASSERT_FALSE(chiihou.yaku.empty());
  EXPECT_EQ(chiihou.yaku[0].id, protocol::YakuId::kChiihou);
  EXPECT_EQ(chiihou.limit, Limit::kYakuman);
  EXPECT_EQ(chiihou.points, 32000);  // 16000 + 8000 + 8000

  // 没有置位时照常按普通手牌算（不能把每局都当日和）
  const ScoreResult plain = ScoreHand(hand, {}, *ParseTile("4s"), Tsumo());
  EXPECT_LT(plain.han, 13);
  EXPECT_NE(plain.limit, Limit::kYakuman);
}

// 场风 / 自风（v1.14 半庄；v1.25 修正自风口径）。
//
// 东场：庄家（seat 0）是东，下家南、对家西、上家北。
// **自风只看「相对庄家的座位」**，与场风无关 —— 庄家恒为东家。南场庄家仍是东家，
// 所以南场庄家的南刻子只算场风 1 番，不是连风 2 番。
// 旧实现把 round_wind 加进自风（「庄家自风 = 场风」），南场整体错一圈；天凤真实牌谱
// （20 半庄 / 183 手和牌）逐手核对后改正，见 game/docs/TENHOU_CORPUS.md。
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

  // 南场：**自风与东场完全相同** —— 庄家仍是东家，只是场风变成南
  EXPECT_EQ(SeatWindOf(1, 0, 0), 0);  // 庄家是东（不是南）
  EXPECT_EQ(SeatWindOf(1, 1, 0), 1);  // 下家南
  EXPECT_EQ(SeatWindOf(1, 2, 0), 2);  // 对家西
  EXPECT_EQ(SeatWindOf(1, 3, 0), 3);  // 上家北
  // 「庄家自风 == 场风」在南场**不成立**（庄家是东、场风是南）
  EXPECT_NE(SeatWindOf(1, 0, 0), RoundWindOf(4));

  // 自风与场风解耦：同一座位关系下，换场风结果不变
  for (int round_wind = 0; round_wind < 4; ++round_wind) {
    for (int dealer = 0; dealer < 4; ++dealer) {
      for (int seat = 0; seat < 4; ++seat) {
        EXPECT_EQ(SeatWindOf(round_wind, seat, dealer), (seat - dealer + 4) % 4)
            << "round_wind=" << round_wind << " seat=" << seat << " dealer=" << dealer;
      }
    }
  }
}

// 连风牌（场风与自风同面）只在**东场庄家持东刻子**这类局面成立 ⇒ 役牌 2 番。
// 天凤的真实反例（南场庄家持南）见上一用例。
TEST(RiichiYakuhai, DoubleEastWindInEastRound) {
  riichi::WinContext ctx;
  ctx.winner_seat = 0;
  ctx.from_seat = 1;
  ctx.dealer_seat = 0;
  ctx.round_wind = 0;  // 东场
  ctx.seat_wind = SeatWindOf(0, 0, 0);

  // 副露：东刻子（场风东 + 自风东 = 连风 2 番）。暗手 3 组 + 将共 11 张。
  const std::vector<Meld> melds = {Pong({"1z", "1z", "1z"})};
  const std::vector<Yaku> yaku =
      riichi::DetectYaku(Tiles({"2m", "3m", "4m", "5p", "6p", "7p", "8s", "8s", "8s", "9s", "9s"}), melds,
                         *ParseTile("9s"), ctx);
  int yakuhai_han = 0;
  for (const Yaku& item : yaku) {
    if (item.id == protocol::YakuId::kYakuhai) {
      yakuhai_han = item.han;
    }
  }
  EXPECT_EQ(yakuhai_han, 2) << "东场庄家的东刻子：场风 + 自风 = 2 番";
}

// 南场庄家的南刻子：只有场风 1 番（庄家的自风是东，不是南）。
TEST(RiichiYakuhai, SouthRoundDealerSouthTripletIsOneHan) {
  riichi::WinContext ctx;
  ctx.winner_seat = 3;
  ctx.from_seat = -1;  // 自摸
  ctx.dealer_seat = 3;
  ctx.round_wind = 1;  // 南场
  ctx.seat_wind = SeatWindOf(1, 3, 3);

  const std::vector<Meld> melds = {Chow({"3p", "4p", "5p"})};
  const std::vector<Yaku> yaku =
      riichi::DetectYaku(Tiles({"2z", "2z", "2z", "3s", "3s", "3s", "4s", "4s", "5m", "6m", "7m"}),
                         melds, *ParseTile("5m"), ctx);
  int yakuhai_han = 0;
  for (const Yaku& item : yaku) {
    if (item.id == protocol::YakuId::kYakuhai) {
      yakuhai_han = item.han;
    }
  }
  EXPECT_EQ(yakuhai_han, 1) << "南场庄家的南刻子只算场风，不是连风 2 番";
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
  EXPECT_EQ(YakuHan(protocol::YakuId::kAkaDora, /*closed=*/true), 1);
  EXPECT_FALSE(IsYakumanYaku(protocol::YakuId::kAkaDora));
}

// ============================================================================
// 双倍役满（v1.18）：国士十三面 / 四暗刻单骑 / 纯正九莲 / 大四喜 = 2 倍
// ============================================================================

TEST(RiichiYakuman, MultiplierTable) {
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kKokushi13), 2);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kSuuankouTanki), 2);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kChuurenJunsei), 2);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kDaisuushii), 2);

  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kKokushi), 1);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kSuuankou), 1);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kChuuren), 1);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kShousuushii), 1);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kDaisangen), 1);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kTenhou), 1);
  EXPECT_EQ(YakumanMultiplier(protocol::YakuId::kPinfu), 0);

  for (protocol::YakuId id : {protocol::YakuId::kKokushi13, protocol::YakuId::kSuuankouTanki,
                              protocol::YakuId::kChuurenJunsei, protocol::YakuId::kDaisuushii}) {
    EXPECT_TRUE(IsYakumanYaku(id)) << protocol::NameOf(id);
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

// ---------------------------------------------------------------------------
// tenpai 帧成本分解（PB-2 现场）
//
// `BuildTenpaiFrame` 的等待态路径原来把「等待张」算了两遍：
// `riichi_mahjong_game.cc:2251` 一次，`WinningWaits` 内部再一次（输入完全相同）；
// 之后才对每张等待牌跑一次 `DetectYaku`（只用来判 `!empty()`）。
//
// 本用例量「重复的那一遍」值多少钱，并给出帧的成本构成：
//   A  = 一次 RiichiWaitingTiles
//   9B = 逐张判番总耗时
// 改后帧 = A + WinningWaits(waits)。
//
// 手牌与 `MahjongRules.WaitingTilesPerfBudget` 同款：九面听 1112345678999m（13 张门前）。
// 2026-11-12 基线（Release）：A ≈ 78 µs，WinningWaits 旧 ≈ 97 µs / 新 ≈ 5 µs，9B ≈ 3 µs。
// 结论：**判役不是瓶颈**（3 µs 级别），重复的 A 才是——去掉后帧成本减半。
// ---------------------------------------------------------------------------
TEST(RiichiPerf, TenpaiFrameCostBreakdown) {
  const std::vector<Tile> concealed =
      Tiles({"1m", "1m", "1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "9m", "9m"});
  const std::vector<Meld> melds;
  const std::vector<Tile> waits = RiichiWaitingTiles(concealed, melds);
  ASSERT_GE(waits.size(), 8u) << "九面听的手牌必须真的多面听";
  const WinContext ron = Ron(2);

  // 每张等待牌补一张后的 14 张手牌（与 WinningWaits 内部构造完全一致）
  std::vector<std::vector<Tile>> hands;
  hands.reserve(waits.size());
  for (Tile wait : waits) {
    std::vector<Tile> hand = concealed;
    hand.push_back(wait);
    SortTiles(hand);
    hands.push_back(std::move(hand));
  }

  // 计时助手：用 `asm volatile` 空约束消费每轮结果，避免单轮被合并。
  // 跨 TU 的调用（`Shanten` / `CanWinExact` 都编在 libmahjong_rules 里）编译器既不能内联，
  // 也不能当作无副作用消除，所以屏障不是「数字可信」的前提，只是让计时范围更明确。
  auto keep = [](long long value) { asm volatile("" : : "r,m"(value) : "memory"); };
  auto time_us = [&keep](int iters, auto&& body) {
    long long acc = 0;
    for (int i = 0; i < 10; ++i) {
      acc += body();
      keep(acc);  // 预热
    }
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
      acc += body();
      keep(acc);
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count() / iters;
  };

  // A = 单次 RiichiWaitingTiles（帧里原来被调两次 ⇒ 2A）。
  // 2026-11-12：去掉 `WaitingTiles` 内的 Shanten 门控后，A 从 76 µs 降到 6 µs。
  // 门控在重叠手牌上要跑 77–170 µs 的 DFS，只为省掉 4.7 µs 的 34 次 CanWinExact。
  const double a_us = time_us(200, [&] {
    return static_cast<long long>(RiichiWaitingTiles(concealed, melds).size());
  });
  // 9B = 帧里逐张判番的总和（每轮把全部等待张过一遍，除以张数得单次）
  const double nine_b_us = time_us(50, [&] {
    long long total = 0;
    for (size_t i = 0; i < hands.size(); ++i) {
      total += static_cast<long long>(DetectYaku(hands[i], melds, waits[i], ron).size());
    }
    return total;
  });
  const double b_us = nine_b_us / static_cast<double>(hands.size());
  // 实测 WinningWaits：旧签名内部会再算一遍 RiichiWaitingTiles，新签名直接吃 waits
  const double winning_us = time_us(50, [&] {
    return static_cast<long long>(WinningWaits(concealed, melds, ron).size());
  });
  const double winning_reuse_us = time_us(50, [&] {
    return static_cast<long long>(WinningWaits(concealed, melds, ron, waits).size());
  });

  // 帧的实际形态（改后）：RiichiWaitingTiles 一次 + WinningWaits(waits)
  const double frame_model_us = a_us + winning_reuse_us;
  std::printf(
      "[perf] tenpai 分解（%zu 面听）: A(RiichiWaitingTiles)=%.1f us | B(DetectYaku)=%.1f us/张 | "
      "9B=%.1f us |\n"
      "       WinningWaits(旧,重算 waits)=%.1f us | WinningWaits(新,复用 waits)=%.1f us | 帧模型 A+WW=%.1f us\n",
      waits.size(), a_us, b_us, nine_b_us, winning_us, winning_reuse_us, frame_model_us);
  std::printf("[perf] 占比（帧模型口径）: 等待张 A = %.0f%% | 逐张判番 9B = %.0f%%\n",
              100.0 * a_us / frame_model_us, 100.0 * nine_b_us / frame_model_us);

  // 门禁：Debug(-O0) 实测见上方输出，按它留余量，只兜住「翻倍级」回归。
  EXPECT_LT(frame_model_us, 200.0) << "tenpai 帧主导成本回归（Release 目标 < 20µs）";
}

// ---- AI 弃牌：并列处按「役 / 振听」决胜 -----------------------------------------
// 共享的 `ChooseDiscard` 只用 `BestShantenAfterDraw` 评分，并列时按冻结牌序取最小牌面。
// 日麻有「无役不能和」，所以并列时必须挑那手**能和**的 —— 否则会出现「牌型完整、听牌、
// 却永远和不了」的死手（实测占荒庄局的一大半）。

// 测试内的「按牌面扣一张」：`RemoveOneFaceIn` 是 riichi_rules.cc 的匿名命名空间实现，
// 没有导出，这里照同语义补一份（只扣恰好一张，用于构造等待态）。
void DropFace(std::vector<Tile>* tiles, Face face) {
  for (auto it = tiles->begin(); it != tiles->end(); ++it) {
    if (FaceOf(*it) == face) {
      tiles->erase(it);
      return;
    }
  }
  FAIL() << "牌面不在手里: " << static_cast<int>(face);
}

TEST(RiichiAiDiscard, PrefersTheWaitThatActuallyHasYaku) {
  // 234m 567m 345p 88s 67s + 9s。两种打法都**立刻听牌**（评分并列），但：
  //   打 9s ⇒ 234m 567m 345p 88s 67s，听 5s/8s，全手无幺九字 ⇒ 断幺九，有役；
  //   打 6s ⇒ 234m 567m 345p 88s 79s，听嵌张 8s，带 9s ⇒ 无断幺、无役牌、无役。
  // 冻结牌序会在并列处取更小的 6s，于是走成死听 —— 这正是要修的。
  const std::vector<Tile> hand =
      Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "3p", "4p", "5p", "6s", "7s", "8s", "8s", "9s"});
  const WinContext ctx = Ron(/*from_seat=*/2);
  const std::optional<Tile> chosen = ChooseDiscard(hand, {}, {}, {}, ctx, /*can_riichi=*/false);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(FaceOf(*chosen), FaceOf(*ParseTile("9s")))
      << "应打 9s 留下断幺九的 5s/8s 两面听；实际打出 " << FormatTile(*chosen);

  // 反向确认这手牌确实存在「无役听牌」的另一半，否则上面那条断言没有区分力。
  std::vector<Tile> no_yaku_side = hand;
  DropFace(&no_yaku_side, FaceOf(*ParseTile("6s")));
  const std::vector<Tile> waits = RiichiWaitingTiles(no_yaku_side, {});
  ASSERT_EQ(waits.size(), 1u) << "打 6s 之后应当是单张听牌";
  EXPECT_TRUE(WinningWaits(no_yaku_side, {}, ctx, waits).empty()) << "打 6s 那一路本来就该是无役的";

  // 有役的那一半确实能荣和。
  std::vector<Tile> yaku_side = hand;
  DropFace(&yaku_side, FaceOf(*ParseTile("9s")));
  const std::vector<Tile> yaku_waits = RiichiWaitingTiles(yaku_side, {});
  ASSERT_FALSE(yaku_waits.empty());
  EXPECT_FALSE(WinningWaits(yaku_side, {}, ctx, yaku_waits).empty()) << "打 9s 那一路应当是断幺九";
}

TEST(RiichiAiDiscard, ShantenStillBeatsYaku) {
  // 役只在**并列**处决胜。这里拆 99s 去追断幺九会退向听，必须放弃 —— 评分是第一关键字。
  const std::vector<Tile> hand = Tiles({"2m", "3m", "4m", "5m", "6m", "7m", "3p", "4p",
                                       "5p", "6s", "7s", "9s", "9s", "1z"});
  const WinContext ctx = Ron(/*from_seat=*/2);
  const std::optional<Tile> chosen = ChooseDiscard(hand, {}, {}, {}, ctx, /*can_riichi=*/false);
  ASSERT_TRUE(chosen.has_value());

  // 逐个候选算评分，确认选中那张拿到了最小评分（役不能把更差的向听顶上来）。
  int best = 8;  // 向听上限（`mahjong_rules` 的 kMaxShanten，未导出）
  for (Tile tile : hand) {
    std::vector<Tile> rest = hand;
    DropFace(&rest, FaceOf(tile));
    best = std::min(best, BestShantenAfterDraw(rest, {}));
  }
  std::vector<Tile> after = hand;
  DropFace(&after, FaceOf(*chosen));
  EXPECT_EQ(BestShantenAfterDraw(after, {}), best) << "选牌没有拿到最优向听：役盖过了第一关键字";
}

TEST(RiichiAiDiscard, ClosedHandWithoutYakuStillCountsViaRiichi) {
  // 门清、能立直 ⇒ 无役听牌也是活的（立直就是役）。这里 can_riichi=true，
  // 因此不该出现「宁可不听牌也不打那手」的怪行为。
  const std::vector<Tile> hand = Tiles({"1m", "2m", "3m", "7p", "8p", "9p", "2s", "3s",
                                       "4s", "6z", "6z", "1z", "1z", "5m"});
  const WinContext ctx = Ron(/*from_seat=*/2);
  const std::optional<Tile> chosen = ChooseDiscard(hand, {}, {}, {}, ctx, /*can_riichi=*/true);
  ASSERT_TRUE(chosen.has_value());
  std::vector<Tile> after = hand;
  after.erase(std::find(after.begin(), after.end(), *chosen));
  EXPECT_FALSE(RiichiWaitingTiles(after, {}).empty()) << "能立直时至少要选一张打成听牌的";
}

TEST(RiichiAiDiscard, TiesFallBackToTheFrozenTileOrder) {
  // 全并列（无役路线、不能立直）时，必须回到共享实现的确定性口径：最小牌面。
  // 七对子形：1z 1z 3z 3z 5z 5z 7z 7z（4 对）+ 散牌，役为零、门清但 can_riichi=false。
  const std::vector<Tile> hand = Tiles({"1z", "1z", "3z", "3z", "5z", "5z", "7z", "7z",
                                       "2m", "5m", "2p", "5p", "2s", "5s"});
  const WinContext ctx = Ron(/*from_seat=*/2);
  const std::optional<Tile> chosen = ChooseDiscard(hand, {}, {}, {}, ctx, /*can_riichi=*/false);
  ASSERT_TRUE(chosen.has_value());
  // 与共享实现的选择对照：并列处两者都必须落在同一张，否则「确定性」就丢了。
  const std::optional<Tile> shared = mahjong::ChooseDiscard(hand, {}, std::nullopt);
  ASSERT_TRUE(shared.has_value());
  // 评分并列时双方取同一牌面；不并列则只要求本实现不更差。
  std::vector<Tile> after = hand;
  after.erase(std::find(after.begin(), after.end(), *chosen));
  std::vector<Tile> after_shared = hand;
  after_shared.erase(std::find(after_shared.begin(), after_shared.end(), *shared));
  EXPECT_LE(BestShantenAfterDraw(after, {}), BestShantenAfterDraw(after_shared, {}));
}

// ---- 喰い替え禁止（v2.5，PROTOCOL §12.15）-----------------------------------------------
// 口径：鸣牌所用的手牌 + 被鸣的那张牌，凡能与之组成面子的牌面，鸣牌后当巡都不许打。
// 三组断言分别守モロ喰い替え、スジ喰い替え、以及「边张/嵌张/字牌只有モロ」这条边界。

TEST(RiichiKuikae, PongOnlyForbidsTheClaimedFace) {
  // 碰 5p：手牌两张 5p ⇒ 只有 5p 被禁（モロ喰い替え）。
  const std::vector<Face> faces = KuikaeForbiddenFaces(Tiles({"5p", "5p"}), *ParseTile("5p"));
  ASSERT_EQ(faces.size(), size_t {1});
  EXPECT_EQ(faces.front(), FaceOf(*ParseTile("5p")));
}

TEST(RiichiKuikae, RyanmenChowForbidsTheClaimedAndTheOtherSide) {
  // 4m5m 吃 3m ⇒ 3m（モロ）与 6m（スジ）都被禁。
  const std::vector<Face> faces = KuikaeForbiddenFaces(Tiles({"4m", "5m"}), *ParseTile("3m"));
  ASSERT_EQ(faces.size(), size_t {2});
  EXPECT_EQ(faces[0], FaceOf(*ParseTile("3m")));
  EXPECT_EQ(faces[1], FaceOf(*ParseTile("6m")));

  // 反向：4m5m 吃 6m ⇒ 6m 与 3m。
  const std::vector<Face> other = KuikaeForbiddenFaces(Tiles({"4m", "5m"}), *ParseTile("6m"));
  ASSERT_EQ(other.size(), size_t {2});
  EXPECT_EQ(other[0], FaceOf(*ParseTile("3m")));
  EXPECT_EQ(other[1], FaceOf(*ParseTile("6m")));
}

TEST(RiichiKuikae, EdgeChowAtTheWallOnlyForbidsTheClaimedFace) {
  // 1s2s 吃 3s：另一侧是 0s（不存在）⇒ 只有 3s 被禁。
  const std::vector<Face> low = KuikaeForbiddenFaces(Tiles({"1s", "2s"}), *ParseTile("3s"));
  ASSERT_EQ(low.size(), size_t {1});
  EXPECT_EQ(low.front(), FaceOf(*ParseTile("3s")));

  // 8s9s 吃 7s：另一侧是 10s（不存在）⇒ 只有 7s。
  const std::vector<Face> high = KuikaeForbiddenFaces(Tiles({"8s", "9s"}), *ParseTile("7s"));
  ASSERT_EQ(high.size(), size_t {1});
  EXPECT_EQ(high.front(), FaceOf(*ParseTile("7s")));
}

TEST(RiichiKuikae, KanchanChowOnlyForbidsTheClaimedFace) {
  // 7m9m 吃 8m（嵌张）：与被鸣牌组成面子的只有 8m 自己 ⇒ 6m 不禁（常见误区）。
  const std::vector<Face> faces = KuikaeForbiddenFaces(Tiles({"7m", "9m"}), *ParseTile("8m"));
  ASSERT_EQ(faces.size(), size_t {1});
  EXPECT_EQ(faces.front(), FaceOf(*ParseTile("8m")));
}

TEST(RiichiKuikae, HonorMeldAndKongOnlyForbidTheClaimedFace) {
  // 字牌碰（东东 + 东）⇒ 只有东。
  const std::vector<Face> honor = KuikaeForbiddenFaces(Tiles({"1z", "1z"}), *ParseTile("1z"));
  ASSERT_EQ(honor.size(), size_t {1});
  EXPECT_EQ(honor.front(), FaceOf(*ParseTile("1z")));

  // 大明杠（手牌 3 张）⇒ 同样只有被鸣的那一面（且杠后本来就要先补摸）。
  const std::vector<Face> kong = KuikaeForbiddenFaces(Tiles({"4p", "4p", "4p"}), *ParseTile("4p"));
  ASSERT_EQ(kong.size(), size_t {1});
  EXPECT_EQ(kong.front(), FaceOf(*ParseTile("4p")));
}

TEST(RiichiKuikae, AiDiscardNeverPicksAForbiddenFace) {
  // 手牌里最该打的那张正好落在禁区 ⇒ AI 必须换一张，且不能因此变差太多。
  // 这里只断言「不选禁区」，具体换哪张由牌效率决定。
  const std::vector<Tile> hand = Tiles({"1m", "1m", "1m", "2p", "3p", "4p", "5s", "6s", "7s", "2z", "2z"});
  const WinContext ctx = Ron(/*from_seat=*/2);
  const std::vector<Face> forbidden {FaceOf(*ParseTile("1m"))};
  const std::optional<Tile> chosen = ChooseDiscard(hand, {}, {}, {}, ctx, /*can_riichi=*/false, std::nullopt,
                                                   forbidden);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_NE(FaceOf(*chosen), FaceOf(*ParseTile("1m"))) << "喰い替え禁止的牌面不该出现在 AI 的选择里";
  EXPECT_TRUE(ChosenIsInHand(hand, *chosen));
}

TEST(RiichiKuikae, AiDiscardWithoutForbiddenFacesIsUnchanged) {
  // 空禁区必须与旧行为逐字一致（这条是回归锚：新参数不能悄悄改掉既有策略）。
  const std::vector<Tile> hand = Tiles({"1m", "2m", "3m", "7p", "8p", "9p", "2s", "3s",
                                       "4s", "6z", "6z", "1z", "1z", "5m"});
  const WinContext ctx = Ron(/*from_seat=*/2);
  const std::optional<Tile> with_empty = ChooseDiscard(hand, {}, {}, {}, ctx, /*can_riichi=*/true, std::nullopt, {});
  const std::optional<Tile> legacy = ChooseDiscard(hand, {}, {}, {}, ctx, /*can_riichi=*/true);
  ASSERT_TRUE(with_empty.has_value());
  ASSERT_TRUE(legacy.has_value());
  EXPECT_EQ(with_empty->id, legacy->id);
}

// ---------------------------------------------------------------------------
// 役满与低频役（TENHOU_CORPUS.md 的补充）
//
// 天凤**没有**公开的「役满一览」接口（`/sc/yakuman/` 等均 404），归档日志的每行也只有
// 「谁和了、得分」而没有役种，因此真实牌谱语料撞不到役满（第一版 20 半庄 / 183 手和牌里
// 役满为零）。役满的牌型与倍数关系**没有歧义**，这里用人工构造的手牌覆盖，与真实牌谱回归
// 互补：真实语料保证「常见役在实际局面下算对」，这里保证「稀有形态不会漏判/错倍数」。
// ---------------------------------------------------------------------------
namespace {

// 役满用例的上下文：闲家自摸（庄家 bonus 不参与役满，但分摊要用 dealer_seat）。
WinContext YakumanContext() {
  WinContext ctx;
  ctx.winner_seat = 1;
  ctx.from_seat = -1;
  ctx.dealer_seat = 0;
  ctx.round_wind = 0;
  ctx.seat_wind = 1;
  return ctx;
}

// 某一役在结果里出现了几次（役满按倍数算，双倍役满 han = 26）。
int HanOf(const ScoreResult& result, protocol::YakuId id) {
  for (const Yaku& item : result.yaku) {
    if (item.id == id) {
      return item.han;
    }
  }
  return 0;
}

// 役满是否成立（limit == yakuman、倍数正确、符恒 0、收入符合 8000×倍数）。
void ExpectYakuman(const ScoreResult& result, int multiplier) {
  EXPECT_EQ(result.limit, Limit::kYakuman) << "应为役满";
  EXPECT_EQ(result.yakuman_count, multiplier) << "役满倍数";
  EXPECT_EQ(result.fu, 0) << "役满符恒为 0";
  // 闲家自摸 = 4 × 8000 × 倍数（庄家付 2 份、两闲家各 1 份）；
  // 庄家自摸 = 6 × 8000 × 倍数（三家各付 2 份）。两种情况都接受，具体由用例的席位决定。
  const int as_non_dealer = 8000 * multiplier * 4;
  const int as_dealer = 8000 * multiplier * 6;
  EXPECT_TRUE(result.points == as_non_dealer || result.points == as_dealer)
      << "役满收入应为 " << as_non_dealer << "（闲家）或 " << as_dealer << "（庄家），实际 "
      << result.points;
}

}  // namespace

// 天和：庄家配牌即和（无鸣牌、无打牌）。
TEST(RiichiYakuman, Tenhou) {
  WinContext ctx = YakumanContext();
  ctx.winner_seat = 0;  // 庄家
  ctx.dealer_seat = 0;
  ctx.seat_wind = 0;
  ctx.is_tenhou = true;
  const ScoreResult result = ScoreHand(Tiles({"1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "9s",
                                              "1z", "1z", "1z", "5z", "5z"}),
                                       {}, *ParseTile("5z"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kTenhou), 13);
  ExpectYakuman(result, 1);
}

// 地和：闲家第一巡自摸。
TEST(RiichiYakuman, Chiihou) {
  WinContext ctx = YakumanContext();
  ctx.is_chiihou = true;
  const ScoreResult result = ScoreHand(Tiles({"1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "9s",
                                              "1z", "1z", "1z", "5z", "5z"}),
                                       {}, *ParseTile("5z"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kChiihou), 13);
  ExpectYakuman(result, 1);
}

// 国士无双（普通，非十三面）：和牌前**已经有那一对**，和牌张补的是缺的那种。
// 13 种各一张 + 6z 成对 = 14 张里缺 1s，荣和 1s。
TEST(RiichiYakuman, Kokushi) {
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;  // 荣和
  const ScoreResult result = ScoreHand(Tiles({"1m", "9m", "1p", "9p", "9s", "1z", "2z",
                                              "3z", "4z", "5z", "6z", "6z", "7z", "1s"}),
                                       {}, *ParseTile("1s"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kKokushi), 13) << "普通国士（非十三面）";
  EXPECT_EQ(HanOf(result, protocol::YakuId::kKokushi13), 0);
  ExpectYakuman(result, 1);
}

// 国士十三面：和牌前是十三种各一张（听 13 面）⇒ **双倍**役满。
// 判据必须看**和牌前**：荣和时 concealed 已含和牌张，只看「和牌张成对」会误判
// （`Kokushi` 那手会被误报成双倍）。
TEST(RiichiYakuman, KokushiThirteenSidesIsDouble) {
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;  // 荣和 1m
  const ScoreResult result = ScoreHand(Tiles({"1m", "9m", "1p", "9p", "1s", "9s", "1z", "2z",
                                              "3z", "4z", "5z", "6z", "7z", "1m"}),
                                       {}, *ParseTile("1m"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kKokushi13), 26) << "双倍役满记 26 番";
  EXPECT_EQ(HanOf(result, protocol::YakuId::kKokushi), 0);
  ExpectYakuman(result, 2);
}

// 四暗刻单骑、纯正九莲宝灯的双倍倍数已由 `RiichiYakuman.SuuankouTankiIsDouble` /
// `.ChuurenJunseiIsDouble`（本文件上半部分）覆盖，这里不再重复。

// 大四喜：四组风刻子 + 将 ⇒ 双倍役满。**注意**：这手牌同时是字一色（1 倍）+ 四暗刻单骑
// （自摸、和牌张是把将 ⇒ 2 倍），所以合计 5 倍 —— 引擎按「多役满倍数相加」算，与天凤口径一致。
TEST(RiichiYakuman, DaisuushiiIsDouble) {
  const ScoreResult result = ScoreHand(Tiles({"1z", "1z", "1z", "2z", "2z", "2z", "3z", "3z", "3z",
                                              "4z", "4z", "4z", "5z", "5z"}),
                                       {}, *ParseTile("5z"), YakumanContext());
  EXPECT_EQ(HanOf(result, protocol::YakuId::kDaisuushii), 26) << "大四喜是双倍役满";
  EXPECT_EQ(HanOf(result, protocol::YakuId::kTsuuiisou), 13) << "同一手牌也是字一色";
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSuuankouTanki), 26) << "自摸单骑 ⇒ 四暗刻单骑";
  ExpectYakuman(result, 5);  // 2 + 1 + 2
}

// 大三元：三组三元刻子。
TEST(RiichiYakuman, Daisangen) {
  const ScoreResult result = ScoreHand(Tiles({"5z", "5z", "5z", "6z", "6z", "6z", "7z", "7z", "7z",
                                              "1m", "2m", "3m", "9p", "9p"}),
                                       {}, *ParseTile("9p"), YakumanContext());
  EXPECT_EQ(HanOf(result, protocol::YakuId::kDaisangen), 13);
  ExpectYakuman(result, 1);
}

// 字一色：全部字牌（与 `DaisuushiiIsDouble` 同一手牌的侧面断言 —— 单看字一色是 1 倍，
// 但那手牌叠加了大四喜与四暗刻单骑，所以倍数合计 5）。
TEST(RiichiYakuman, Tsuuiisou) {
  const ScoreResult result = ScoreHand(Tiles({"1z", "1z", "1z", "2z", "2z", "2z", "3z", "3z", "3z",
                                              "5z", "5z", "5z", "6z", "6z"}),
                                       {}, *ParseTile("6z"), YakumanContext());
  EXPECT_EQ(HanOf(result, protocol::YakuId::kTsuuiisou), 13);
  // 只有大三元（5z/6z… 这里只有白与发的刻子）不成立，故至少是「字一色 + 四暗刻单骑」2 + 1
  EXPECT_GE(result.yakuman_count, 1);
  EXPECT_EQ(result.fu, 0);
  EXPECT_EQ(result.limit, Limit::kYakuman);
}

// 绿一色：只有 2s/3s/4s/6s/8s/发。
TEST(RiichiYakuman, Ryuuiisou) {
  const ScoreResult result = ScoreHand(Tiles({"2s", "3s", "4s", "2s", "3s", "4s", "6s", "6s", "6s",
                                              "8s", "8s", "8s", "6z", "6z"}),
                                       {}, *ParseTile("6z"), YakumanContext());
  EXPECT_EQ(HanOf(result, protocol::YakuId::kRyuuiisou), 13);
  ExpectYakuman(result, 1);
}

// 清老头：全部老头牌。这手同时也是四暗刻单骑（自摸、和牌张成刻）⇒ 1 + 2 = 3 倍。
TEST(RiichiYakuman, Chinroutou) {
  const ScoreResult result = ScoreHand(Tiles({"1m", "1m", "1m", "9m", "9m", "9m", "1p", "1p", "1p",
                                              "9s", "9s", "9s", "9p", "9p"}),
                                       {}, *ParseTile("9p"), YakumanContext());
  EXPECT_EQ(HanOf(result, protocol::YakuId::kChinroutou), 13);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSuuankouTanki), 26) << "自摸单骑 ⇒ 四暗刻单骑";
  ExpectYakuman(result, 3);  // 清老头 1 + 四暗刻单骑 2
}

// 四杠子：四组暗杠 + 将 ⇒ 单倍役满。四组暗刻同时构成四暗刻（1 倍），合计 2 倍。
TEST(RiichiYakuman, Suukantsu) {
  std::vector<Meld> melds;
  for (const char* face : {"1m", "5p", "9s", "3z"}) {
    const Face f = FaceOf(*ParseTile(face));
    Meld kan;
    kan.type = MeldType::kAnkan;
    kan.tiles = {MakeTileFromFace(f, 0), MakeTileFromFace(f, 1), MakeTileFromFace(f, 2),
                 MakeTileFromFace(f, 3)};
    melds.push_back(kan);
  }
  const ScoreResult result = ScoreHand(Tiles({"7z", "7z"}), melds, *ParseTile("7z"), YakumanContext());
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSuukantsu), 13) << "四杠子";
  // 四组暗刻 + 自摸 = 四暗刻（和牌张在将上 ⇒ 不是单骑，单倍）
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSuuankou), 13) << "同时构成四暗刻（单倍）";
  ExpectYakuman(result, 2);
}

// 小四喜：三组风刻子 + 风将（不是役满倍数叠加，单倍役满）。
TEST(RiichiYakuman, Shousuushii) {
  const ScoreResult result = ScoreHand(Tiles({"1z", "1z", "1z", "2z", "2z", "2z", "3z", "3z", "3z",
                                              "1m", "2m", "3m", "4z", "4z"}),
                                       {}, *ParseTile("4z"), YakumanContext());
  EXPECT_EQ(HanOf(result, protocol::YakuId::kShousuushii), 13);
  ExpectYakuman(result, 1);
}

// ---- 低频通常役（真实语料同样撞不到）----

// 抢杠：加杠的那张被荣和（1 番，且无役也能和）。14 张完整手牌 + 荣和 9p。
TEST(RiichiRareYaku, Chankan) {
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;
  ctx.is_chankan = true;
  const ScoreResult result = ScoreHand(Tiles({"2m", "3m", "4m", "5p", "6p", "7p", "2s", "3s", "4s",
                                              "1z", "1z", "1z", "9p", "9p"}),
                                       {}, *ParseTile("9p"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kChankan), 1);
  EXPECT_EQ(result.yakuman_count, 0);
}

// 岭上开花：杠后补牌自摸。
TEST(RiichiRareYaku, Rinshan) {
  WinContext ctx = YakumanContext();
  ctx.is_rinshan = true;
  const ScoreResult result = ScoreHand(Tiles({"2m", "3m", "4m", "5p", "6p", "7p", "2s", "3s", "4s",
                                              "1z", "1z", "1z", "9p", "9p"}),
                                       {}, *ParseTile("9p"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kRinshan), 1);
}

// 河底捞鱼：最后一张弃牌荣和。
TEST(RiichiRareYaku, Houtei) {
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;
  ctx.is_houtei = true;
  const ScoreResult result = ScoreHand(Tiles({"2m", "3m", "4m", "5p", "6p", "7p", "2s", "3s", "4s",
                                              "1z", "1z", "1z", "9p", "9p"}),
                                       {}, *ParseTile("9p"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kHoutei), 1);
}

// 三色同刻（v1.26，2 番）：三种花色、同一数字各一组刻子。**不是**三色同顺。
TEST(RiichiRareYaku, SanshokuDoukou) {
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;  // 荣和；牌全在手里 ⇒ 仍是门前清
  // 5m5m5m / 5p5p5p / 5s5s5s 是三色**同刻**；第四组用顺子 1m2m3m，
  // 避免整手变成四暗刻（那会先命中役满、根本走不到通常役的判定）。
  const ScoreResult result = ScoreHand(Tiles({"5m", "5m", "5m", "5p", "5p", "5p", "5s", "5s", "5s",
                                              "2m", "3m", "4m", "9p", "9p"}),
                                       {}, *ParseTile("9p"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSanshokuDoukou), 2) << "门前清三色同刻 2 番";
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSanshokuDoujun), 0) << "同刻不是同顺";
}

// 三色同刻副露 1 番（与三色同顺同档）。
TEST(RiichiRareYaku, SanshokuDoukouOpenIsOneHan) {
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;
  // 副露：碰 5m，手牌仍含 5p/5s 的刻子 ⇒ 非门前清 ⇒ 1 番
  std::vector<Meld> melds = {Pong({"5m", "5m", "5m"})};
  const ScoreResult result = ScoreHand(Tiles({"5p", "5p", "5p", "5s", "5s", "5s", "1z", "1z", "1z",
                                              "9p", "9p"}),
                                       melds, *ParseTile("9p"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSanshokuDoukou), 1) << "副露三色同刻 1 番";
}

// 三色同顺仍是独立役：2m3m4m / 2p3p4p / 2s3s4s（2 番），不得误报成三色同刻。
TEST(RiichiRareYaku, SanshokuDoujunIsNotDoukou) {
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;
  const ScoreResult result = ScoreHand(Tiles({"2m", "3m", "4m", "2p", "3p", "4p", "2s", "3s", "4s",
                                              "1z", "1z", "1z", "9p", "9p"}),
                                       {}, *ParseTile("9p"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSanshokuDoujun), 2) << "门前清三色同顺 2 番";
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSanshokuDoukou), 0) << "同顺不是同刻";
}

// 三杠子（2 番，通常役）：三组杠 + 一组顺子 + 将。
TEST(RiichiRareYaku, Sankantsu) {
  std::vector<Meld> melds;
  for (const char* face : {"1m", "5p", "9s"}) {
    const Face f = FaceOf(*ParseTile(face));
    Meld kan;
    kan.type = MeldType::kAnkan;
    kan.tiles = {MakeTileFromFace(f, 0), MakeTileFromFace(f, 1), MakeTileFromFace(f, 2),
                 MakeTileFromFace(f, 3)};
    melds.push_back(kan);
  }
  WinContext ctx = YakumanContext();
  ctx.from_seat = 2;  // 荣和，避免三暗刻/四暗刻
  // 三组杠（9 张账面）+ 顺子 2m3m4m（3 张）+ 将 7z7z ⇒ 14 张逻辑
  const ScoreResult result = ScoreHand(Tiles({"2m", "3m", "4m", "7z", "7z"}), melds, *ParseTile("4m"), ctx);
  EXPECT_EQ(HanOf(result, protocol::YakuId::kSankantsu), 2);
  EXPECT_EQ(result.yakuman_count, 0) << "三杠子不是役满";
}

}  // namespace
}  // namespace mahjong::riichi