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
  return Meld{MeldType::kPong, tiles};
}

Meld Kong(std::vector<std::string> texts) {
  std::vector<Tile> tiles = H(std::move(texts));
  SortTiles(tiles);
  return Meld{MeldType::kKong, tiles};
}

int Sum(const std::array<int, 4>& delta) {
  return delta[0] + delta[1] + delta[2] + delta[3];
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
  const Deal deal = DealTiles(BuildWall(42));
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
  const Deal again = DealTiles(BuildWall(42));
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
  EXPECT_TRUE(WinTags(p1_full, {}).empty());  // 平和：无牌型标签

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
  EXPECT_EQ(WinTags(p3, {}), std::vector<std::string>{"seven_pairs"});

  // 自摸形态统一入口
  EXPECT_TRUE(IsWinningHand(p3, {}, *ParseTile("8p"), /*is_tsumo=*/true));
}

TEST(MahjongRules, WinPureOneSuit) {  // 6
  std::vector<Tile> p4 = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m", "4m", "5m",
                            "6m", "7m", "7m"});
  EXPECT_TRUE(CanWinExact(p4, {}));
  EXPECT_FALSE(IsSevenPairs(p4));
  EXPECT_EQ(WinTags(p4, {}), std::vector<std::string>{"pure_one_suit"});

  std::vector<Tile> p5 = H({"1m", "1m", "2m", "2m", "3m", "3m", "4m", "4m", "5m", "5m", "6m",
                            "6m", "7m", "7m"});
  EXPECT_TRUE(CanWinExact(p5, {}));
  EXPECT_EQ(WinTags(p5, {}),
            (std::vector<std::string>{"seven_pairs", "pure_one_suit"}));
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
  // S1 平和非自摸：放炮者 −1、和牌者 +1
  const ScoreResult ron = ScoreWin(/*winner=*/1, /*from_seat=*/0, hira, {});
  EXPECT_EQ(ron.multiplier, 1);
  EXPECT_EQ(ron.per_payer, 1);
  EXPECT_EQ(ron.delta[0], -1);
  EXPECT_EQ(ron.delta[1], 1);
  EXPECT_EQ(ron.delta[2], 0);
  EXPECT_EQ(ron.delta[3], 0);
  EXPECT_EQ(Sum(ron.delta), 0);

  // S3 平和自摸：三家各 −2、和牌者 +6
  const ScoreResult tsumo = ScoreWin(/*winner=*/2, /*from_seat=*/-1, hira, {});
  EXPECT_EQ(tsumo.per_payer, 2);
  EXPECT_EQ(tsumo.total, 6);
  EXPECT_EQ(tsumo.delta[0], -2);
  EXPECT_EQ(tsumo.delta[1], -2);
  EXPECT_EQ(tsumo.delta[3], -2);
  EXPECT_EQ(tsumo.delta[2], 6);
  EXPECT_EQ(Sum(tsumo.delta), 0);

  // S5 荒庄：四家全 0
  const ScoreResult draw = ScoreDraw();
  EXPECT_EQ(draw.multiplier, 0);
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

  EXPECT_EQ(MultiplierForTags(WinTags(seven, {})), 4);
  EXPECT_EQ(MultiplierForTags(WinTags(pure, {})), 4);
  EXPECT_EQ(MultiplierForTags(WinTags(both, {})), 16);

  // 七对子荣和：−4 / +4
  const ScoreResult r = ScoreWin(0, 3, seven, {});
  EXPECT_EQ(r.multiplier, 4);
  EXPECT_EQ(r.delta[3], -4);
  EXPECT_EQ(r.delta[0], 4);
  EXPECT_EQ(Sum(r.delta), 0);

  // 七对子 + 清一色自摸：三家各 −32、和牌者 +96
  const ScoreResult t = ScoreWin(1, -1, both, {});
  EXPECT_EQ(t.multiplier, 16);
  EXPECT_EQ(t.per_payer, 32);
  EXPECT_EQ(t.total, 96);
  EXPECT_EQ(t.delta[1], 96);
  EXPECT_EQ(t.delta[0], -32);
  EXPECT_EQ(t.delta[2], -32);
  EXPECT_EQ(t.delta[3], -32);
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
  // 字牌做刻子：3 组字牌刻子 + 1 组数牌刻子 + 将
  std::vector<Tile> honor_triplet = H({"5z", "5z", "5z", "6z", "6z", "6z", "7z", "7z", "7z",
                                       "1m", "1m", "1m", "9s", "9s"});
  EXPECT_TRUE(CanWinExact(honor_triplet, {}));
  EXPECT_EQ(WinTags(honor_triplet, {}), std::vector<std::string>{});  // 字牌不产生清一色

  // 字牌做将：4 组数牌面子 + 将 1z1z
  std::vector<Tile> honor_pair = H({"1m", "2m", "3m", "4m", "5m", "6m", "7p", "8p", "9p", "1s",
                                    "2s", "3s", "1z", "1z"});
  EXPECT_TRUE(CanWinExact(honor_pair, {}));
  EXPECT_EQ(WinTags(honor_pair, {}), std::vector<std::string>{});

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
  EXPECT_EQ(WinTags(honor_seven, {}), std::vector<std::string>{"seven_pairs"});
  const ScoreResult r = ScoreWin(0, 3, honor_seven, {});
  EXPECT_EQ(r.multiplier, 4);  // 七对子 ×4，字牌不额外加成
  EXPECT_EQ(r.delta[0], 4);
  EXPECT_EQ(r.delta[3], -4);
  EXPECT_EQ(Sum(r.delta), 0);

  // 全字牌七对子：仍只是七对子（无「字一色」→ 不是清一色）
  std::vector<Tile> all_honor_seven = H({"1z", "1z", "2z", "2z", "3z", "3z", "4z", "4z", "5z",
                                         "5z", "6z", "6z", "7z", "7z"});
  EXPECT_TRUE(CanWinExact(all_honor_seven, {}));
  EXPECT_EQ(WinTags(all_honor_seven, {}), std::vector<std::string>{"seven_pairs"});
  EXPECT_EQ(MultiplierForTags(WinTags(all_honor_seven, {})), 4);
}

