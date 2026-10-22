// 麻将规则库单元测试（规格：game/docs/RULESET.md v1.0 §10）
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "server/mahjong_rules.h"

namespace mahjong {
namespace {

std::vector<Tile> H(std::vector<std::string> texts) {
  auto tiles = ParseTiles(texts);
  EXPECT_TRUE(tiles.has_value());
  return tiles.value_or(std::vector<Tile>{});
}

Meld Pong(std::vector<std::string> texts) {
  std::vector<Tile> tiles = H(std::move(texts));
  SortTiles(tiles);
  return Meld{.type = MeldType::kPong, .tiles = tiles};
}

Meld Kong(std::vector<std::string> texts) {
  std::vector<Tile> tiles = H(std::move(texts));
  SortTiles(tiles);
  return Meld{.type = MeldType::kKong, .tiles = tiles};
}

int Sum(const std::array<int, 4>& delta) {
  return delta[0] + delta[1] + delta[2] + delta[3];
}

// ---- B1 辅助：番种列表的常用断言形态 ----
std::vector<std::string> Ids(const std::vector<Yaku>& yaku) {
  std::vector<std::string> ids;
  ids.reserve(yaku.size());
  for (const Yaku& item : yaku) ids.push_back(item.id);
  return ids;
}

bool HasYakuId(const std::vector<Yaku>& yaku, std::string_view id) {
  for (const Yaku& item : yaku)
    if (item.id == id) return true;
  return false;
}

int HanOfYaku(const std::vector<Yaku>& yaku, std::string_view id) {
  for (const Yaku& item : yaku)
    if (item.id == id) return item.han;
  return 0;
}

bool HasTag(const std::vector<Tile>& hand, const std::vector<Meld>& melds, std::string_view id) {
  for (const std::string& tag : WinTags(hand, melds))
    if (tag == id) return true;
  return false;
}

std::string FormatHand(const std::vector<Tile>& tiles) {
  std::string out;
  for (Tile t : SortedTiles(tiles)) {
    out += FormatTile(t);
    out += ' ';
  }
  return out;
}

ScoreContext Ctx(int winner_seat, int from_seat, bool dealer = false, bool rinshan = false) {
  ScoreContext ctx;
  ctx.winner_seat = winner_seat;
  ctx.from_seat = from_seat;
  ctx.winner_is_dealer = dealer;
  ctx.is_rinshan = rinshan;
  return ctx;
}

Meld Ankan(std::vector<std::string> texts) {
  std::vector<Tile> tiles = H(std::move(texts));
  SortTiles(tiles);
  return Meld{.type = MeldType::kAnkan, .tiles = tiles};
}

Meld Kakan(std::vector<std::string> texts) {
  std::vector<Tile> tiles = H(std::move(texts));
  SortTiles(tiles);
  return Meld{.type = MeldType::kKakan, .tiles = tiles};
}

// 断言「番数 ↔ 倍率 ↔ 支付」三者自洽（§2.1/§2.2 的主公式，B1.1 裁决版：
// 自摸不是番种 ⇒ han 不含自摸；"自摸 ×2" 只在 2R/6R 支付公式里），返回最终番数供进一步断言
int ExpectScoreConsistent(const ScoreResult& score, const ScoreContext& ctx) {
  const int expected_han =
      std::min(YakuHanTotal(score.yaku) + (ctx.winner_is_dealer ? 1 : 0), 6);
  EXPECT_EQ(score.han, expected_han);
  EXPECT_EQ(score.multiplier, 1 << expected_han);
  EXPECT_EQ(score.per_payer, ctx.Tsumo() ? 2 * score.multiplier : score.multiplier);
  EXPECT_EQ(score.total, ctx.Tsumo() ? 6 * score.multiplier : score.multiplier);
  EXPECT_EQ(Sum(score.delta), 0) << "四家 delta 之和必须为 0";
  return score.han;
}

// ---------------------------------------------------------------------------
// 独立的暴力参照实现（与规则库的「最小牌面锚」算法走法不同）：
// 在每一层尝试所有牌面的刻子/顺子，带失败状态记忆化。仅用于差分测试。
// ---------------------------------------------------------------------------
bool BruteDecompose(std::array<uint8_t, kFaceCount> counts, int need_groups,
                    std::set<std::array<uint8_t, kFaceCount>>& failed) {
  if (need_groups == 0) {
    for (uint8_t c : counts) {
      if (c != 0) return false;
    }
    return true;
  }
  if (failed.count(counts) != 0) return false;
  for (int f = 0; f < kFaceCount; ++f) {
    if (counts[f] >= 3) {
      counts[f] -= 3;
      const bool ok = BruteDecompose(counts, need_groups - 1, failed);
      counts[f] += 3;
      if (ok) return true;
    }
    const int rank = f % kRankCount;  // 0-based
    // 顺子仅限同一门数牌；字牌面（≥27）编号连续但绝不成顺（与规则库同一约束，独立重写）
    if (f < kNumericFaceCount && rank <= kRankCount - 3 && counts[f] > 0 && counts[f + 1] > 0 &&
        counts[f + 2] > 0) {
      counts[f] -= 1;
      counts[f + 1] -= 1;
      counts[f + 2] -= 1;
      const bool ok = BruteDecompose(counts, need_groups - 1, failed);
      counts[f] += 1;
      counts[f + 1] += 1;
      counts[f + 2] += 1;
      if (ok) return true;
    }
  }
  failed.insert(counts);
  return false;
}

bool BruteCanWin(const std::vector<Tile>& tiles) {
  if (tiles.size() != 14) return false;
  const FaceCounts counts = CountFaces(tiles);
  int pairs = 0;
  int non_pairs = 0;
  for (uint8_t c : counts) {
    if (c > kCopiesPerFace) return false;
    if (c == 2) ++pairs;
    if (c != 0 && c != 2) ++non_pairs;
  }
  if (pairs == 7 && non_pairs == 0) return true;  // 七对子（独立复算，不复用规则库）
  std::array<uint8_t, kFaceCount> work = counts;
  for (int f = 0; f < kFaceCount; ++f) {
    if (work[f] >= 2) {
      work[f] -= 2;
      std::set<std::array<uint8_t, kFaceCount>> failed;
      const bool ok = BruteDecompose(work, 4, failed);
      work[f] += 2;
      if (ok) return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// 牌张：编码与排序（PROTOCOL.md §2）
// ---------------------------------------------------------------------------
TEST(MahjongRules, ParseAndSortTiles) {  // 1
  EXPECT_EQ(FormatTile(*ParseTile("1m")), "1m");
  EXPECT_EQ(FormatTile(*ParseTile("9s")), "9s");
  EXPECT_FALSE(ParseTile("0m").has_value());
  // t14 起字牌合法（1z..7z）；非法边界改为 8z/9z/5x 等，详见 HonorConstantsAndEncoding 用例
  EXPECT_TRUE(ParseTile("1z").has_value());
  EXPECT_FALSE(ParseTile("8z").has_value());
  EXPECT_FALSE(ParseTile("5x").has_value());
  EXPECT_FALSE(ParseTile("m1").has_value());
  EXPECT_FALSE(ParseTile("10p").has_value());

  std::vector<Tile> tiles =
      H({"9s", "3p", "1m", "2m", "1s", "3p", "1m", "5m", "9m", "3p", "4p", "2s", "2s", "7s"});
  const std::vector<Tile> sorted = SortedTiles(tiles);
  std::vector<std::string> texts;
  for (Tile t : sorted) texts.push_back(FormatTile(t));
  const std::vector<std::string> expected = {"1m", "1m", "2m", "5m", "9m", "3p", "3p", "3p",
                                             "4p", "1s", "2s", "2s", "7s", "9s"};
  EXPECT_EQ(texts, expected);
  EXPECT_EQ(kTileCount, 136);   // t14 口径：34 面 × 4
  EXPECT_EQ(FaceOf(*ParseTile("1m")), 0);
  EXPECT_EQ(FaceOf(*ParseTile("9s")), 26);
}

// ---------------------------------------------------------------------------
// 牌山与发牌（RULESET §1.3/§1.4）：同 seed 必须同牌山
// ---------------------------------------------------------------------------
TEST(MahjongRules, WallIsDeterministicAndComplete) {  // 2
  const std::vector<Tile> a = BuildWall(20261005);
  const std::vector<Tile> b = BuildWall(20261005);
  const std::vector<Tile> c = BuildWall(20261006);
  ASSERT_EQ(a.size(), static_cast<size_t>(kTileCount));
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);

  // 每面恰好 4 张、id 无重复
  FaceCounts counts = CountFaces(a);
  for (uint8_t n : counts) EXPECT_EQ(n, kCopiesPerFace);
  std::set<uint8_t> ids;
  for (Tile t : a) ids.insert(t.id);
  EXPECT_EQ(ids.size(), static_cast<size_t>(kTileCount));
}

TEST(MahjongRules, DealFromWall) {  // 3
  const Deal deal = DealTiles(BuildWall(42), /*dealer=*/0);
  EXPECT_EQ(deal.hands[0].size(), 14u);  // 庄家
  for (int seat = 1; seat < 4; ++seat) EXPECT_EQ(deal.hands[seat].size(), 13u);
  EXPECT_EQ(deal.remaining, 83);  // t14 口径：136 − 53
  EXPECT_EQ(deal.next_index, 53);

  // 张数守恒：发出去的牌 + 牌山剩余 = 136
  std::vector<Tile> all;
  for (const auto& hand : deal.hands) all.insert(all.end(), hand.begin(), hand.end());
  EXPECT_EQ(all.size(), 53u);
  std::set<uint8_t> ids;
  for (Tile t : all) ids.insert(t.id);
  EXPECT_EQ(ids.size(), 53u);  // 无重复发牌

  // 同 seed 发牌一致
  const Deal again = DealTiles(BuildWall(42), /*dealer=*/0);
  EXPECT_EQ(deal.hands, again.hands);
}

// ---------------------------------------------------------------------------
// 和牌判定的正例（RULESET §10.1）
// ---------------------------------------------------------------------------
TEST(MahjongRules, WinStandardHands) {  // 4
  // P1：4 顺子 + 将（荣和形态：门前 13 张 + 和牌张）
  std::vector<Tile> p1 = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s",
                            "3s", "5s"});
  EXPECT_TRUE(CanWinWith(p1, {}, *ParseTile("5s")));
  std::vector<Tile> p1_full = p1;
  p1_full.push_back(*ParseTile("5s"));
  // B1：`pinfu` 的语义由「没有番的普通和牌」改为「门清 + 全顺子 + 数牌将」1 番（GAME_DESIGN_V2 §5.1 #11），
  // 因此这手牌现在判出 menzen + pinfu 两条番种，不再是「无标签」。
  EXPECT_EQ(WinTags(p1_full, {}), (std::vector<std::string>{"menzen", "pinfu"}));

  // P2：含刻子
  std::vector<Tile> p2 = H({"1m", "1m", "1m", "2m", "3m", "4m", "5p", "5p", "5p", "7s", "8s",
                            "9s", "9m"});
  EXPECT_TRUE(CanWinWith(p2, {}, *ParseTile("9m")));

  // P8 边界：4 张同牌面同时充当刻子 + 顺子的一环
  std::vector<Tile> p8 = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "1m", "2m",
                            "3m", "5m", "5m"});
  EXPECT_TRUE(CanWinExact(p8, {}));
}

TEST(MahjongRules, WinSevenPairs) {  // 5
  std::vector<Tile> p3 = H({"1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p",
                            "6p", "8p", "8p"});
  EXPECT_TRUE(CanWinExact(p3, {}));
  EXPECT_TRUE(IsSevenPairs(p3));
  EXPECT_EQ(WinTags(p3, {}), (std::vector<std::string>{"menzen", "seven_pairs"}));

  // 自摸形态统一入口
  EXPECT_TRUE(IsWinningHand(p3, {}, *ParseTile("8p"), /*is_tsumo=*/true));
}

TEST(MahjongRules, WinPureOneSuit) {  // 6
  std::vector<Tile> p4 = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m",
                            "6m", "7m", "7m"});
  EXPECT_TRUE(CanWinExact(p4, {}));
  EXPECT_FALSE(IsSevenPairs(p4));
  // 清一色 5 番 + 门清 1 番 + 三暗刻 2 番（111/222/333 三组暗刻）
  EXPECT_EQ(WinTags(p4, {}),
            (std::vector<std::string>{"menzen", "sanankou", "pure_one_suit"}));

  std::vector<Tile> p5 = H({"1m", "1m", "2m", "2m", "3m", "3m", "4m", "4m", "5m", "5m", "6m",
                            "6m", "7m", "7m"});
  EXPECT_TRUE(CanWinExact(p5, {}));
  // 七对子（1+2+5=8 番）与「四顺子 + 将」拆法（menzen1+pinfu1+iipeikou1+pure5=8 番）同分；
  // §1.3.4：同分取**枚举序第一个** ⇒ 标准型拆法（枚举先于七对子候选）胜出。
  EXPECT_EQ(WinTags(p5, {}),
            (std::vector<std::string>{"menzen", "pinfu", "iipeikou", "pure_one_suit"}));
}

TEST(MahjongRules, WinWithMelds) {  // 7
  // P6：1 组碰 + 门前 10 张 + 和牌张
  const std::vector<Meld> pong = {Pong({"5p", "5p", "5p"})};
  std::vector<Tile> p6 = H({"1m", "2m", "3m", "4m", "5m", "6m", "7s", "8s", "9s", "3p"});
  EXPECT_TRUE(CanWinWith(p6, pong, *ParseTile("3p")));
  EXPECT_EQ(MeldCountOf(pong), 1);
  EXPECT_EQ(KanCountOf(pong), 0);

  // P7：1 组明杠（逻辑 14 张、物理 15 张）
  const std::vector<Meld> kong = {Kong({"9s", "9s", "9s", "9s"})};
  std::vector<Tile> p7 = H({"1m", "2m", "3m", "4p", "5p", "6p", "1s", "2s", "3s", "2m"});
  std::vector<Tile> p7_full = p7;
  p7_full.push_back(*ParseTile("2m"));
  ASSERT_EQ(p7_full.size(), 11u);
  EXPECT_TRUE(CanWinWith(p7, kong, *ParseTile("2m")));
  EXPECT_EQ(KanCountOf(kong), 1);
  EXPECT_TRUE(IsValidMeld(kong[0]));
}

// ---------------------------------------------------------------------------
// 和牌判定的反例（RULESET §10.2）：不允许误判为和牌
// ---------------------------------------------------------------------------
TEST(MahjongRules, NotWinMissingPair) {  // 8
  std::vector<Tile> n1 = H({"1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "1p", "2p",
                            "3p", "5p", "7p"});
  EXPECT_FALSE(CanWinExact(n1, {}));
}

TEST(MahjongRules, NotWinIsolatedTiles) {  // 9
  std::vector<Tile> n2 = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s",
                            "3s", "5s", "6s"});
  EXPECT_FALSE(CanWinExact(n2, {}));

  std::vector<Tile> n7 = H({"1m", "1m", "2m", "2m", "3m", "3m", "4m", "4m", "5m", "5m", "6m",
                            "6m", "7m", "9m"});
  EXPECT_FALSE(CanWinExact(n7, {}));  // 9m 无法归属任何面子
}

TEST(MahjongRules, NotWinFourOfAKindIsNotTwoPairs) {  // 10
  std::vector<Tile> n3 = H({"1m", "1m", "1m", "1m", "2p", "2p", "3p", "3p", "4p", "4p", "5p",
                            "5p", "6p", "6p"});
  EXPECT_FALSE(IsSevenPairs(n3));
  EXPECT_FALSE(CanWinExact(n3, {}));
}

TEST(MahjongRules, NotWinWrongTileCount) {  // 11
  std::vector<Tile> n4 = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s",
                            "3s", "5s"});
  EXPECT_FALSE(CanWinExact(n4, {}));  // 13 张

  std::vector<Tile> n5 = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s",
                            "3s", "5s", "5s", "5s"});
  EXPECT_FALSE(CanWinExact(n5, {}));  // 15 张

  std::vector<Tile> n6 = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "7p"});
  EXPECT_FALSE(CanWinExact(n6, {}));  // 只有 2 组面子 + 将
}