TEST(MahjongRules, HonorIsNotPureOneSuit) {  // 23
  // 全字牌（4 组字牌刻子 + 字牌将）：能和的，但**不是**清一色（RULESET §3.4 无「字一色」）
  std::vector<Tile> all_honor = H({"1z", "1z", "1z", "2z", "2z", "2z", "3z", "3z", "3z", "4z",
                                   "4z", "4z", "5z", "5z"});
  EXPECT_TRUE(CanWinExact(all_honor, {}));
  EXPECT_FALSE(IsPureOneSuit(all_honor, {}));
  EXPECT_TRUE(WinTags(all_honor, {}).empty());
  EXPECT_EQ(MultiplierForTags(WinTags(all_honor, {})), 1);  // 不得拿到 ×4

  // 含单个字牌对的数牌清一色手牌 → 也不是清一色
  std::vector<Tile> one_honor_pair = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "3m", "3m",
                                        "4m", "5m", "6m", "1z", "1z"});
  EXPECT_TRUE(CanWinExact(one_honor_pair, {}));
  EXPECT_FALSE(IsPureOneSuit(one_honor_pair, {}));
  EXPECT_TRUE(WinTags(one_honor_pair, {}).empty());

  // 副露含字牌刻子时同样不是清一色（1 组副露 ⇒ 门前 11 张：3 组面子 + 将）
  const std::vector<Meld> honor_meld = {Pong({"5z", "5z", "5z"})};
  std::vector<Tile> with_meld = H({"1m", "1m", "1m", "2m", "2m", "2m", "3m", "4m", "5m", "6m",
                                   "6m"});
  ASSERT_EQ(with_meld.size(), 11u);
  EXPECT_TRUE(CanWinExact(with_meld, honor_meld));
  EXPECT_FALSE(IsPureOneSuit(with_meld, honor_meld));
  EXPECT_TRUE(WinTags(with_meld, honor_meld).empty());
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
  EXPECT_TRUE(IsValidMeld(Meld{MeldType::kChow, H({"3p", "4p", "5p"})}));
  EXPECT_FALSE(IsValidMeld(Meld{MeldType::kChow, H({"3p", "4p", "6p"})}));
  EXPECT_FALSE(IsValidMeld(Meld{MeldType::kChow, H({"8p", "9p", "1s"})}));
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

}  // namespace mahjong