TEST(MahjongRules, NotWinSevenPairsRequiresConcealed) {  // 12
  const std::vector<Meld> pong = {Pong({"1m", "1m", "1m"})};
  std::vector<Tile> n8 = H({"1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p", "6p"});
  ASSERT_EQ(n8.size(), 11u);  // 1 组副露时门前应为 11 张
  EXPECT_FALSE(CanWinExact(n8, pong));
  EXPECT_TRUE(WinTags(n8, pong).empty());
}

// ---------------------------------------------------------------------------
// 算分（RULESET §6）
// ---------------------------------------------------------------------------
TEST(MahjongRules, ScoreRonAndTsumo) {  // 13
  std::vector<Tile> hira = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s",
                              "3s", "5s", "5s"});
  // 番数制（B1）：这手牌 = menzen 1 + pinfu 1 = 2 番 ⇒ R = 4
  // S1 荣和：放炮者 −4、和牌者 +4
  const ScoreResult ron = ScoreWin(/*winner=*/1, /*from_seat=*/0, hira, {});
  EXPECT_EQ(ron.han, 2);
  EXPECT_EQ(ron.multiplier, 4);
  EXPECT_EQ(ron.per_payer, 4);
  EXPECT_EQ(ron.total, 4);
  EXPECT_EQ(ron.delta[0], -4);
  EXPECT_EQ(ron.delta[1], 4);
  EXPECT_EQ(ron.delta[2], 0);
  EXPECT_EQ(ron.delta[3], 0);
  EXPECT_EQ(Sum(ron.delta), 0);

  // S3 自摸（B1.1 裁决）：自摸不是番种 ⇒ han 仍 2 番 ⇒ R = 4；三家各付 2R = 8、和牌者 +6R = 24
  const ScoreResult tsumo = ScoreWin(/*winner=*/2, /*from_seat=*/-1, hira, {});
  EXPECT_EQ(tsumo.han, 2);
  EXPECT_EQ(tsumo.multiplier, 4);
  EXPECT_EQ(tsumo.per_payer, 8);
  EXPECT_EQ(tsumo.total, 24);
  EXPECT_EQ(tsumo.delta[0], -8);
  EXPECT_EQ(tsumo.delta[1], -8);
  EXPECT_EQ(tsumo.delta[3], -8);
  EXPECT_EQ(tsumo.delta[2], 24);
  EXPECT_EQ(Sum(tsumo.delta), 0);

  // S5 荒庄：四家全 0
  const ScoreResult draw = ScoreDraw();
  EXPECT_EQ(draw.multiplier, 0);
  EXPECT_EQ(draw.han, 0);
  EXPECT_EQ(Sum(draw.delta), 0);
  for (int d : draw.delta) EXPECT_EQ(d, 0);
}

TEST(MahjongRules, ScoreMultipliers) {  // 14
  std::vector<Tile> seven = H({"1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p",
                               "6p", "8p", "8p"});
  std::vector<Tile> pure = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m",
                              "6m", "7m", "7m"});
  std::vector<Tile> both = H({"1m", "1m", "2m", "2m", "3m", "3m", "4m", "4m", "5m", "5m", "6m",
                              "6m", "7m", "7m"});

  // 七对子 = menzen 1 + seven_pairs 2 = 3 番 ⇒ R = 8
  const ScoreResult r = ScoreWin(0, 3, seven, {});
  EXPECT_EQ(r.han, 3);
  EXPECT_EQ(r.multiplier, 8);
  EXPECT_EQ(r.delta[3], -8);
  EXPECT_EQ(r.delta[0], 8);
  EXPECT_EQ(Sum(r.delta), 0);

  // 清一色（menzen 1 + sanankou 2 + pure_one_suit 5 = 8 番）⇒ 封顶 6 番 ⇒ R = 64
  const ScoreResult p = ScoreWin(0, 3, pure, {});
  EXPECT_EQ(p.han, 6);
  EXPECT_EQ(p.multiplier, 64);

  // 七对子 + 清一色自摸：满贯封顶 6 番 ⇒ 三家各 −128、和牌者 +384
  const ScoreResult t = ScoreWin(1, -1, both, {});
  EXPECT_EQ(t.han, 6);
  EXPECT_EQ(t.multiplier, 64);
  EXPECT_EQ(t.per_payer, 128);
  EXPECT_EQ(t.total, 384);
  EXPECT_EQ(t.delta[1], 384);
  EXPECT_EQ(t.delta[0], -128);
  EXPECT_EQ(t.delta[2], -128);
  EXPECT_EQ(t.delta[3], -128);
  EXPECT_EQ(Sum(t.delta), 0);
}

// ---------------------------------------------------------------------------
// 动作可选项与超时默认动作（RULESET §5）
// ---------------------------------------------------------------------------
TEST(MahjongRules, ClaimOptions) {  // 15
  // 碰：手中有 2 张
  const std::vector<Tile> two = H({"9s", "9s", "1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p",
                                   "9p", "1p", "2p"});
  const auto pong_options = ClaimOptions(two, {}, *ParseTile("9s"), 55, /*is_next_player=*/false);
  ASSERT_NE(FindOption(pong_options, "pong"), nullptr);
  EXPECT_EQ(FindOption(pong_options, "kong"), nullptr);
  EXPECT_EQ(pong_options.front().action, "pong");  // 顺序：win → kong → pong → chow → pass
  EXPECT_EQ(pong_options.back().action, "pass");
  EXPECT_TRUE(HasNonPassOption(pong_options));
  EXPECT_TRUE(OptionTilesMatch(*FindOption(pong_options, "pong"), H({"9s"})));

  // 杠：手中 3 张且 remaining ≥ 1；remaining == 0 时不提供
  const std::vector<Tile> three = H({"9s", "9s", "9s", "1m", "2m", "3m", "4m", "5m", "6m", "7p",
                                     "8p", "9p", "1p"});
  const auto kong_ok = ClaimOptions(three, {}, *ParseTile("9s"), 55, false);
  ASSERT_NE(FindOption(kong_ok, "kong"), nullptr);
  ASSERT_NE(FindOption(kong_ok, "pong"), nullptr);
  EXPECT_EQ(kong_ok[0].action, "kong");
  EXPECT_EQ(kong_ok[1].action, "pong");
  const auto kong_blocked = ClaimOptions(three, {}, *ParseTile("9s"), 0, false);
  EXPECT_EQ(FindOption(kong_blocked, "kong"), nullptr);
  EXPECT_NE(FindOption(kong_blocked, "pong"), nullptr);

  // 吃：仅下家；每个合法组合一条选项
  const std::vector<Tile> chow_hand = H({"1p", "2p", "4p", "5p", "1m", "2m", "3m", "4m", "5m",
                                         "6m", "7m", "8m", "9m"});
  const auto chow_next = ClaimOptions(chow_hand, {}, *ParseTile("3p"), 55, /*is_next_player=*/true);
  int chow_count = 0;
  for (const Option& option : chow_next) {
    if (option.action == "chow") {
      ++chow_count;
      EXPECT_EQ(option.tiles.size(), 2u);
    }
  }
  EXPECT_EQ(chow_count, 3);  // {1p,2p}、{2p,4p}、{4p,5p}（RULESET §4.2）
  const auto chow_not_next = ClaimOptions(chow_hand, {}, *ParseTile("3p"), 55, false);
  EXPECT_EQ(FindOption(chow_not_next, "chow"), nullptr);
  EXPECT_FALSE(HasNonPassOption(chow_not_next));  // 只剩 pass
}

TEST(MahjongRules, DiscardOptionsAndDefaultDiscard) {  // 16
  std::vector<Tile> winning = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s",
                                 "2s", "3s", "5s", "5s"});
  const auto win_options = DiscardOptions(winning, {});
  ASSERT_EQ(win_options.size(), 1u);
  EXPECT_EQ(win_options[0].action, "win");

  std::vector<Tile> not_winning = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s",
                                     "2s", "5s", "6s", "9s"});
  EXPECT_TRUE(DiscardOptions(not_winning, {}).empty());

  // 超时默认打牌：有摸牌记录 → 打刚摸到的牌
  std::vector<Tile> hand = H({"1m", "1m", "5s", "9s", "3p"});
  EXPECT_EQ(FormatTile(*DefaultDiscard(hand, *ParseTile("1m"))), "1m");
  // 无摸牌记录 → 排序后最后一张（最大牌）
  EXPECT_EQ(FormatTile(*DefaultDiscard(hand, std::nullopt)), "9s");
  EXPECT_FALSE(DefaultDiscard({}, std::nullopt).has_value());
}

// ---------------------------------------------------------------------------
// 差分测试：构造正例 + 变异牌型，与独立的暴力参照实现逐例比对
// ---------------------------------------------------------------------------
TEST(MahjongRules, DifferentialAgainstBruteForce) {  // 17
  std::mt19937 rng(20261005);
  std::uniform_int_distribution<int> face_dist(0, kFaceCount - 1);
  std::uniform_int_distribution<int> run_dist(0, 6);
  std::uniform_int_distribution<int> suit_dist(0, 2);
  std::uniform_int_distribution<int> style_dist(0, 1);
  // 字牌面 27..33：构造刻子/将时按 50% 概率取字牌，使语料大量含字牌
  std::uniform_int_distribution<int> honor_dist(kNumericFaceCount, kFaceCount - 1);
  std::uniform_int_distribution<int> coin(0, 1);

  int constructed = 0;
  int mutated = 0;
  int honor_faces_used = 0;

  for (int iter = 0; iter < 4000; ++iter) {
    // 1) 构造一个必然和牌的 14 张（4 面子 + 将，或七对子），牌面不超过 4 张
    std::vector<Tile> tiles;
    const bool seven_pairs_style = style_dist(rng) == 0;
    if (seven_pairs_style) {
      std::set<int> faces;
      // 七对子里也混入字牌对子
      while (faces.size() < 7) {
        if (faces.empty() || coin(rng) == 0) {
          faces.insert(honor_dist(rng));
        } else {
          faces.insert(face_dist(rng));
        }
      }
      for (int f : faces) {
        if (f >= kNumericFaceCount) ++honor_faces_used;
        tiles.push_back(MakeTileFromFace(static_cast<Face>(f)));
        tiles.push_back(MakeTileFromFace(static_cast<Face>(f)));
      }
    } else {
      for (int group = 0; group < 4; ++group) {
        if (coin(rng) == 0) {
          // 刻子：数牌或字牌都可以
          const int face = coin(rng) == 0 ? honor_dist(rng) : face_dist(rng);
          if (face >= kNumericFaceCount) ++honor_faces_used;
          for (int k = 0; k < 3; ++k) tiles.push_back(MakeTileFromFace(static_cast<Face>(face)));
        } else {
          // 顺子：只能由同一门数牌构成（字牌绝不成顺）
          const int suit = suit_dist(rng);
          const int start = suit * kRankCount + run_dist(rng);
          for (int k = 0; k < 3; ++k) {
            tiles.push_back(MakeTileFromFace(static_cast<Face>(start + k)));
          }
        }
      }
      const int pair_face = coin(rng) == 0 ? honor_dist(rng) : face_dist(rng);
      if (pair_face >= kNumericFaceCount) ++honor_faces_used;
      tiles.push_back(MakeTileFromFace(static_cast<Face>(pair_face)));
      tiles.push_back(MakeTileFromFace(static_cast<Face>(pair_face)));
    }
    const FaceCounts counts = CountFaces(tiles);
    bool valid = true;
    for (uint8_t c : counts) {
      if (c > kCopiesPerFace) valid = false;
    }
    if (!valid) continue;

    ASSERT_TRUE(CanWinExact(tiles, {})) << "构造的和牌被误判为不和";
    EXPECT_TRUE(BruteCanWin(tiles));
    ++constructed;

    // 2) 变异一张牌（换一个牌面），与暴力参照实现比对
    std::vector<Tile> mutated_hand = tiles;
    const int pos = static_cast<int>(rng() % mutated_hand.size());
    mutated_hand[static_cast<size_t>(pos)] = MakeTileFromFace(static_cast<Face>(face_dist(rng)));
    const FaceCounts mutated_counts = CountFaces(mutated_hand);
    bool mutated_valid = true;
    for (uint8_t c : mutated_counts) {
      if (c > kCopiesPerFace) mutated_valid = false;
    }
    if (!mutated_valid) continue;
    const bool fast = CanWinExact(mutated_hand, {});
    const bool brute = BruteCanWin(mutated_hand);
    EXPECT_EQ(fast, brute) << "规则库与暴力参照实现不一致";
    ++mutated;
  }

  EXPECT_GT(constructed, 3000);
  EXPECT_GT(mutated, 3000);
  EXPECT_GT(honor_faces_used, 1000) << "语料必须大量含字牌";
}

// ---------------------------------------------------------------------------
// v1.2（t15）：7 种字牌 —— 常量、编码、排序
// ---------------------------------------------------------------------------
TEST(MahjongRules, HonorConstantsAndEncoding) {  // 20
  // 常量（RULESET v1.2 §1.1/§1.2）
  EXPECT_EQ(kFaceCount, 34);
  EXPECT_EQ(kTileCount, 136);
  EXPECT_EQ(kWallSizeAfterDeal, 83);
  EXPECT_EQ(kHonorCount, 7);
  EXPECT_EQ(kNumericFaceCount, 27);

  // 编码往返 + 面编号映射（27=1z 东 … 33=7z 中）
  const std::vector<std::string> honors = {"1z", "2z", "3z", "4z", "5z", "6z", "7z"};
  const std::vector<std::string> names = {"东", "南", "西", "北", "白", "发", "中"};
  for (size_t i = 0; i < honors.size(); ++i) {
    const auto tile = ParseTile(honors[i]);
    ASSERT_TRUE(tile.has_value()) << honors[i];
    EXPECT_EQ(FormatTile(*tile), honors[i]);                       // 往返可逆
    EXPECT_EQ(FaceOf(*tile), 27 + static_cast<int>(i));            // 面编号 27..33
    EXPECT_EQ(SuitOf(FaceOf(*tile)), 3);                           // 字牌花色编号 3
    EXPECT_EQ(RankOf(FaceOf(*tile)), i + 1);                       // 1..7
    EXPECT_TRUE(IsHonorFace(FaceOf(*tile)));
    EXPECT_FALSE(IsNumberFace(FaceOf(*tile)));
    EXPECT_EQ(HonorName(FaceOf(*tile)), names[i]);                 // 与文档映射逐字一致
    EXPECT_EQ(*tile, *ParseTile(honors[i]));
  }
  EXPECT_TRUE(HonorName(FaceOf(*ParseTile("1m"))).empty());        // 非字牌无名

  // 非法编码必须被拒（PROTOCOL §2.1 合取正则）
  for (const char* bad : {"8z", "9z", "0z", "10z", "5x", "z1", "", "1Z", "m1"}) {
    EXPECT_FALSE(ParseTile(bad).has_value()) << bad;
  }

  // 排序：先数牌后字牌；字牌内 1z..7z
  std::vector<Tile> tiles = H({"7z", "1z", "9s", "1m", "5z", "3p", "2z", "1s"});
  const std::vector<Tile> sorted = SortedTiles(tiles);
  std::vector<std::string> texts;
  for (Tile t : sorted) texts.push_back(FormatTile(t));
  const std::vector<std::string> expected = {"1m", "3p", "1s", "9s", "1z", "2z", "5z", "7z"};
  EXPECT_EQ(texts, expected);
}

// ---------------------------------------------------------------------------
// v1.2（t15）：字牌参与和牌判定（一视同仁，无特例分支）
// ---------------------------------------------------------------------------
TEST(MahjongRules, HonorInWinningHands) {  // 21
  // 字牌做刻子：3 组字牌刻子（白发中）+ 1 组数牌刻子 + 将
  std::vector<Tile> honor_triplet = H({"5z", "5z", "5z", "6z", "6z", "6z", "7z", "7z", "7z",
                                       "1m", "1m", "1m", "9s", "9s"});
  EXPECT_TRUE(CanWinExact(honor_triplet, {}));
  // menzen 1 + yakuhai 3（白/发/中各一组，合并为一条）+ toitoi 2 + sanankou 2 = 8 番
  EXPECT_EQ(WinTags(honor_triplet, {}),
            (std::vector<std::string>{"menzen", "yakuhai", "toitoi", "sanankou"}));
  // 字牌不产生清一色 / 混一色（全字牌不满足「恰含一门数牌」）
  EXPECT_TRUE(IsPureOneSuit(honor_triplet, {}) == false);
  EXPECT_FALSE(HasTag(honor_triplet, {}, "honitsu"));

  // 字牌做将：4 组数牌面子 + 将 1z1z ⇒ 只有门清（将牌是字牌 ⇒ 平和不成立）
  std::vector<Tile> honor_pair = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s",
                                    "2s", "3s", "1z", "1z"});
  EXPECT_TRUE(CanWinExact(honor_pair, {}));
  EXPECT_EQ(WinTags(honor_pair, {}), std::vector<std::string>{"menzen"});

  // 字牌做将（荣和形态：13 张 + 和牌张）
  std::vector<Tile> ron_hand = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s",
                                  "2s", "3s", "1z"});
  EXPECT_TRUE(CanWinWith(ron_hand, {}, *ParseTile("1z")));

  // 字牌 + 数牌混合刻子（含中张顺子）
  std::vector<Tile> mixed = H({"2z", "2z", "2z", "3p", "4p", "5p", "6s", "7s", "8s", "4z", "4z",
                               "4z", "9m", "9m"});
  EXPECT_TRUE(CanWinExact(mixed, {}));
}

TEST(MahjongRules, HonorSevenPairsAndScoring) {  // 22
  // 含字牌对子的七对子
  std::vector<Tile> honor_seven = H({"1z", "1z", "2z", "2z", "3m", "3m", "4m", "4m", "5p", "5p",
                                     "6p", "6p", "7s", "7s"});
  EXPECT_TRUE(IsSevenPairs(honor_seven));
  EXPECT_TRUE(CanWinExact(honor_seven, {}));
  // menzen 1 + seven_pairs 2 = 3 番（字牌不额外加成，也不构成混一色：三色数牌）
  EXPECT_EQ(WinTags(honor_seven, {}), (std::vector<std::string>{"menzen", "seven_pairs"}));
  const ScoreResult r = ScoreWin(0, 3, honor_seven, {});
  EXPECT_EQ(r.han, 3);
  EXPECT_EQ(r.multiplier, 8);
  EXPECT_EQ(r.delta[0], 8);
  EXPECT_EQ(r.delta[3], -8);
  EXPECT_EQ(Sum(r.delta), 0);

  // 全字牌七对子：仍只是七对子（无「字一色」→ 不是清一色，也不是混一色）
  std::vector<Tile> all_honor_seven = H({"1z", "1z", "2z", "2z", "3z", "3z", "4z", "4z", "5z",
                                         "5z", "6z", "6z", "7z", "7z"});
  EXPECT_TRUE(CanWinExact(all_honor_seven, {}));
  EXPECT_EQ(WinTags(all_honor_seven, {}),
            (std::vector<std::string>{"menzen", "seven_pairs"}));
  EXPECT_EQ(ScoreWin(0, 3, all_honor_seven, {}).multiplier, 8);  // 不得拿到清一色/混一色的倍率
}

TEST(MahjongRules, HonorIsNotPureOneSuit) {  // 23
  // 全字牌（4 组字牌刻子 + 字牌将）：能和的，但**不是**清一色（RULESET §3.4 无「字一色」），
  // 也**不是**混一色（要求「恰含一门数牌」）
  std::vector<Tile> all_honor = H({"1z", "1z", "1z", "2z", "2z", "2z", "3z", "3z", "3z", "4z",
                                   "4z", "4z", "5z", "5z"});
  EXPECT_TRUE(CanWinExact(all_honor, {}));
  EXPECT_FALSE(IsPureOneSuit(all_honor, {}));
  EXPECT_FALSE(HasTag(all_honor, {}, "pure_one_suit"));
  EXPECT_FALSE(HasTag(all_honor, {}, "honitsu"));
  // menzen 1 + toitoi 2 + sanankou 2 = 5 番
  EXPECT_EQ(WinTags(all_honor, {}),
            (std::vector<std::string>{"menzen", "toitoi", "sanankou"}));

  // 含单个字牌对的数牌手牌 → 不是清一色，而是**混一色**（B1 新增番种，§1.4）
  std::vector<Tile> one_honor_pair = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m",
                                        "4m", "5m", "6m", "1z", "1z"});
  EXPECT_TRUE(CanWinExact(one_honor_pair, {}));
  EXPECT_FALSE(IsPureOneSuit(one_honor_pair, {}));
  EXPECT_FALSE(HasTag(one_honor_pair, {}, "pure_one_suit"));
  EXPECT_TRUE(HasTag(one_honor_pair, {}, "honitsu"));

  // 副露含字牌刻子时同样不是清一色（1 组副露 ⇒ 门前 11 张：3 组面子 + 将）
  const std::vector<Meld> honor_meld = {Pong({"5z", "5z", "5z"})};
  std::vector<Tile> with_meld = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "4m", "5m", "6m",
                                   "6m"});
  ASSERT_EQ(with_meld.size(), 11u);
  EXPECT_TRUE(CanWinExact(with_meld, honor_meld));
  EXPECT_FALSE(IsPureOneSuit(with_meld, honor_meld));
  EXPECT_FALSE(HasTag(with_meld, honor_meld, "pure_one_suit"));
}

TEST(MahjongRules, HonorCannotChow) {  // 24
  // 字牌不可吃：即使手里有看似「连续」的字牌，也不得产生 chow 组合
  std::vector<Tile> hand = H({"1z", "2z", "3z", "4z", "5z", "6z", "7z", "1m", "2m", "3m", "4m",
                              "5m", "6m"});
  for (const char* honor : {"1z", "2z", "3z", "4z", "5z", "6z", "7z"}) {
    const auto tile = *ParseTile(honor);
    EXPECT_TRUE(ChowCombinations(hand, tile).empty()) << honor;
    EXPECT_FALSE(CanChow(hand, tile)) << honor;
    // 对应地，claim 选项里不得出现 chow（但 pass 恒在）
    const auto options = ClaimOptions(hand, {}, tile, 83, /*is_next_player=*/true);
    EXPECT_EQ(FindOption(options, "chow"), nullptr) << honor;
    EXPECT_NE(FindOption(options, "pass"), nullptr) << honor;
  }
  // 对照：同样「连续」的数牌可以吃
  std::vector<Tile> number_hand = H({"1m", "2m", "3m", "4m", "5m", "6m", "7m", "9s", "9s", "1p",
                                     "2p", "4p", "5p"});
  EXPECT_FALSE(ChowCombinations(number_hand, *ParseTile("3p")).empty());
}

TEST(MahjongRules, HonorRunIsNotAWinningHand) {  // 25
  // 只有靠「字牌成顺」才能和的手牌 → 必须判不和（字牌不可成顺）
  std::vector<Tile> honor_run = H({"1z", "2z", "3z", "1m", "2m", "3m", "4p", "5p", "6p", "7s",
                                   "8s", "9s", "5z", "5z"});
  EXPECT_FALSE(CanWinExact(honor_run, {}));

  // 与上面同构、但把「伪顺子」换成真顺子 → 必须判和（证明拒绝的是字牌顺子而非整个牌型）
  std::vector<Tile> real_run = H({"1m", "2m", "3m", "2m", "3m", "4m", "4p", "5p", "6p", "7s",
                                  "8s", "9s", "5z", "5z"});
  EXPECT_TRUE(CanWinExact(real_run, {}));

  // 边界：2z3z4z / 6z7z 这类组合都不得成顺
  EXPECT_FALSE(CanWinExact(H({"2z", "3z", "4z", "1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s",
                              "9s", "5z", "5z"}),
                           {}));
  EXPECT_FALSE(CanWinExact(H({"6z", "7z", "1z", "1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s",
                              "9s", "5z", "5z"}),
                           {}));
}

TEST(MahjongRules, HonorTileConservation) {  // 26
  // 任一字牌面 > 4 张 → 判非法
  EXPECT_FALSE(IsVerifiedHand(H({"1z", "1z", "1z", "1z", "1z"})));
  EXPECT_FALSE(CanWinExact(H({"5z", "5z", "5z", "5z", "5z", "1m", "2m", "3m", "4m", "5m", "6m",
                              "7p", "8p", "9p"}),
                           {}));
  // 恰好 4 张合法
  EXPECT_TRUE(IsVerifiedHand(H({"1z", "1z", "1z", "1z"})));
  // 字牌明杠（4 张）+ 手牌再出现同面 → 合计 5 张，非法
  const std::vector<Meld> honor_kong = {Kong({"5z", "5z", "5z", "5z"})};
  EXPECT_FALSE(CanWinExact(H({"5z", "1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "9s", "2m"}),
                           honor_kong));
  // 字牌碰 + 合法和牌形则成立
  const std::vector<Meld> honor_pong = {Pong({"1z", "1z", "1z"})};
  std::vector<Tile> hand = H({"1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "9s", "2m", "2m"});
  EXPECT_TRUE(CanWinExact(hand, honor_pong));
  EXPECT_TRUE(CanPong(H({"1z", "1z", "3m"}), *ParseTile("1z")));   // 碰对字牌可用
  EXPECT_TRUE(CanKong(H({"1z", "1z", "1z", "3m"}), *ParseTile("1z"), 83));
  EXPECT_TRUE(CanKong(H({"1z", "1z", "1z", "3m"}), *ParseTile("1z"), 0) == false);  // 无牌可补则不可杠
}

// ---------------------------------------------------------------------------
// 副露合法性（RULESET §4）
// ---------------------------------------------------------------------------
TEST(MahjongRules, MeldLegality) {  // 18
  EXPECT_TRUE(IsValidMeld(Pong({"5p", "5p", "5p"})));
  EXPECT_TRUE(IsValidMeld(Kong({"9s", "9s", "9s", "9s"})));
  EXPECT_TRUE(IsValidMeld(Meld{.type = MeldType::kChow, .tiles = H({"3p", "4p", "5p"})}));
  EXPECT_FALSE(IsValidMeld(Meld{.type = MeldType::kChow, .tiles = H({"3p", "4p", "6p"})}));
  EXPECT_FALSE(IsValidMeld(Meld{.type = MeldType::kChow, .tiles = H({"8p", "9p", "1s"})}));
  EXPECT_FALSE(IsValidMeld(Pong({"5p", "5p"})));
  EXPECT_FALSE(IsValidMeld(Kong({"9s", "9s", "9s"})));

  std::vector<Tile> hand = H({"5p", "5p", "6p", "6p", "7p", "1m", "2m", "3m", "4m", "5m", "6m",
                              "7m", "8m"});
  EXPECT_TRUE(CanPong(hand, *ParseTile("5p")));
  EXPECT_FALSE(CanKong(hand, *ParseTile("5p"), 55));
  EXPECT_TRUE(CanChow(hand, *ParseTile("8p")));   // 6p 7p
  EXPECT_TRUE(CanChow(hand, *ParseTile("5p")));   // 6p 7p
  EXPECT_FALSE(CanChow(hand, *ParseTile("9p")));  // 无 7p8p
  EXPECT_FALSE(CanChow(hand, *ParseTile("1p")));
}

// ---------------------------------------------------------------------------
// 张数守恒与手牌不变量（RULESET §2.2）
// ---------------------------------------------------------------------------
TEST(MahjongRules, HandSizeInvariants) {  // 19
  const std::vector<Meld> melds = {Pong({"5p", "5p", "5p"})};
  // 有 1 组副露时：等待态 concealed = 10，行动态 = 11
  std::vector<Tile> waiting = H({"1m", "2m", "3m", "4m", "5m", "6m", "7s", "8s", "9s", "3p"});
  EXPECT_EQ(waiting.size() + 3u * MeldCountOf(melds), 13u);
  std::vector<Tile> active = waiting;
  active.push_back(*ParseTile("3p"));
  EXPECT_EQ(active.size() + 3u * MeldCountOf(melds), 14u);
  EXPECT_TRUE(CanWinExact(active, melds));

  // 明杠：物理张数 = 14 + 杠数
  const std::vector<Meld> kong = {Kong({"9s", "9s", "9s", "9s"})};
  std::vector<Tile> kong_active = H({"1m", "2m", "3m", "4p", "5p", "6p", "1s", "2s", "3s", "2m",
                                     "2m"});
  EXPECT_EQ(kong_active.size() + 3u * MeldCountOf(kong) + KanCountOf(kong), 15u);
  EXPECT_TRUE(CanWinExact(kong_active, kong));
  // 牌张守恒：与杠重复的第 5 张同面牌必须判非法
  EXPECT_FALSE(CanWinExact(H({"1m", "2m", "3m", "4p", "5p", "6p", "9s", "9s", "9s", "2m", "2m"}),
                           kong));
  EXPECT_FALSE(IsVerifiedHand(H({"1m", "1m", "1m", "1m", "1m"})));  // 5 张同面非法
}


// ---------------------------------------------------------------------------
// t20：向听数与牌效率弃牌策略
// ---------------------------------------------------------------------------
namespace {
// 打出一张后的门前手牌
std::vector<Tile> RemoveOne(const std::vector<Tile>& hand, Face face) {
  std::vector<Tile> out;
  bool removed = false;
  for (Tile t : hand) {
    if (!removed && FaceOf(t) == face) {
      removed = true;
      continue;
    }
    out.push_back(t);
  }
  return out;
}
// 候选打牌的最优向听（本口径的评分）
int ScoreOf(const std::vector<Tile>& hand, const std::vector<Meld>& melds, Face face) {
  return BestShantenAfterDraw(RemoveOne(hand, face), melds);
}
// 「只看向听数」的朴素口径：打牌后 13 张的向听最小，并列时优先打刚摸到的牌
Tile OnlyShantenPick(const std::vector<Tile>& hand, Tile drawn) {
  std::vector<Face> cands;
  for (Tile t : hand) {
    if (std::find(cands.begin(), cands.end(), FaceOf(t)) == cands.end()) cands.push_back(FaceOf(t));
  }
  std::sort(cands.begin(), cands.end());
  int best = 99;
  Face best_face = cands.front();
  for (Face f : cands) {
    const int s = Shanten(RemoveOne(hand, f), {});
    if (s < best) {
      best = s;
      best_face = f;
    } else if (s == best && f == FaceOf(drawn)) {
      best_face = f;
    }
  }
  return MakeTileFromFace(best_face);
}
}  // namespace

TEST(MahjongRules, ShantenKnownHands) {  // 27
  // 口径：-1 = 和牌形（逻辑 14 张）；0 = 听牌（13 张）；n = n 向听
  struct Case {
    std::vector<Tile> hand;
    std::vector<Meld> melds;
    int expect;
  };
  std::vector<Case> cases = {
      {H({"1m","2m","3m","4m","5m","6m","7p","8p","9p","1s","2s","3s","5s","5s"}), {}, -1},  // 和牌形
      {H({"1m","2m","3m","4m","5m","6m","7p","8p","9p","1s","2s","3s","5s"}), {}, 0},        // 听牌
      {H({"1m","2m","3m","4m","5m","6m","7p","8p","9p","3s","4s","1z","5z"}), {}, 1},        // 一向听
      {H({"1m","2m","3m","4m","5m","6m","7p","8p","3s","4s","1z","5z","9m"}), {}, 2},        // 两向听
      {H({"1m","2m","3m","4m","5m","6m","7p","3s","1z","5z","9m","2p","4p"}), {}, 3},        // 三向听
      {H({"1m","1m","2m","2m","3m","3m","4p","4p","5p","5p","6p","6p","7s"}), {}, 0},         // 七对子听牌
      {H({"1m","1m","2m","2m","3m","3m","4p","4p","5p","5p","1z","3z","5z"}), {}, 1},         // 七对子一向听
      {H({"1m","2m","3m","4m","5m","6m","7p","8p","9s","9s"}), {Pong({"5z","5z","5z"})}, 0},  // 副露听牌
      {H({"1m","2m","3m","4m","5m","6m","7p","8p","9s"}), {Pong({"5z","5z","5z"})}, 1},       // 副露一向听（面子计入）
      {H({"1m","2m","3m","4m","5m","6m","7p","3s","1z","5z"}), {Pong({"9s","9s","9s"})}, 2},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(Shanten(c.hand, c.melds), c.expect)
        << "hand size=" << c.hand.size() << " melds=" << c.melds.size();
  }
}

TEST(MahjongRules, ShantenHonorRules) {  // 28
  // 字牌不成顺：1z2z3z 不得被当作一组面子（若误当顺子会得到 0）
  const std::vector<Tile> no_run = H({"1z","2z","3z","5z","5z","1m","2m","3m","4m","5m","6m","7m","8m"});
  EXPECT_EQ(Shanten(no_run, {}), 2) << "字牌被误判成顺子时会得到 0";

  // 字牌刻子计入面子、字牌对子可作将
  EXPECT_EQ(Shanten(H({"1z","1z","1z","2z","2z","2z","3z","3z","3z","4z","4z","4z","5z"}), {}), 0);
  EXPECT_EQ(Shanten(H({"1z","1z","1z","2z","2z","2z","3z","3z","3z","4z","4z","4z","5z","5z"}), {}), -1);
  EXPECT_EQ(Shanten(H({"1m","2m","3m","4m","5m","6m","7m","8m","9m","5z","5z","1p","2p"}), {}), 0);
}

TEST(MahjongRules, ChooseDiscardPrefersIsolatedHonor) {  // 29
  // 孤字牌（1z/5z）是纯浮牌，应优先于数牌搭子（3p4p）被切掉：
  // 切孤字牌 → 打牌后摸一张可达听牌（0）；切搭子中的 3p → 只能到一向听（1）。
  std::vector<Tile> hand =
      H({"1m","2m","3m","4m","5m","6m","7p","8p","3p","4p","9s","9s","1z","5z"});
  Tile drawn = *ParseTile("5z");
  const Tile pick = *ChooseDiscard(hand, {}, drawn);
  EXPECT_TRUE(IsHonorFace(FaceOf(pick))) << FormatTile(pick);
  EXPECT_EQ(ScoreOf(hand, {}, FaceOf(pick)), 0);
  // 数牌候选（含搭子 3p4p 与搭子 7p8p）都要严格更差
  for (const char* n : {"3p", "4p", "7p", "8p"}) {
    EXPECT_GT(ScoreOf(hand, {}, FaceOf(*ParseTile(n))), 0) << n;
  }
  // 两个孤字牌并列（都是 0）⇒ 冻结牌序取最小牌面 1z
  EXPECT_EQ(FormatTile(pick), "1z");
}

TEST(MahjongRules, ChooseDiscardKeepsHonorPairAndTriplet) {  // 30
  // 字牌对子是**唯一将**：拆掉它必须严格更差（0 向听差 vs 听牌 -1）
  {
    std::vector<Tile> hand =
        H({"1m","2m","3m","4m","5m","6m","7m","8m","9m","5z","5z","1p","2p","9p"});
    Tile drawn = *ParseTile("9p");
    const Tile pick = *ChooseDiscard(hand, {}, drawn);
    EXPECT_FALSE(IsHonorFace(FaceOf(pick))) << "字牌对子被拆掉：" << FormatTile(pick);
    EXPECT_EQ(FormatTile(pick), "9p");                     // 唯一最优：切 9p 后听牌
    EXPECT_EQ(ScoreOf(hand, {}, FaceOf(pick)), -1);
    EXPECT_GT(ScoreOf(hand, {}, FaceOf(*ParseTile("5z"))), -1);  // 拆对子明显更差
  }
  // 字牌刻子可作面子：拆掉它同样严格更差
  {
    std::vector<Tile> hand =
        H({"5z","5z","5z","1m","2m","3m","4m","5m","6m","7p","8p","1s","9s","9s"});
    Tile drawn = *ParseTile("9s");
    const Tile pick = *ChooseDiscard(hand, {}, drawn);
    EXPECT_FALSE(IsHonorFace(FaceOf(pick))) << "字牌刻子被拆掉：" << FormatTile(pick);
    EXPECT_EQ(FormatTile(pick), "1s");                       // 唯一最优：切 1s 后听牌
    EXPECT_EQ(ScoreOf(hand, {}, FaceOf(pick)), -1);
    EXPECT_GT(ScoreOf(hand, {}, FaceOf(*ParseTile("5z"))), -1);  // 拆刻子更差
  }
}

TEST(MahjongRules, ChooseDiscardUsesBestAfterDrawMetric) {  // 31
  // 口径证明：评分 = 「打牌后从 34 面摸一张的最优向听」，而不是只看当前向听数。
  std::vector<Tile> hand =
      H({"3m","9m","5p","5p","8p","9p","9p","2s","7s","8s","1z","3z","3z","6z"});

  // (1) 不退化回「恒打刚摸到的牌」（DefaultDiscard 的旧行为）：打摸到的 9p 严格更差
  {
    Tile drawn = *ParseTile("9p");
    const Tile pick = *ChooseDiscard(hand, {}, drawn);
    EXPECT_EQ(FormatTile(pick), "3m");
    EXPECT_EQ(ScoreOf(hand, {}, FaceOf(pick)), 2);
    EXPECT_EQ(ScoreOf(hand, {}, FaceOf(drawn)), 3);           // 打摸到的牌严格更差
    EXPECT_LT(ScoreOf(hand, {}, FaceOf(pick)), ScoreOf(hand, {}, FaceOf(drawn)));
  }
  // (2) 与「只看向听数」的朴素口径给出**不同答案**：朴素口径并列时偏好刚摸到的 9m，
  //     本口径取「打牌后最优摸牌」评分的最小值，并列时按冻结牌序取最小牌面 3m。
  {
    Tile drawn = *ParseTile("9m");
    const Tile ours = *ChooseDiscard(hand, {}, drawn);
    const Tile naive = OnlyShantenPick(hand, drawn);
    EXPECT_NE(FormatTile(ours), FormatTile(naive));
    EXPECT_EQ(FormatTile(ours), "3m");
    EXPECT_EQ(FormatTile(naive), "9m");
    // 选定结果符合本口径：ours 是评分最小的候选之一，且并列中牌面最小
    const int ours_score = ScoreOf(hand, {}, FaceOf(ours));
    int min_score = 99;
    int min_score_min_face = 99;
    std::vector<Face> cands;
    for (Tile t : hand) {
      if (std::find(cands.begin(), cands.end(), FaceOf(t)) == cands.end()) cands.push_back(FaceOf(t));
    }
    std::sort(cands.begin(), cands.end());
    for (Face f : cands) min_score = std::min(min_score, ScoreOf(hand, {}, f));
    for (Face f : cands) {
      if (ScoreOf(hand, {}, f) == min_score) { min_score_min_face = f; break; }
    }
    EXPECT_EQ(ours_score, min_score);
    EXPECT_EQ(static_cast<int>(FaceOf(ours)), min_score_min_face);
    // 说明性断言：本口径的序与「打牌后向听数」的序一致（每个候选 best == immediate − 1），
    // 因此两者的差异体现在并列取舍上；若实现退化为忽略摸牌的口径，上面的选择会改变。
    for (Face f : cands) {
      const std::vector<Tile> rest = RemoveOne(hand, f);
      EXPECT_EQ(BestShantenAfterDraw(rest, {}), Shanten(rest, {}) - 1) << FormatTile(MakeTileFromFace(f));
    }
  }
}

TEST(MahjongRules, ChooseDiscardWithoutDrawRecord) {  // 32
  // 无摸牌记录（庄家第一打 / 碰吃后的待打牌）：可传 std::nullopt，且不改变选牌结果
  std::vector<Tile> opening =
      H({"1m","2m","3m","4m","5m","6m","7p","8p","3p","4p","9s","9s","1z","5z"});
  const auto no_draw = ChooseDiscard(opening, {}, std::nullopt);
  ASSERT_TRUE(no_draw.has_value());
  EXPECT_EQ(ScoreOf(opening, {}, FaceOf(*no_draw)),
            ScoreOf(opening, {}, FaceOf(*ChooseDiscard(opening, {}))));  // 2 参重载一致
  EXPECT_TRUE(IsHonorFace(FaceOf(*no_draw)));

  // 碰后待打牌（1 组副露 ⇒ 门前 11 张，行动态）：返回合法的、评分最优的牌
  const std::vector<Meld> melds = {Pong({"5z", "5z", "5z"})};
  std::vector<Tile> after_pong = H({"1m","2m","3m","4m","5m","6m","7p","8p","3p","4p","9s"});
  ASSERT_EQ(after_pong.size(), 11u);
  const auto pick = ChooseDiscard(after_pong, melds, std::nullopt);
  ASSERT_TRUE(pick.has_value());
  EXPECT_TRUE(std::find(after_pong.begin(), after_pong.end(), *pick) != after_pong.end());
  int best = 99;
  std::vector<Face> cands;
  for (Tile t : after_pong) {
    if (std::find(cands.begin(), cands.end(), FaceOf(t)) == cands.end()) cands.push_back(FaceOf(t));
  }
  for (Face f : cands) best = std::min(best, ScoreOf(after_pong, melds, f));
  EXPECT_EQ(ScoreOf(after_pong, melds, FaceOf(*pick)), best);

  // 空手牌 ⇒ nullopt
  EXPECT_FALSE(ChooseDiscard({}, {}, std::nullopt).has_value());
}

TEST(MahjongRules, ChooseDiscardDeterminism) {  // 33
  std::vector<Tile> hand =
      H({"3m","9m","5p","5p","8p","9p","9p","2s","7s","8s","1z","3z","3z","6z"});
  Tile drawn = *ParseTile("9m");
  const Tile a = *ChooseDiscard(hand, {}, drawn);
  const Tile b = *ChooseDiscard(hand, {}, drawn);
  EXPECT_EQ(FormatTile(a), FormatTile(b));  // 同一输入两次一致

  // 完全并列时取冻结牌序最小牌面：手中 5 个孤字牌评分相同（且都优于任何数牌候选）⇒ 取 1z
  std::vector<Tile> tie_hand =
      H({"1m","2m","3m","4m","5m","6m","7p","8p","9p","1z","3z","5z","6z","7z"});
  const Tile tie_pick = *ChooseDiscard(tie_hand, {}, *ParseTile("7z"));
  EXPECT_TRUE(IsHonorFace(FaceOf(tie_pick)));
  EXPECT_EQ(FormatTile(tie_pick), "1z");  // 最小字牌面（冻结牌序中数牌在前，但数牌候选更差）
  for (const char* n : {"1m", "9p"}) {
    EXPECT_GT(ScoreOf(tie_hand, {}, FaceOf(*ParseTile(n))), ScoreOf(tie_hand, {}, FaceOf(tie_pick)))
        << n;
  }

  // DefaultDiscard 语义未被改动（超时代打 / 人类托管的冻结回退）
  EXPECT_EQ(FormatTile(*DefaultDiscard(hand, drawn)), FormatTile(drawn));
  EXPECT_EQ(FormatTile(*DefaultDiscard(hand, std::nullopt)),
            FormatTile(SortedTiles(hand).back()));
}

TEST(MahjongRules, ChooseDiscardHonorTripletBeatsRunTemptation) {  // 34
  // 字牌永不参与顺子：把 1z2z3z 当顺子的实现会认为「切 5z」最优；正确实现应保留 5z 对子/刻子语义
  const std::vector<Tile> hand =
      H({"1z","2z","3z","5z","5z","1m","2m","3m","4m","5m","6m","7m","8m","9m"});
  const Tile pick = *ChooseDiscard(hand, {}, *ParseTile("9m"));
  EXPECT_TRUE(IsHonorFace(FaceOf(pick))) << FormatTile(pick);
  // 1z/2z/3z 三个孤字牌并列（评分相同）⇒ 冻结牌序取 1z
  EXPECT_EQ(FormatTile(pick), "1z");
  EXPECT_EQ(ScoreOf(hand, {}, FaceOf(pick)), Shanten(RemoveOne(hand, FaceOf(pick)), {}) - 1);
}

TEST(MahjongRules, ShantenAfterMeldHelper) {  // 35
  // hand 为鸣牌前门前手牌（等待态 13 张口径）
  const std::vector<Tile> hand =
      H({"1m","2m","3m","4m","5m","6m","7p","8p","9s","5z","5z","1p","2p"});
  // 碰 5z（消耗手中的两张 5z）
  const auto pong = ShantenAfterMeld(hand, {}, *ParseTile("5z"), H({"5z","5z"}), MeldType::kPong);
  ASSERT_TRUE(pong.has_value());
  EXPECT_GE(*pong, -1);
  // 吃：{1p,2p} 配 3p 成顺子
  const auto chow = ShantenAfterMeld(hand, {}, *ParseTile("3p"), H({"1p","2p"}), MeldType::kChow);
  ASSERT_TRUE(chow.has_value());
  // 非法组合一律 nullopt：consume 与 claimed 不同面 / 张数不符 / 无牌可吃 / 字牌吃
  EXPECT_FALSE(ShantenAfterMeld(hand, {}, *ParseTile("3p"), H({"9s","9s"}), MeldType::kPong).has_value());
  EXPECT_FALSE(ShantenAfterMeld(hand, {}, *ParseTile("5z"), H({"5z","5z","5z"}), MeldType::kPong).has_value());
  EXPECT_FALSE(ShantenAfterMeld(hand, {}, *ParseTile("3p"), H({"1p","2p","2p"}), MeldType::kChow).has_value());
  EXPECT_FALSE(ShantenAfterMeld(hand, {}, *ParseTile("5z"), H({"1z","2z"}), MeldType::kChow).has_value());
  EXPECT_FALSE(ShantenAfterMeld(hand, {}, *ParseTile("4p"), H({"1p","2p"}), MeldType::kChow).has_value());
  // 明杠：补摸按最优补牌（乐观口径）；此处手中正好 3 张 5z
  const std::vector<Tile> kong_hand =
      H({"1m","2m","3m","4m","5m","6m","7p","8p","9s","5z","5z","5z","1p"});
  const auto kong = ShantenAfterMeld(kong_hand, {}, *ParseTile("5z"), H({"5z","5z","5z"}), MeldType::kKong);
  ASSERT_TRUE(kong.has_value());
  EXPECT_GE(*kong, -1);
}

TEST(MahjongRules, ShantenMatchesTenpaiProperty) {  // 36
  // 独立性质校验（构造近和牌样本，不依赖手挑牌例）：
  //   - 构造的和牌形（14 张）⇒ Shanten == -1 且 CanWinExact == true；
  //   - 从和牌形去掉任意一张（13 张）⇒ 听牌：Shanten == 0 **且**存在可摸到的和牌张；
  //   - 对和牌形做一次单牌变异 ⇒ `Shanten == -1` ⟺ `CanWinExact == true`。
  // 构造时限制每个牌面 ≤ 3 张，保证被去掉的那张牌仍可摸回（避免「死听」干扰性质）。
  std::mt19937 rng(20261020);
  std::uniform_int_distribution<int> face_dist(0, kFaceCount - 1);
  std::uniform_int_distribution<int> run_dist(0, 6);
  std::uniform_int_distribution<int> suit_dist(0, 2);
  std::uniform_int_distribution<int> coin(0, 1);

  const auto counts_ok = [](const std::vector<Tile>& tiles, int limit) {
    for (uint8_t c : CountFaces(tiles)) {
      if (c > limit) return false;
    }
    return true;
  };

  int hands = 0, tenpai_samples = 0, mutated = 0;
  for (int iter = 0; iter < 800; ++iter) {
    // 构造一个合法和牌形（4 面子 + 将，或七对子），每面 ≤ 3 张
    std::vector<Tile> h14;
    if (coin(rng) == 0) {
      std::set<Face> faces;
      while (faces.size() < 7) faces.insert(static_cast<Face>(face_dist(rng)));
      for (Face f : faces) {
        h14.push_back(MakeTileFromFace(f));
        h14.push_back(MakeTileFromFace(f));
      }
    } else {
      for (int group = 0; group < 4; ++group) {
        if (coin(rng) == 0) {
          const Face f = static_cast<Face>(face_dist(rng));
          for (int k = 0; k < 3; ++k) h14.push_back(MakeTileFromFace(f));
        } else {
          const int start = suit_dist(rng) * kRankCount + run_dist(rng);
          for (int k = 0; k < 3; ++k) h14.push_back(MakeTileFromFace(static_cast<Face>(start + k)));
        }
      }
      const Face pair = static_cast<Face>(face_dist(rng));
      h14.push_back(MakeTileFromFace(pair));
      h14.push_back(MakeTileFromFace(pair));
    }
    if (!counts_ok(h14, 3)) continue;  // 保证「去掉一张后仍可摸回」
    ++hands;

    // 和牌形：-1 且判和
    EXPECT_EQ(Shanten(h14, {}), -1);
    EXPECT_TRUE(CanWinExact(h14, {}));

    // 去掉一张 ⇒ 听牌（0）且存在可摸到的和牌张
    for (int probe = 0; probe < 3; ++probe) {
      const size_t pos = rng() % h14.size();
      const Tile removed = h14[pos];
      std::vector<Tile> h13 = h14;
      h13.erase(h13.begin() + static_cast<long>(pos));
      EXPECT_EQ(Shanten(h13, {}), 0) << "去掉一张后必须是听牌";
      std::vector<Tile> back = h13;
      back.push_back(removed);
      EXPECT_TRUE(CanWinExact(back, {})) << "被去掉的那张必须能立刻和牌";
      ++tenpai_samples;
    }

    // 单牌变异：-1 ⟺ 和牌
    std::vector<Tile> mutated_hand = h14;
    mutated_hand[rng() % mutated_hand.size()] = MakeTileFromFace(static_cast<Face>(face_dist(rng)));
    if (!counts_ok(mutated_hand, kCopiesPerFace)) continue;
    EXPECT_EQ(Shanten(mutated_hand, {}) == -1, CanWinExact(mutated_hand, {}))
        << "和牌 ⟺ -1 性质不符";
    ++mutated;
  }

  std::cout << "[property] 和牌形样本=" << hands << "，听牌样本=" << tenpai_samples
            << "，变异样本=" << mutated << std::endl;
  EXPECT_GT(hands, 300);
  EXPECT_GT(tenpai_samples, 900);
  EXPECT_GT(mutated, 300);
}

TEST(MahjongRules, ChooseDiscardPerformanceAndDefaultUnchanged) {  // 36
  std::vector<Tile> hand =
      H({"1m","2m","3m","4m","5m","6m","7p","8p","3p","4p","9s","9s","1z","5z"});
  // 预热
  for (int i = 0; i < 10; ++i) (void)ChooseDiscard(hand, {}, *ParseTile("5z"));
  const int kIters = 200;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; ++i) (void)ChooseDiscard(hand, {}, *ParseTile("5z"));
  const auto end = std::chrono::steady_clock::now();
  const double per_call_us =
      std::chrono::duration<double, std::micro>(end - start).count() / kIters;
  std::cout << "[perf] ChooseDiscard: " << per_call_us << " us/次 ("
            << per_call_us / 1000.0 << " ms/次), 阈值 2000 us" << std::endl;
  EXPECT_LT(per_call_us, 2000.0) << "单次弃牌决策必须 < 2ms";
}

// ===========================================================================
// B1（规则库改造）：全拆法枚举 / 16 项番种 / 番数制算分 / 暗杠加杠 / 庄家与发牌
// 规格：game/docs/GAME_DESIGN_V2_DRAFT.md §1（番种表）§2（分数公式）§4（杠）§3.3（连庄）
// ===========================================================================
namespace {

// 番种表序（GAME_DESIGN_V2 §1.2）：**序 = 判定顺序 = 数组顺序**，冻结
const std::vector<std::string> kYakuOrder = {
    "menzen",  "tanyao", "pinfu",  "iipeikou", "yakuhai",
    "rinshan", "sanshoku_doujun", "ittsuu", "toitoi", "sanankou",
    "honitsu", "shousangen", "seven_pairs", "pure_one_suit",
};

// 断言：id 均已知、严格按冻结表序出现、同一 id 至多一条、han 与冻结番数表一致
void ExpectFrozenYakuOrder(const std::vector<Yaku>& yaku) {
  int last = -1;
  std::set<std::string> seen;
  for (const Yaku& item : yaku) {
    const auto it = std::find(kYakuOrder.begin(), kYakuOrder.end(), item.id);
    ASSERT_NE(it, kYakuOrder.end()) << "未知番种 id: " << item.id;
    const int index = static_cast<int>(it - kYakuOrder.begin());
    EXPECT_GT(index, last) << "番种顺序不是冻结表序: " << item.id;
    last = index;
    EXPECT_TRUE(seen.insert(item.id).second) << "同一 id 出现多次: " << item.id;
    if (item.id == "yakuhai") {
      EXPECT_GE(item.han, 1);
      EXPECT_LE(item.han, 3);
    } else {
      EXPECT_EQ(item.han, YakuHan(item.id)) << item.id;
    }
  }
}

// 「14 张手牌 / 副露 / 和牌张」的判定入口（默认自摸口径的统一封装）
std::vector<Yaku> YakuOf(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                         std::string_view winning = "") {
  Tile tile = concealed.empty() ? Tile{} : concealed.front();
  if (!winning.empty()) tile = *ParseTile(winning);
  return DetectYaku(concealed, melds, tile, Ctx(0, -1));
}

}  // namespace

// ---------------------------------------------------------------------------
// R1：全拆法枚举
// ---------------------------------------------------------------------------
TEST(MahjongRules, EnumerateDecompositionsReturnsAllSplits) {
  // 111m 222m 333m 555p 99s：同一手牌有**两种**拆法
  //   ① 111+222+333+555p（刻子分支先）
  //   ② 123+123+123+555p（顺子分支后）
  std::vector<Tile> hand =
      H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "5p", "5p", "5p", "9s", "9s"});
  ASSERT_TRUE(CanWinExact(hand, {}));
  const std::vector<Decomposition> decomps = EnumerateDecompositions(hand, {});
  ASSERT_EQ(decomps.size(), 2u);
  for (const Decomposition& d : decomps) {
    EXPECT_EQ(d.pair, FaceOf(*ParseTile("9s")));
  }
  // §1.3.4 ③：同一最小牌面下刻子分支先于顺子分支（= 既有 Decompose 的回溯序）
  EXPECT_FALSE(decomps[0].groups[0].is_run);
  EXPECT_TRUE(decomps[1].groups[0].is_run);
  EXPECT_EQ(decomps[0].groups[0].face, decomps[1].groups[0].face);

  // 和牌张落在哪一组：9s 在将牌上
  EXPECT_EQ(WinGroupIndex(decomps[0], FaceOf(*ParseTile("9s"))), kWinInPair);
  // 3m 在拆法①里是刻子组、在拆法②里是顺子组（顺子优先）
  EXPECT_TRUE(decomps[1].groups[WinGroupIndex(decomps[1], FaceOf(*ParseTile("3m")))].is_run);
  // 不含的牌面 ⇒ -1
  EXPECT_EQ(WinGroupIndex(decomps[0], FaceOf(*ParseTile("1z"))), -1);

  // 副露占 1 组：门槛张数随之变为 11
  const std::vector<Meld> pong = {Pong({"5p", "5p", "5p"})};
  std::vector<Tile> with_meld = H({"1m", "2m", "3m", "4m", "5m", "6m", "7s", "8s", "9s", "3p", "3p"});
  ASSERT_TRUE(CanWinExact(with_meld, pong));
  const std::vector<Decomposition> meld_decomps = EnumerateDecompositions(with_meld, pong);
  ASSERT_FALSE(meld_decomps.empty());
  EXPECT_TRUE(meld_decomps[0].groups[0].from_meld);
  EXPECT_EQ(meld_decomps[0].groups[0].meld_index, 0);

  // 张数不符 / 非和牌形 ⇒ 空（不制造异常）
  EXPECT_TRUE(EnumerateDecompositions(H({"1m", "2m", "3m"}), {}).empty());
  EXPECT_TRUE(EnumerateDecompositions(
                  H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s", "3s", "5s", "7p"}),
                  {})
                  .empty());
}

TEST(MahjongRules, EnumerateDecompositionsAgreesWithWinningJudge) {
  // 差分：全拆法枚举与早退版判定（CanWinExact → Decompose）对同一输入必须给出同一结论。
  // 早退版保留在判定热路径上（AI/鸣牌窗口），全量版只在和牌时判番用一次，两者不得漂移。
  std::mt19937 rng(20261101);
  std::uniform_int_distribution<int> face_dist(0, kFaceCount - 1);
  std::uniform_int_distribution<int> run_dist(0, 6);
  std::uniform_int_distribution<int> suit_dist(0, 2);
  std::uniform_int_distribution<int> coin(0, 1);
  int constructed = 0;
  int agree = 0;
  for (int iter = 0; iter < 1200; ++iter) {
    std::vector<Tile> hand;
    if (coin(rng) == 0) {  // 七对子
      std::set<int> faces;
      while (faces.size() < 7) faces.insert(face_dist(rng));
      for (int f : faces) {
        hand.push_back(MakeTileFromFace(static_cast<Face>(f)));
        hand.push_back(MakeTileFromFace(static_cast<Face>(f)));
      }
    } else {  // 4 面子 + 将
      for (int group = 0; group < 4; ++group) {
        if (coin(rng) == 0) {
          const Face face = static_cast<Face>(face_dist(rng));
          for (int k = 0; k < 3; ++k) hand.push_back(MakeTileFromFace(face));
        } else {
          const int start = suit_dist(rng) * kRankCount + run_dist(rng);
          for (int k = 0; k < 3; ++k) hand.push_back(MakeTileFromFace(static_cast<Face>(start + k)));
        }
      }
      const Face pair = static_cast<Face>(face_dist(rng));
      hand.push_back(MakeTileFromFace(pair));
      hand.push_back(MakeTileFromFace(pair));
    }
    if (!IsVerifiedHand(hand)) continue;
    ++constructed;
    // 判定契约（与 CanWinExact 的实现一致）：和牌 ⇔ 有标准型拆法 **或**（七对子）。
    // 七对子没有「4 面子 + 1 将」拆法 ⇒ 枚举为空但判和为真 —— 判番走独立分支，两条路径依然同源。
    ASSERT_TRUE(CanWinExact(hand, {})) << "构造出的手牌必须能和：" << FormatHand(hand);
    EXPECT_EQ(CanWinExact(hand, {}),
              !EnumerateDecompositions(hand, {}).empty() || IsSevenPairs(hand))
        << "全拆法枚举与和牌判定必须同源：" << FormatHand(hand);
    ++agree;
    // 变异一手牌：枚举结果与判定必须一致（标准型和 ⇔ 至少一个拆法）
    std::vector<Tile> mutated = hand;
    mutated[rng() % mutated.size()] = MakeTileFromFace(static_cast<Face>(face_dist(rng)));
    if (!IsVerifiedHand(mutated)) continue;
    EXPECT_EQ(CanWinExact(mutated, {}),
              !EnumerateDecompositions(mutated, {}).empty() || IsSevenPairs(mutated))
        << FormatHand(mutated);
  }
  EXPECT_GT(constructed, 1000);
  EXPECT_EQ(constructed, agree);
  // 七对子没有标准型拆法 ⇒ 枚举为空，但判定为和（判番走独立分支）
  std::vector<Tile> seven =
      H({"1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p", "6p", "8p", "8p"});
  EXPECT_TRUE(EnumerateDecompositions(seven, {}).empty());
  EXPECT_TRUE(CanWinExact(seven, {}));
}

// ---------------------------------------------------------------------------
// R3：番 → 倍率（§2.2 的 7 行表，逐项命中）
// ---------------------------------------------------------------------------
TEST(MahjongRules, HanToMultiplierTable) {
  const int expected[] = {1, 2, 4, 8, 16, 32, 64};
  for (int han = 0; han <= 6; ++han) {
    EXPECT_EQ(MultiplierForHan(han), expected[han]) << "han=" << han;
  }
  // 封顶 6 番：7 番及以上一律 64
  EXPECT_EQ(MultiplierForHan(7), 64);
  EXPECT_EQ(MultiplierForHan(99), 64);
  EXPECT_EQ(MultiplierForHan(-1), 1);
}

TEST(MahjongRules, ScoreHandCoversEveryHanRow) {
  // 用真实手牌逐行命中 han 0..6 ⇒ R = 1,2,4,8,16,32,64（§2.2）
  struct Row {
    std::vector<Tile> hand;
    std::vector<Meld> melds;
    std::string winning;
    int han;
    int multiplier;
    std::string label;
  };
  const std::vector<Row> rows = {
      // 0 番：副露垃圾手（有碰、含幺九、跨三门 ⇒ 无任何番种）
      {H({"1m", "2m", "3m", "4p", "5p", "6p", "7p", "8p", "9p", "2m", "2m"}),
       {Pong({"9s", "9s", "9s"})}, "2m", 0, 1, "0 番 副露垃圾手"},
      // 1 番：只有门清（将牌是字牌 ⇒ 平和不成立）
      {H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s", "3s", "1z", "1z"}),
       {},
       "1z",
       1,
       2,
       "1 番 门清"},
      // 2 番：menzen + tanyao（有刻子 ⇒ 无平和不成立）
      {H({"2m", "2m", "2m", "3p", "4p", "5p", "6s", "7s", "8s", "5m", "6m", "7m", "5s", "5s"}),
       {},
       "5s",
       2,
       4,
       "2 番 门清+断幺九"},
      // 3 番：menzen + pinfu + tanyao
      {H({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "6p", "7p", "8p", "5s", "5s"}),
       {},
       "5s",
       3,
       8,
       "3 番 门清+平和+断幺九"},
      // 4 番：menzen + sanshoku_doujun + yakuhai(中)
      {H({"2m", "3m", "4m", "2p", "3p", "4p", "2s", "3s", "4s", "7z", "7z", "7z", "9m", "9m"}),
       {},
       "9m",
       4,
       16,
       "4 番 三色同顺+箭刻"},
      // 5 番：menzen + ittsuu + honitsu
      {H({"1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "2m", "2m", "2m", "1z", "1z"}),
       {},
       "1z",
       5,
       32,
       "5 番 一气通贯+混一色"},
      // 6 番（封顶）：menzen + sanankou + pure_one_suit 本为 8 番 ⇒ 封顶 6
      {H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m", "6m", "7m", "7m"}),
       {},
       "7m",
       6,
       64,
       "6 番 清一色（封顶）"},
  };
  for (const Row& row : rows) {
    ASSERT_TRUE(CanWinExact(row.hand, row.melds)) << row.label;
    const ScoreContext ctx = Ctx(1, 3);
    const ScoreResult score =
        ScoreHand(row.hand, row.melds, *ParseTile(row.winning), ctx);
    ExpectFrozenYakuOrder(score.yaku);
    EXPECT_EQ(ExpectScoreConsistent(score, ctx), row.han) << row.label;
    EXPECT_EQ(score.multiplier, row.multiplier) << row.label;
    EXPECT_EQ(score.delta[3], -row.multiplier) << row.label;  // 放炮者
    EXPECT_EQ(score.delta[1], row.multiplier) << row.label;   // 和牌者
    EXPECT_EQ(score.delta[0], 0) << row.label;
    EXPECT_EQ(score.delta[2], 0) << row.label;
  }
}

// ---------------------------------------------------------------------------
// R2：14 个番种（每个：正例 + 最容易误判的反例）
// ---------------------------------------------------------------------------
TEST(MahjongRules, YakuMenzen) {  // #1 门前清：ankan 不破门清，pong/chow/kong/kakan 破
  std::vector<Tile> closed =
      H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s", "3s", "1z", "1z"});
  EXPECT_TRUE(HasYakuId(YakuOf(closed, {}), "menzen"));

  // ankan 不破门清（§1.2 #1 / §4.2）
  const std::vector<Meld> ankan = {Ankan({"9s", "9s", "9s", "9s"})};
  std::vector<Tile> with_ankan =
      H({"1m", "2m", "3m", "4p", "5p", "6p", "1s", "2s", "3s", "2m", "2m"});
  ASSERT_TRUE(CanWinExact(with_ankan, ankan));
  EXPECT_TRUE(HasYakuId(YakuOf(with_ankan, ankan), "menzen")) << "暗杠不得破门清";

  // pong / chow / kong / kakan 四种都破门清
  std::vector<Tile> tail = H({"1m", "2m", "3m", "4p", "5p", "6p", "1s", "2s", "3s", "2m", "2m"});
  const std::vector<Meld> pong = {Pong({"5z", "5z", "5z"})};
  EXPECT_FALSE(HasYakuId(YakuOf(tail, pong), "menzen"));
  const std::vector<Meld> chow = {Meld{.type = MeldType::kChow, .tiles = H({"7m", "8m", "9m"})}};
  EXPECT_FALSE(HasYakuId(YakuOf(tail, chow), "menzen"));
  const std::vector<Meld> kong = {Kong({"5z", "5z", "5z", "5z"})};
  EXPECT_FALSE(HasYakuId(YakuOf(tail, kong), "menzen"));
  const std::vector<Meld> kakan = {Kakan({"5z", "5z", "5z", "5z"})};
  EXPECT_FALSE(HasYakuId(YakuOf(tail, kakan), "menzen"));
}

TEST(MahjongRules, YakuTanyao) {  // #2 断幺九
  std::vector<Tile> all_simple =
      H({"2m", "2m", "2m", "3p", "4p", "5p", "6s", "7s", "8s", "5m", "6m", "7m", "5s", "5s"});
  const std::vector<Yaku> positive = YakuOf(all_simple, {});
  EXPECT_EQ(HanOfYaku(positive, "tanyao"), 1);

  // 反例 1：含老头牌（1m）
  std::vector<Tile> with_terminal =
      H({"1m", "2m", "3m", "4p", "5p", "6p", "6s", "7s", "8s", "5m", "6m", "7m", "5s", "5s"});
  EXPECT_FALSE(HasYakuId(YakuOf(with_terminal, {}), "tanyao"));
  // 反例 2：含字牌
  std::vector<Tile> with_honor =
      H({"1z", "1z", "1z", "4p", "5p", "6p", "6s", "7s", "8s", "5m", "6m", "7m", "5s", "5s"});
  EXPECT_FALSE(HasYakuId(YakuOf(with_honor, {}), "tanyao"));
  // 反例 3：副露里含幺九（`all` 是全集，不只是门前手牌）
  const std::vector<Meld> pong = {Pong({"1s", "1s", "1s"})};
  std::vector<Tile> concealed =
      H({"2m", "2m", "2m", "3p", "4p", "5p", "5m", "6m", "7m", "5s", "5s"});
  ASSERT_TRUE(CanWinExact(concealed, pong));
  EXPECT_FALSE(HasYakuId(YakuOf(concealed, pong), "tanyao"));
}

TEST(MahjongRules, YakuPinfu) {  // #3 平和：门清 + 4 顺子 + 数牌将
  std::vector<Tile> pinfu =
      H({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "6p", "7p", "8p", "5s", "5s"});
  EXPECT_EQ(HanOfYaku(YakuOf(pinfu, {}), "pinfu"), 1);

  // 反例 1：将牌是字牌
  std::vector<Tile> honor_pair =
      H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s", "2s", "3s", "1z", "1z"});
  EXPECT_FALSE(HasYakuId(YakuOf(honor_pair, {}), "pinfu"));
  // 反例 2：含刻子
  std::vector<Tile> triplet =
      H({"2m", "2m", "2m", "3p", "4p", "5p", "6s", "7s", "8s", "5m", "6m", "7m", "5s", "5s"});
  EXPECT_FALSE(HasYakuId(YakuOf(triplet, {}), "pinfu"));
  // 反例 3：有副露（平和是门清限定）
  const std::vector<Meld> pong = {Pong({"9s", "9s", "9s"})};
  std::vector<Tile> open = H({"2m", "3m", "4m", "5p", "6p", "7p", "2s", "3s", "4s", "5m", "5m"});
  ASSERT_TRUE(CanWinExact(open, pong));
  EXPECT_FALSE(HasYakuId(YakuOf(open, pong), "pinfu"));
}

TEST(MahjongRules, YakuIipeikou) {  // #4 一杯口：门清 + 两组同花色同起点顺子
  std::vector<Tile> iipeikou =
      H({"2m", "3m", "4m", "2m", "3m", "4m", "6p", "7p", "8p", "3s", "4s", "5s", "9s", "9s"});
  EXPECT_EQ(HanOfYaku(YakuOf(iipeikou, {}), "iipeikou"), 1);

  // 反例 1：两组顺子起点不同（2m3m4m vs 3m4m5m）
  std::vector<Tile> near_miss =
      H({"2m", "3m", "4m", "3m", "4m", "5m", "6p", "7p", "8p", "3s", "4s", "5s", "9s", "9s"});
  EXPECT_FALSE(HasYakuId(YakuOf(near_miss, {}), "iipeikou"));
  // 反例 2：同起点但花色不同（2m3m4m vs 2p3p4p —— 那是三色的形状，不是一杯口）
  std::vector<Tile> cross_suit =
      H({"2m", "3m", "4m", "2p", "3p", "4p", "6s", "7s", "8s", "5m", "6m", "7m", "9s", "9s"});
  EXPECT_FALSE(HasYakuId(YakuOf(cross_suit, {}), "iipeikou"));
  // 反例 3：有副露（一杯口是门清限定）
  const std::vector<Meld> pong = {Pong({"9p", "9p", "9p"})};
  std::vector<Tile> open =
      H({"2m", "3m", "4m", "2m", "3m", "4m", "6p", "7p", "8p", "3s", "3s"});
  ASSERT_TRUE(CanWinExact(open, pong));
  EXPECT_FALSE(HasYakuId(YakuOf(open, pong), "iipeikou"));
}

TEST(MahjongRules, YakuYakuhai) {  // #5 箭刻：多条合并为一条，han = 组数
  std::vector<Tile> one_dragon =
      H({"5z", "5z", "5z", "2m", "2m", "2m", "3p", "4p", "5p", "6s", "7s", "8s", "9m", "9m"});
  const std::vector<Yaku> single = YakuOf(one_dragon, {});
  EXPECT_EQ(HanOfYaku(single, "yakuhai"), 1);
  EXPECT_EQ(std::count_if(single.begin(), single.end(),
                          [](const Yaku& y) { return y.id == "yakuhai"; }),
            1) << "yakuhai 只能出现一条";

  // 两组白/发 ⇒ 仍是一条，han = 2；三组 ⇒ han = 3
  std::vector<Tile> two_dragons =
      H({"5z", "5z", "5z", "6z", "6z", "6z", "2m", "3m", "4m", "5p", "6p", "7p", "9s", "9s"});
  const std::vector<Yaku> two = YakuOf(two_dragons, {});
  EXPECT_EQ(HanOfYaku(two, "yakuhai"), 2);
  EXPECT_EQ(std::count_if(two.begin(), two.end(),
                          [](const Yaku& y) { return y.id == "yakuhai"; }),
            1);
  std::vector<Tile> three_dragons =
      H({"5z", "5z", "5z", "6z", "6z", "6z", "7z", "7z", "7z", "1m", "2m", "3m", "9s", "9s"});
  EXPECT_EQ(HanOfYaku(YakuOf(three_dragons, {}), "yakuhai"), 3);

  // 反例 1：风牌（1z 东）不是箭刻
  std::vector<Tile> wind =
      H({"1z", "1z", "1z", "2m", "2m", "2m", "3p", "4p", "5p", "6s", "7s", "8s", "9m", "9m"});
  EXPECT_FALSE(HasYakuId(YakuOf(wind, {}), "yakuhai"));
  // 反例 2：箭牌的**对子**（不是刻子/杠）不算
  std::vector<Tile> dragon_pair =
      H({"5z", "5z", "2m", "2m", "2m", "3p", "4p", "5p", "6s", "7s", "8s", "1m", "2m", "3m"});
  EXPECT_FALSE(HasYakuId(YakuOf(dragon_pair, {}), "yakuhai"));
  // 箭牌暗杠同样计入箭刻（刻子或杠）
  const std::vector<Meld> dragon_kan = {Ankan({"7z", "7z", "7z", "7z"})};
  std::vector<Tile> with_kan =
      H({"1m", "2m", "3m", "4p", "5p", "6p", "1s", "2s", "3s", "2m", "2m"});
  ASSERT_TRUE(CanWinExact(with_kan, dragon_kan));
  EXPECT_EQ(HanOfYaku(YakuOf(with_kan, dragon_kan), "yakuhai"), 1);
}

TEST(MahjongRules, YakuRinshan) {  // #6 杠上开花：自摸 且 和牌张是本次杠的补牌
  std::vector<Tile> hand =
      H({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "6p", "7p", "8p", "5s", "5s"});
  const ScoreContext rinshan_tsumo = Ctx(0, -1, /*dealer=*/false, /*rinshan=*/true);
  const std::vector<Yaku> yaku = DetectYaku(hand, {}, *ParseTile("5s"), rinshan_tsumo);
  EXPECT_EQ(HanOfYaku(yaku, "rinshan"), 1);
  ExpectFrozenYakuOrder(yaku);
  // 自摸不是番种（B1.1）：杠上开花 1 番照常计入合计；"自摸 ×2" 只体现在支付公式 2R/6R
  const ScoreResult score = ScoreHand(hand, {}, *ParseTile("5s"), rinshan_tsumo);
  EXPECT_EQ(score.han, YakuHanTotal(score.yaku));
  EXPECT_FALSE(HasYakuId(score.yaku, "tsumo")) << "自摸不得写进 yaku[]";

  // 反例 1：荣和永不成立（荣和没有「本次补牌」）
  const std::vector<Yaku> ron = DetectYaku(hand, {}, *ParseTile("5s"), Ctx(0, 2, false, true));
  EXPECT_FALSE(HasYakuId(ron, "rinshan"));
  // 反例 2：自摸但不是杠的补牌
  EXPECT_FALSE(HasYakuId(DetectYaku(hand, {}, *ParseTile("5s"), Ctx(0, -1)), "rinshan"));
}

TEST(MahjongRules, YakuSanshokuDoujun) {  // #7 三色同顺
  std::vector<Tile> sanshoku =
      H({"2m", "3m", "4m", "2p", "3p", "4p", "2s", "3s", "4s", "7z", "7z", "7z", "9m", "9m"});
  EXPECT_EQ(HanOfYaku(YakuOf(sanshoku, {}), "sanshoku_doujun"), 2);

  // 反例 1：三门起点不同（2m vs 3p vs 2s）
  std::vector<Tile> diff_start =
      H({"2m", "3m", "4m", "3p", "4p", "5p", "2s", "3s", "4s", "7z", "7z", "7z", "9m", "9m"});
  EXPECT_FALSE(HasYakuId(YakuOf(diff_start, {}), "sanshoku_doujun"));
  // 反例 2：只有两门（m + p）
  std::vector<Tile> two_suits =
      H({"2m", "3m", "4m", "2p", "3p", "4p", "6s", "7s", "8s", "9m", "9m", "2m", "3m", "4m"});
  EXPECT_FALSE(HasYakuId(YakuOf(two_suits, {}), "sanshoku_doujun"));
  // 副露的**吃**也算一组顺子（三色不要求门清）
  const std::vector<Meld> chow = {Meld{.type = MeldType::kChow, .tiles = H({"2p", "3p", "4p"})}};
  std::vector<Tile> with_chow =
      H({"2m", "3m", "4m", "2s", "3s", "4s", "9m", "9m", "1z", "1z", "1z"});
  ASSERT_TRUE(CanWinExact(with_chow, chow));
  EXPECT_EQ(HanOfYaku(YakuOf(with_chow, chow), "sanshoku_doujun"), 2);
}

TEST(MahjongRules, YakuIttsuu) {  // #8 一气通贯
  std::vector<Tile> ittsuu =
      H({"1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "2p", "2p", "2p", "1z", "1z"});
  EXPECT_EQ(HanOfYaku(YakuOf(ittsuu, {}), "ittsuu"), 2);

  // 反例 1：缺一段（m 门只有 123 + 456，第三段在 p 门）
  std::vector<Tile> broken =
      H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "2p", "3p", "4p", "1z", "1z"});
  ASSERT_TRUE(CanWinExact(broken, {}));
  EXPECT_FALSE(HasYakuId(YakuOf(broken, {}), "ittsuu"));
  // 反例 2：三段都在但跨花色（123m + 456m + 789p）
  std::vector<Tile> cross =
      H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "2s", "3s", "4s", "9m", "9m"});
  ASSERT_TRUE(CanWinExact(cross, {}));
  EXPECT_FALSE(HasYakuId(YakuOf(cross, {}), "ittsuu")) << "一气通贯要求同一门";
}

TEST(MahjongRules, YakuToitoi) {  // #9 对对和：4 组全为刻子或杠
  std::vector<Tile> toitoi =
      H({"1m", "1m", "1m", "5p", "5p", "5p", "9s", "9s", "9s", "2z", "2z", "2z", "4z", "4z"});
  const std::vector<Yaku> yaku = YakuOf(toitoi, {});
  EXPECT_EQ(HanOfYaku(yaku, "toitoi"), 2);
  // 表序：menzen(1) 在 toitoi(9) 之前、sanankou(10) 在 toitoi 之后
  EXPECT_EQ(Ids(yaku), (std::vector<std::string>{"menzen", "toitoi", "sanankou"}));

  // 反例 1：含一组顺子
  std::vector<Tile> with_run =
      H({"2m", "3m", "4m", "5p", "5p", "5p", "9s", "9s", "9s", "2z", "2z", "2z", "4z", "4z"});
  EXPECT_FALSE(HasYakuId(YakuOf(with_run, {}), "toitoi"));
  // 副露的碰/杠同样计入（对对和不要求门清）
  const std::vector<Meld> melds = {Pong({"5p", "5p", "5p"}), Kong({"9s", "9s", "9s", "9s"})};
  std::vector<Tile> mixed = H({"1m", "1m", "1m", "2z", "2z", "2z", "4z", "4z"});
  ASSERT_TRUE(CanWinExact(mixed, melds));
  EXPECT_EQ(HanOfYaku(YakuOf(mixed, melds), "toitoi"), 2);
}

TEST(MahjongRules, YakuSanankou) {  // #10 三暗刻（含荣和排除和牌张补成的刻子）
  // 三组暗刻（111m / 555p / 999s）+ 一组顺子 + 将
  std::vector<Tile> base =
      H({"1m", "1m", "1m", "5p", "5p", "5p", "9s", "9s", "9s", "2m", "3m", "4m", "4z", "4z"});
  ASSERT_TRUE(CanWinExact(base, {}));
  // 自摸：和牌张（9s）补成的刻子仍是暗刻 ⇒ 三暗刻成立
  const std::vector<Yaku> tsumo = DetectYaku(base, {}, *ParseTile("9s"), Ctx(0, -1));
  EXPECT_EQ(HanOfYaku(tsumo, "sanankou"), 2);
  // 荣和：由和牌张补成的 999s 不计暗刻 ⇒ 只剩 2 组 ⇒ 不成立（验收项 A9）
  const std::vector<Yaku> ron = DetectYaku(base, {}, *ParseTile("9s"), Ctx(0, 2));
  EXPECT_FALSE(HasYakuId(ron, "sanankou")) << "荣和时和牌张补成的刻子不得计入三暗刻";
  // 同一手牌改由「顺子里的牌」和牌 ⇒ 三组暗刻完好 ⇒ 仍成立（落组口径：能和成顺子就按顺子算）
  const std::vector<Yaku> ron_run = DetectYaku(base, {}, *ParseTile("2m"), Ctx(0, 2));
  EXPECT_EQ(HanOfYaku(ron_run, "sanankou"), 2);

  // 反例 1：只有两组暗刻（第三组是顺子）且荣和
  std::vector<Tile> two_ankou =
      H({"1m", "1m", "1m", "5p", "5p", "5p", "2m", "3m", "4m", "6s", "7s", "8s", "4z", "4z"});
  EXPECT_FALSE(HasYakuId(DetectYaku(two_ankou, {}, *ParseTile("4z"), Ctx(0, 2)), "sanankou"));
  // 反例 2：暗杠计入暗刻、明碰不计
  const std::vector<Meld> ankan = {Ankan({"9s", "9s", "9s", "9s"})};
  std::vector<Tile> with_ankan =
      H({"1m", "1m", "1m", "5p", "5p", "5p", "2m", "3m", "4m", "4z", "4z"});
  ASSERT_TRUE(CanWinExact(with_ankan, ankan));
  EXPECT_EQ(HanOfYaku(YakuOf(with_ankan, ankan), "sanankou"), 2);  // 111m + 555p + 暗杠 9s
  const std::vector<Meld> pongs = {Pong({"9s", "9s", "9s"}), Pong({"5p", "5p", "5p"})};
  std::vector<Tile> with_pongs =
      H({"1m", "1m", "1m", "2m", "3m", "4m", "4z", "4z"});
  ASSERT_TRUE(CanWinExact(with_pongs, pongs));
  EXPECT_FALSE(HasYakuId(YakuOf(with_pongs, pongs), "sanankou")) << "明碰不是暗刻";
}

TEST(MahjongRules, YakuHonitsu) {  // #11 混一色：恰含一门数牌 + 至少 1 张字牌
  std::vector<Tile> honitsu =
      H({"1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "2m", "2m", "2m", "1z", "1z"});
  const std::vector<Yaku> yaku = YakuOf(honitsu, {});
  EXPECT_EQ(HanOfYaku(yaku, "honitsu"), 2);
  EXPECT_FALSE(HasYakuId(yaku, "pure_one_suit")) << "有字牌 ⇒ 不是清一色";
  ExpectFrozenYakuOrder(yaku);

  // 反例 1：全字牌（不是「恰含一门数牌」⇒ 也不成立）
  std::vector<Tile> all_honor =
      H({"1z", "1z", "1z", "2z", "2z", "2z", "3z", "3z", "3z", "4z", "4z", "4z", "5z", "5z"});
  EXPECT_FALSE(HasYakuId(YakuOf(all_honor, {}), "honitsu")) << "全字牌不得判混一色";
  // 反例 2：两门数牌 + 字牌
  std::vector<Tile> two_suits =
      H({"1m", "2m", "3m", "4p", "5p", "6p", "7m", "8m", "9m", "2m", "2m", "2m", "1z", "1z"});
  EXPECT_FALSE(HasYakuId(YakuOf(two_suits, {}), "honitsu"));
  // 反例 3：单门数牌但零字牌 ⇒ 是清一色而不是混一色（结构互斥，无需互斥分支）
  std::vector<Tile> pure_only =
      H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m", "6m", "7m", "7m"});
  EXPECT_FALSE(HasYakuId(YakuOf(pure_only, {}), "honitsu"));
}

TEST(MahjongRules, YakuShousangen) {  // #12 小三元：两组箭刻 + 第三组箭牌将
  std::vector<Tile> shousangen =
      H({"5z", "5z", "5z", "6z", "6z", "6z", "7z", "7z", "1m", "2m", "3m", "4p", "5p", "6p"});
  const std::vector<Yaku> yaku = YakuOf(shousangen, {});
  EXPECT_EQ(HanOfYaku(yaku, "shousangen"), 2);
  EXPECT_EQ(HanOfYaku(yaku, "yakuhai"), 2) << "小三元与箭刻叠加（§1.2 #12 可叠）";

  // 反例 1：三组都是箭刻（第三组不是将牌）
  std::vector<Tile> three_triplets =
      H({"5z", "5z", "5z", "6z", "6z", "6z", "7z", "7z", "7z", "1m", "2m", "3m", "9p", "9p"});
  EXPECT_FALSE(HasYakuId(YakuOf(three_triplets, {}), "shousangen"));
  // 反例 2：只有一组箭刻 + 一对箭牌
  std::vector<Tile> one_triplet =
      H({"5z", "5z", "5z", "6z", "6z", "1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "9s"});
  EXPECT_FALSE(HasYakuId(YakuOf(one_triplet, {}), "shousangen"));
}

TEST(MahjongRules, YakuSevenPairs) {  // #13 七对子：门清 + 14 张为 7 个不同牌面各 2 张
  std::vector<Tile> seven =
      H({"1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p", "6p", "8p", "8p"});
  const std::vector<Yaku> yaku = YakuOf(seven, {});
  EXPECT_EQ(HanOfYaku(yaku, "seven_pairs"), 2);
  EXPECT_EQ(HanOfYaku(yaku, "menzen"), 1);
  ExpectFrozenYakuOrder(yaku);

  // 反例 1：4 张同面不算两对
  std::vector<Tile> quad =
      H({"1m", "1m", "1m", "1m", "2p", "2p", "3p", "3p", "4p", "4p", "5p", "5p", "6p", "6p"});
  EXPECT_FALSE(IsSevenPairs(quad));
  EXPECT_FALSE(HasYakuId(YakuOf(quad, {}), "seven_pairs"));
  // 反例 2：有副露（七对子是门清限定，结构必然）
  const std::vector<Meld> pong = {Pong({"9s", "9s", "9s"})};
  std::vector<Tile> open =
      H({"1m", "1m", "3m", "3m", "5m", "5m", "2p", "2p", "6p", "6p", "8p"});
  ASSERT_EQ(open.size(), 11u);
  EXPECT_FALSE(HasYakuId(YakuOf(open, pong), "seven_pairs"));
}

TEST(MahjongRules, YakuPureOneSuit) {  // #14 清一色：全同一门数牌、零字牌
  std::vector<Tile> pure =
      H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m", "6m", "7m", "7m"});
  EXPECT_EQ(HanOfYaku(YakuOf(pure, {}), "pure_one_suit"), 5);

  // 反例 1：混入 1 张字牌 ⇒ 跌成混一色
  std::vector<Tile> with_honor =
      H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m", "6m", "1z", "1z"});
  const std::vector<Yaku> mixed = YakuOf(with_honor, {});
  EXPECT_FALSE(HasYakuId(mixed, "pure_one_suit"));
  EXPECT_TRUE(HasYakuId(mixed, "honitsu"));
  // 反例 2：两门数牌 + 字牌
  std::vector<Tile> two_suits =
      H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4p", "5p", "6p", "1z", "1z"});
  EXPECT_FALSE(HasYakuId(YakuOf(two_suits, {}), "pure_one_suit"));
}

TEST(MahjongRules, BonusTsumoAndDealerBonus) {  // 2 个加成：自摸 +1、庄家 +1（都不是番种）
  std::vector<Tile> hand =
      H({"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "6p", "7p", "8p", "5s", "5s"});
  const int base_han = YakuHanTotal(YakuOf(hand, {}));
  ASSERT_EQ(base_han, 3);  // menzen + pinfu + tanyao

  // 自摸（B1.1 裁决）：不是番种 ⇒ han 与荣和相同（3 番），差别只在支付公式 2R/6R
  const ScoreResult tsumo = ScoreHand(hand, {}, *ParseTile("5s"), Ctx(2, -1));
  EXPECT_EQ(tsumo.han, 3);
  EXPECT_EQ(tsumo.yaku.size(), 3u);
  EXPECT_EQ(tsumo.dealer_bonus, 0);
  EXPECT_FALSE(HasYakuId(tsumo.yaku, "tsumo"));
  EXPECT_EQ(tsumo.per_payer, 16);  // 2R = 2*8
  EXPECT_EQ(tsumo.total, 48);      // 6R

  // 庄家：+1 番，单独字段，不进 yaku[]
  const ScoreResult dealer = ScoreHand(hand, {}, *ParseTile("5s"), Ctx(2, 1, /*dealer=*/true));
  EXPECT_EQ(dealer.han, 4);
  EXPECT_EQ(dealer.dealer_bonus, 1);
  EXPECT_FALSE(HasYakuId(dealer.yaku, "dealer"));
  EXPECT_EQ(dealer.multiplier, 16);
  // 庄家加成与自摸可叠加（自摸不影响 han），且受**同一**封顶
  const ScoreResult both = ScoreHand(hand, {}, *ParseTile("5s"), Ctx(2, -1, true));
  EXPECT_EQ(both.han, 4);
  EXPECT_EQ(both.multiplier, 16);
  EXPECT_EQ(both.per_payer, 32);
  EXPECT_EQ(both.total, 96);
  // 封顶对庄家加成同样生效：6 番手牌 + 庄家 ⇒ 仍 6 番
  std::vector<Tile> capped =
      H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m", "6m", "7m", "7m"});
  const ScoreResult capped_dealer = ScoreHand(capped, {}, *ParseTile("7m"), Ctx(0, 1, true));
  EXPECT_EQ(capped_dealer.han, 6) << "庄家加成必须受同一封顶";
  EXPECT_EQ(capped_dealer.multiplier, 64);
}

// ---------------------------------------------------------------------------
// R3：零和 / 回归锚点 / 0 番
// ---------------------------------------------------------------------------
// N3（A1_ACCEPTANCE §七 / RULESET §10.3 S3）：**0 番自摸恰好 −2/+6** 的绝对值锚——
// 不是关系式（total == 3×per_payer），是逐字段冻结数字：R=1 ⇒ 三家各 −2、和牌者 +6。
TEST(MahjongRules, ZeroHanTsumoIsExactlyMinusTwoPlusSix) {
  const std::vector<Meld> pong = {Pong({"9s", "9s", "9s"})};
  std::vector<Tile> zero_han = H({"1m", "2m", "3m", "4p", "5p", "6p", "7p", "8p", "9p", "2m", "2m"});
  ASSERT_TRUE(CanWinExact(zero_han, pong));
  const ScoreResult tsumo = ScoreHand(zero_han, pong, *ParseTile("2m"), Ctx(2, -1));
  EXPECT_TRUE(tsumo.yaku.empty());
  EXPECT_EQ(tsumo.han, 0);
  EXPECT_EQ(tsumo.multiplier, 1);  // R = 2^0 = 1（绝对值锚的根）
  EXPECT_EQ(tsumo.per_payer, 2);
  EXPECT_EQ(tsumo.total, 6);
  EXPECT_EQ(tsumo.delta, (std::array<int, 4>{-2, -2, 6, -2}));
  EXPECT_EQ(Sum(tsumo.delta), 0);
}

// R15（A1_ACCEPTANCE §七 / RULESET §10.3 S12）：判番确定性——同一手牌、输入牌序打乱 1000 次，
// `yaku[]`（id、han、顺序）与番数合计逐字段不变。打乱用固定种子的 mt19937（仅测试内使用，
// 不触任何产品代码路径）。
TEST(MahjongRules, YakuDetectionIsDeterministicUnderShuffles) {
  const struct Case {
    const char* label;
    std::vector<const char*> tiles;
    std::vector<Meld> melds;
  } cases[] = {
      {"门清平和断幺九", {"2m", "3m", "4m", "5m", "6m", "7m", "2p", "3p", "4p", "6p", "7p", "8p", "5s", "5s"}, {}},
      {"七对子清一色", {"1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2m", "2m", "4m", "4m", "6m", "6m"}, {}},
      {"对对和三暗刻箭刻", {"1m", "1m", "1m", "5p", "5p", "5p", "9s", "9s", "9s", "2z", "2z", "2z", "4z", "4z"}, {}},
      {"副露三色", {"2m", "3m", "4m", "2p", "3p", "4p", "2s", "3s", "4s", "7z", "7z", "7z", "9m", "9m"}, {}},
      {"暗杠混一色", {"1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m", "9m", "2m", "2m"}, {Ankan({"1z", "1z", "1z", "1z"})}},
  };
  std::mt19937 rng(20261118);
  for (const Case& item : cases) {
    std::vector<Tile> hand = H({item.tiles.begin(), item.tiles.end()});
    ASSERT_TRUE(CanWinExact(hand, item.melds)) << item.label;
    const std::vector<Yaku> reference =
        DetectYaku(hand, item.melds, hand.front(), Ctx(0, -1));
    const int reference_han = YakuHanTotal(reference);
    for (int iter = 0; iter < 1000; ++iter) {
      std::shuffle(hand.begin(), hand.end(), rng);
      const std::vector<Yaku> yaku = DetectYaku(hand, item.melds, hand.front(), Ctx(0, -1));
      if (yaku != reference || YakuHanTotal(yaku) != reference_han) {
        std::string dealt;
        for (Tile t : hand) dealt += FormatTile(t);
        FAIL() << item.label << "：第 " << iter << " 次打乱后判番漂移（hand=" << dealt << "）";
      }
    }
    // 判番顺序冻结的另一个侧面：连续两次调用（不打乱）也必须逐字段相同。
    EXPECT_EQ(DetectYaku(hand, item.melds, hand.front(), Ctx(0, -1)), reference) << item.label;
  }
}

TEST(MahjongRules, ZeroSumForEverySettlementKind) {  std::vector<Tile> hand =
      H({"1m", "2m", "3m", "4p", "5p", "6p", "7p", "8p", "9p", "2m", "2m"});
  const std::vector<Meld> pong = {Pong({"9s", "9s", "9s"})};
  ASSERT_TRUE(CanWinExact(hand, pong));

  // 荣和：−R / +R / 0 / 0
  for (int winner = 0; winner < 4; ++winner) {
    for (int from = 0; from < 4; ++from) {
      if (from == winner) continue;
      const ScoreResult ron = ScoreHand(hand, pong, *ParseTile("2m"), Ctx(winner, from));
      EXPECT_EQ(Sum(ron.delta), 0) << "winner=" << winner << " from=" << from;
      EXPECT_EQ(ron.delta[static_cast<size_t>(from)], -ron.multiplier);
      EXPECT_EQ(ron.delta[static_cast<size_t>(winner)], ron.multiplier);
    }
    // 自摸：3×(−2R) + 6R
    const ScoreResult tsumo = ScoreHand(hand, pong, *ParseTile("2m"), Ctx(winner, -1));
    EXPECT_EQ(Sum(tsumo.delta), 0);
    EXPECT_EQ(tsumo.delta[static_cast<size_t>(winner)], tsumo.total);
    EXPECT_EQ(tsumo.total, 3 * tsumo.per_payer);
  }
  // 荒庄：全 0
  const ScoreResult draw = ScoreDraw();
  for (int d : draw.delta) EXPECT_EQ(d, 0);
  EXPECT_EQ(draw.multiplier, 0);
  EXPECT_EQ(draw.han, 0);
}

TEST(MahjongRules, ZeroHanWinScoresOne) {  // A11：无起和番，0 番和牌得 1 分
  const std::vector<Meld> pong = {Pong({"9s", "9s", "9s"})};
  std::vector<Tile> hand = H({"1m", "2m", "3m", "4p", "5p", "6p", "7p", "8p", "9p", "2m", "2m"});
  ASSERT_TRUE(CanWinExact(hand, pong));
  const ScoreResult score = ScoreHand(hand, pong, *ParseTile("2m"), Ctx(1, 3));
  EXPECT_TRUE(score.yaku.empty());
  EXPECT_EQ(score.han, 0);
  EXPECT_EQ(score.multiplier, 1);
  EXPECT_EQ(score.delta[1], 1);
  EXPECT_EQ(score.delta[3], -1);
  EXPECT_EQ(Sum(score.delta), 0);
}

TEST(MahjongRules, RegressionAnchorLegacyTags) {
  // 验收项 A10 / §2.3 的回归锚点（B1.1 裁决后的三态口径）：
  //   ① 0 番荣和：旧 M=1 ⇒ −1/+1；新 R=1 ⇒ −1/+1 —— 逐位相等
  //   ② 0 番自摸：旧 M=1 ⇒ 三家 −2 / 和牌者 +6；新（自摸不是番种，×2 只在支付公式）⇒
  //      han=0 ⇒ R=1 ⇒ 三家 −2 / 和牌者 +6 —— 逐位相等
  //   ③ 荒庄：全 0 —— 逐位相等
  //   （七对子/清一色的差额是设计升级、故意为之：±4→±8、±4→±64，见下方对照断言）
  const std::vector<Meld> pong = {Pong({"9s", "9s", "9s"})};
  std::vector<Tile> zero_han = H({"1m", "2m", "3m", "4p", "5p", "6p", "7p", "8p", "9p", "2m", "2m"});
  ASSERT_TRUE(CanWinExact(zero_han, pong));

  // ① 0 番荣和
  const ScoreResult ron = ScoreHand(zero_han, pong, *ParseTile("2m"), Ctx(1, 3));
  EXPECT_TRUE(ron.yaku.empty());
  EXPECT_EQ(ron.han, 0);
  EXPECT_EQ(ron.multiplier, 1);
  EXPECT_EQ(ron.delta, (std::array<int, 4>{0, 1, 0, -1}));

  // ② 0 番自摸（自摸不是番种 ⇒ han 与荣和一致，仅支付路径不同）
  const ScoreResult tsumo = ScoreHand(zero_han, pong, *ParseTile("2m"), Ctx(1, -1));
  EXPECT_TRUE(tsumo.yaku.empty());
  EXPECT_EQ(tsumo.han, 0);
  EXPECT_EQ(tsumo.multiplier, 1);
  EXPECT_EQ(tsumo.delta, (std::array<int, 4>{-2, 6, -2, -2}));

  // ③ 荒庄
  const ScoreResult draw = ScoreDraw();
  EXPECT_EQ(draw.delta, (std::array<int, 4>{0, 0, 0, 0}));

  // 新旧行为对照（设计升级，故意为之）：差额全部来自新增番种与封顶倍率，而非自摸
  // ④ 七对子荣和：旧 M=4 ⇒ ±4；新 menzen(1)+seven_pairs(2)=3 番 ⇒ R=8 ⇒ ±8
  std::vector<Tile> seven =
      H({"1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p", "6p", "8p", "8p"});
  const ScoreResult seven_ron = ScoreHand(seven, {}, *ParseTile("8p"), Ctx(0, 3));
  EXPECT_EQ(seven_ron.delta[0], 8);
  EXPECT_EQ(seven_ron.delta[3], -8);
  EXPECT_EQ(YakuHanTotal(seven_ron.yaku), 3);
  // 若把 menzen 从合计里摘掉，则新旧倍率完全相等（证明差异**只**来自新增番种）
  EXPECT_EQ(MultiplierForHan(YakuHanTotal(seven_ron.yaku) - 1), 4);

  // ⑤ 清一色荣和：旧 M=4；新 menzen(1)+sanankou(2)+pure_one_suit(5)=8 番 ⇒ 封顶 6 ⇒ R=64
  std::vector<Tile> pure =
      H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m", "6m", "7m", "7m"});
  const ScoreResult pure_ron = ScoreHand(pure, {}, *ParseTile("7m"), Ctx(0, 3));
  EXPECT_EQ(pure_ron.multiplier, 64);
  EXPECT_EQ(pure_ron.delta[0], 64);
}

// ---------------------------------------------------------------------------
// R4：杠（5 值 MeldType / 张数推导 / 就地升级 / 张数公式不变）
// ---------------------------------------------------------------------------
TEST(MahjongRules, MeldTypesAndKanCounting) {
  EXPECT_EQ(MeldTypeName(MeldType::kPong), "pong");
  EXPECT_EQ(MeldTypeName(MeldType::kChow), "chow");
  EXPECT_EQ(MeldTypeName(MeldType::kKong), "kong");
  EXPECT_EQ(MeldTypeName(MeldType::kAnkan), "ankan");
  EXPECT_EQ(MeldTypeName(MeldType::kKakan), "kakan");

  EXPECT_TRUE(IsKanType(MeldType::kKong));
  EXPECT_TRUE(IsKanType(MeldType::kAnkan));
  EXPECT_TRUE(IsKanType(MeldType::kKakan));
  EXPECT_FALSE(IsKanType(MeldType::kPong));
  EXPECT_FALSE(IsKanType(MeldType::kChow));

  // 门清：ankan 不破；kakan 必然破（结构结论）
  EXPECT_FALSE(BreaksMenzen(MeldType::kAnkan));
  EXPECT_TRUE(BreaksMenzen(MeldType::kPong));
  EXPECT_TRUE(BreaksMenzen(MeldType::kChow));
  EXPECT_TRUE(BreaksMenzen(MeldType::kKong));
  EXPECT_TRUE(BreaksMenzen(MeldType::kKakan));

  // 合法性：ankan/kakan 与 kong 同形（4 张同面）
  EXPECT_TRUE(IsValidMeld(Ankan({"5z", "5z", "5z", "5z"})));
  EXPECT_TRUE(IsValidMeld(Kakan({"5z", "5z", "5z", "5z"})));
  EXPECT_FALSE(IsValidMeld(Ankan({"5z", "5z", "5z"})));
  EXPECT_FALSE(IsValidMeld(Kakan({"5z", "5z", "5z"})));

  // kans = 三种杠的组数之和（旧「仅明杠」作废）；melds 仍是组数
  const std::vector<Meld> melds = {Pong({"1m", "1m", "1m"}), Kong({"2m", "2m", "2m", "2m"}),
                                   Ankan({"3m", "3m", "3m", "3m"}),
                                   Kakan({"4m", "4m", "4m", "4m"})};
  EXPECT_EQ(MeldCountOf(melds), 4);
  EXPECT_EQ(KanCountOf(melds), 3);
}

TEST(MahjongRules, AnkanAndKakanDetection) {
  // 手中 1m 恰 4 张 ⇒ 可暗杠；5p 仅 1 张（是副露 pong 5p 的第 4 张）⇒ 该组可加杠；
  // 9m 不在手 ⇒ 9m 的 pong 组不可加杠
  std::vector<Tile> hand = H({"1m", "1m", "1m", "1m", "5p", "2s", "3s", "4s"});
  EXPECT_EQ(AnkanFaces(hand), std::vector<Face>{FaceOf(*ParseTile("1m"))});
  EXPECT_TRUE(CanAnkan(hand));
  // 恰好 3 张不是暗杠（明杠走 CanKong 的鸣牌路径）
  EXPECT_FALSE(CanAnkan(H({"1m", "1m", "1m", "5p"})));
  EXPECT_TRUE(AnkanFaces(H({"1m", "2m", "3m"})).empty());

  // 加杠：只有**手中还有同面第 4 张**的那组 pong 可加杠
  const std::vector<Meld> melds = {Pong({"5p", "5p", "5p"}), Pong({"9m", "9m", "9m"})};
  EXPECT_EQ(KakanMeldIndices(hand, melds), std::vector<int>{0});
  EXPECT_TRUE(CanKakan(hand, melds));
  // 手里没有第 4 张 ⇒ 不可加杠
  EXPECT_FALSE(CanKakan(hand, {Pong({"9m", "9m", "9m"})}));
  // 非 pong（chow/kong/ankan/kakan）不可再加杠
  EXPECT_FALSE(CanKakan(H({"2s", "3s", "4s", "5p"}), {Kong({"5p", "5p", "5p", "5p"})}));
}

TEST(MahjongRules, KongTileAccountingStaysInsideBothFormulas) {
  // 逻辑张数 = concealed + 3×melds ∈ {13,14}；物理 = 逻辑 + kans。**两条公式逐字不变**，
  // 三个杠的差别只在「第 4 张从哪来」，对台账零影响（GAME_DESIGN_V2 §4.1）。
  std::vector<Tile> hand14 = H({"1m", "1m", "1m", "1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m",
                                "9m", "5p", "5p"});
  ASSERT_EQ(hand14.size(), 14u);

  // ---- 暗杠：14 → 扣 4 张、melds+1 ⇒ 10 + 3 = 13 → 补 1 张 ⇒ 14 ----
  const auto ankan = ApplyAnkan(hand14, {}, FaceOf(*ParseTile("1m")));
  ASSERT_TRUE(ankan.has_value());
  EXPECT_EQ(ankan->hand.size(), 10u);
  EXPECT_EQ(MeldCountOf(ankan->melds), 1);
  EXPECT_EQ(KanCountOf(ankan->melds), 1);
  EXPECT_EQ(ankan->melds[0].type, MeldType::kAnkan);
  EXPECT_EQ(ankan->melds[0].tiles.size(), 4u);
  EXPECT_EQ(ankan->hand.size() + 3u * MeldCountOf(ankan->melds), 13u);
  // 补牌（房间负责取牌山头；规则库只维护公式）
  std::vector<Tile> after_draw = ankan->hand;
  after_draw.push_back(*ParseTile("3p"));
  EXPECT_EQ(after_draw.size() + 3u * MeldCountOf(ankan->melds), 14u);
  EXPECT_EQ(after_draw.size() + 3u * MeldCountOf(ankan->melds) + KanCountOf(ankan->melds), 15u);

  // ---- 加杠：14 → 扣 1 张、melds 不变 ⇒ 13 → 补 1 张 ⇒ 14；kans+1 使物理张数守恒 ----
  const std::vector<Meld> pong = {Pong({"8m", "8m", "8m"})};
  std::vector<Tile> hand = H({"8m", "2m", "3m", "4m", "5m", "6m", "7m", "9m", "1p", "1p", "5s"});
  ASSERT_EQ(hand.size() + 3u * MeldCountOf(pong), 14u);
  const auto kakan = ApplyKakan(hand, pong, 0);
  ASSERT_TRUE(kakan.has_value());
  EXPECT_EQ(kakan->hand.size(), 10u);
  EXPECT_EQ(kakan->hand.size() + 3u * MeldCountOf(kakan->melds), 13u);
  // 就地升级：组序不变、组数不变、type=kakan、tiles 变 4 张同面
  ASSERT_EQ(kakan->melds.size(), 1u);
  EXPECT_EQ(kakan->melds[0].type, MeldType::kKakan);
  EXPECT_EQ(kakan->melds[0].tiles.size(), 4u);
  for (Tile t : kakan->melds[0].tiles) EXPECT_EQ(FaceOf(t), FaceOf(*ParseTile("8m")));
  EXPECT_TRUE(IsValidMeld(kakan->melds[0]));
  EXPECT_EQ(KanCountOf(kakan->melds), 1);
  std::vector<Tile> kakan_draw = kakan->hand;
  kakan_draw.push_back(*ParseTile("3p"));
  EXPECT_EQ(kakan_draw.size() + 3u * MeldCountOf(kakan->melds), 14u);
  EXPECT_EQ(kakan_draw.size() + 3u * MeldCountOf(kakan->melds) + KanCountOf(kakan->melds), 15u);

  // 组序不变：kakan 后第 2 组仍是原来的第 2 组
  const std::vector<Meld> two_pongs = {Pong({"1s", "1s", "1s"}), Pong({"8m", "8m", "8m"})};
  std::vector<Tile> hand2 = H({"8m", "2m", "3m", "4m", "5m", "6m", "7m", "9m"});
  ASSERT_EQ(hand2.size() + 3u * MeldCountOf(two_pongs), 14u);
  const auto upgraded = ApplyKakan(hand2, two_pongs, 1);
  ASSERT_TRUE(upgraded.has_value());
  ASSERT_EQ(upgraded->melds.size(), 2u);
  EXPECT_EQ(upgraded->melds[0].type, MeldType::kPong);
  EXPECT_EQ(FaceOf(upgraded->melds[0].tiles[0]), FaceOf(*ParseTile("1s")));
  EXPECT_EQ(upgraded->melds[1].type, MeldType::kKakan);

  // 条件不成立 ⇒ nullopt（不抛错）
  EXPECT_FALSE(ApplyAnkan(hand14, {}, FaceOf(*ParseTile("2m"))).has_value());  // 只有 1 张
  EXPECT_FALSE(ApplyKakan(hand, pong, /*meld_index=*/5).has_value());
  // 手中没有该面的第 4 张 ⇒ 不可加杠
  EXPECT_FALSE(ApplyKakan(hand, {Pong({"9s", "9s", "9s"})}, 0).has_value());
}

TEST(MahjongRules, DiscardOptionsKongWindows) {
  // 顺序固定 win → ankan → kakan（R6）
  const std::vector<Meld> melds = {Pong({"5p", "5p", "5p"})};
  std::vector<Tile> hand =
      H({"1m", "1m", "1m", "1m", "5p", "2m", "3m", "4m", "6m", "7m", "8m"});
  ASSERT_EQ(hand.size() + 3u * MeldCountOf(melds), 14u);
  ASSERT_FALSE(CanWinExact(hand, melds));  // 该手牌不是和牌 ⇒ 首项应是 ankan
  const std::vector<Option> options = DiscardOptions(hand, melds, /*remaining=*/10);
  ASSERT_GE(options.size(), 2u);
  EXPECT_EQ(options[0].action, "ankan");
  // B1.1 裁决 G：选项 tiles = 恰好 1 个牌面元素（与 kong 同形，PROTOCOL v1.6 §4.6；
  // UI 显示 4 张是客户端的事）
  ASSERT_EQ(options[0].tiles.size(), 1u);
  EXPECT_EQ(FaceOf(options[0].tiles[0]), FaceOf(*ParseTile("1m")));
  EXPECT_EQ(options[1].action, "kakan");
  EXPECT_EQ(options[1].tiles.size(), 1u);
  EXPECT_TRUE(OptionTilesMatch(options[1], H({"5p"})));
  EXPECT_EQ(FindOption(options, "win"), nullptr);

  // win 在 ankan 之前
  std::vector<Tile> winning =
      H({"1m", "1m", "1m", "1m", "2m", "3m", "4p", "5p", "6p", "7s", "8s", "9s", "5z", "5z"});
  ASSERT_TRUE(CanWinExact(winning, {}));
  const std::vector<Option> win_options = DiscardOptions(winning, {}, /*remaining=*/5);
  ASSERT_GE(win_options.size(), 2u);
  EXPECT_EQ(win_options[0].action, "win");
  EXPECT_EQ(win_options[1].action, "ankan");

  // remaining == 0 ⇒ 无牌可补 ⇒ 不提供任何杠选项（与 CanKong 同源守卫）
  EXPECT_EQ(FindOption(DiscardOptions(hand, melds, /*remaining=*/0), "ankan"), nullptr);
  EXPECT_EQ(FindOption(DiscardOptions(hand, melds, /*remaining=*/0), "kakan"), nullptr);
  // 未传 remaining（B2 未接线）⇒ 不提供（安全默认，房间行为不变）
  EXPECT_TRUE(DiscardOptions(hand, melds).empty());
  // 非本家行动态（13 张）⇒ 不提供
  std::vector<Tile> waiting = hand;
  waiting.pop_back();
  EXPECT_EQ(waiting.size() + 3u * MeldCountOf(melds), 13u);
  EXPECT_TRUE(DiscardOptions(waiting, melds, /*remaining=*/10).empty());
}

// ---------------------------------------------------------------------------
// R5：庄家与发牌 / 连庄
// ---------------------------------------------------------------------------
TEST(MahjongRules, DealTilesRespectsDealerSeat) {
  const std::vector<Tile> wall = BuildWall(20261005);
  for (int dealer = 0; dealer < 4; ++dealer) {
    const Deal deal = DealTiles(wall, dealer);
    for (int seat = 0; seat < 4; ++seat) {
      const size_t expected = (seat == dealer) ? 14u : 13u;
      EXPECT_EQ(deal.hands[static_cast<size_t>(seat)].size(), expected)
          << "dealer=" << dealer << " seat=" << seat;
    }
    EXPECT_EQ(deal.remaining, 83);
    EXPECT_EQ(deal.next_index, 53);
    // 53 张互不重复，且第 53 张（index 52）发给庄家
    std::set<uint8_t> ids;
    for (const auto& hand : deal.hands) {
      for (Tile t : hand) ids.insert(t.id);
    }
    EXPECT_EQ(ids.size(), 53u) << "dealer=" << dealer;
    EXPECT_NE(std::find(deal.hands[static_cast<size_t>(dealer)].begin(),
                        deal.hands[static_cast<size_t>(dealer)].end(), wall[52]),
              deal.hands[static_cast<size_t>(dealer)].end())
        << "第 53 张必须发给庄家（dealer=" << dealer << "）";
    // 牌山剩余段与发出去 53 张不重叠
    EXPECT_EQ(deal.wall.size(), static_cast<size_t>(kTileCount));
  }
  // 同一 dealer 两次发牌一致（确定性），不同 dealer 只搬第 53 张
  EXPECT_EQ(DealTiles(wall, 2).hands, DealTiles(wall, 2).hands);
  const Deal d0 = DealTiles(wall, 0);
  const Deal d3 = DealTiles(wall, 3);
  for (int seat = 0; seat < 4; ++seat) {
    if (seat == 0 || seat == 3) continue;
    EXPECT_EQ(d0.hands[static_cast<size_t>(seat)], d3.hands[static_cast<size_t>(seat)]);
  }
  // 越界庄家按 0 处理
  EXPECT_EQ(DealTiles(wall, -1).hands[0].size(), 14u);
  EXPECT_EQ(DealTiles(wall, 9).hands[0].size(), 14u);
}

TEST(MahjongRules, NextDealerAndClassifyRound) {
  // 庄家和牌 ⇒ 连庄
  for (int dealer = 0; dealer < 4; ++dealer) {
    EXPECT_EQ(NextDealer(dealer, RoundResult::kDealerWin), dealer);
    // 荒庄 ⇒ 一律连庄（无和牌者的显式消歧）
    EXPECT_EQ(NextDealer(dealer, RoundResult::kDrawExhausted), dealer);
    // 闲家和牌 ⇒ 轮庄 (dealer+1)%4
    EXPECT_EQ(NextDealer(dealer, RoundResult::kNonDealerWin), (dealer + 1) % 4);
  }
  EXPECT_EQ(NextDealer(3, RoundResult::kNonDealerWin), 0) << "轮庄必须回绕到 0";

  // reason → RoundResult：荒庄不看和牌者
  EXPECT_EQ(ClassifyRound("draw_exhausted", 2, 0), RoundResult::kDrawExhausted);
  EXPECT_EQ(ClassifyRound("win_ron", 0, 0), RoundResult::kDealerWin);
  EXPECT_EQ(ClassifyRound("win_tsumo", 0, 0), RoundResult::kDealerWin);
  EXPECT_EQ(ClassifyRound("win_ron", 2, 0), RoundResult::kNonDealerWin);
  EXPECT_EQ(ClassifyRound("win_tsumo", 3, 2), RoundResult::kNonDealerWin);
}

}  // namespace mahjong
