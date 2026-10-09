// 麻将规则库实现（规格：game/docs/RULESET.md v1.0）
#include "mahjong_rules.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>

namespace mahjong {
namespace {

constexpr uint8_t kIdsPerFace = kCopiesPerFace;

char SuitChar(uint8_t suit) {
  switch (suit) {
    case 0:
      return 'm';
    case 1:
      return 'p';
    case 2:
      return 's';
    default:
      return 'z';  // 3 = 字牌（1z..7z）
  }
}

// 摘除「最小牌面」锚点的递归拆解（RULESET §3.2）：
// 最小牌面只能是刻子或顺子的最小张，故分支完备。
bool Decompose(FaceCounts& counts, int need_groups) {
  if (need_groups == 0) {
    for (uint8_t c : counts) {
      if (c != 0) return false;
    }
    return true;
  }
  int first = -1;
  for (int f = 0; f < kFaceCount; ++f) {
    if (counts[f] != 0) {
      first = f;
      break;
    }
  }
  if (first < 0) return false;

  // A. 刻子
  if (counts[first] >= 3) {
    counts[first] -= 3;
    const bool ok = Decompose(counts, need_groups - 1);
    counts[first] += 3;
    if (ok) return true;
  }
  // B. 顺子（first 必为顺子最小张）。
  //    ⚠️ 必须显式排除字牌面：字牌编号 27..33 是连续的，若只判「编号连续」，
  //    `1z(27) 2z(28) 3z(29)` 会被误判成一个顺子，从而判出**非法的和牌型**。
  //    顺子只在同一门数牌内成立（`SuitOf` 相同且为 0/1/2），因此这里用 IsNumberFace 兜住。
  const int rank = first % kRankCount;  // 0-based
  if (IsNumberFace(static_cast<Face>(first)) && rank <= kRankCount - 3 &&
      counts[first + 1] != 0 && counts[first + 2] != 0) {
    counts[first] -= 1;
    counts[first + 1] -= 1;
    counts[first + 2] -= 1;
    const bool ok = Decompose(counts, need_groups - 1);
    counts[first] += 1;
    counts[first + 1] += 1;
    counts[first + 2] += 1;
    if (ok) return true;
  }
  return false;
}

// 标准型：从 14 张逻辑牌里取 4 组面子 + 1 对将。
//
// ⚠️ 和牌判定对**字牌与数牌一视同仁**：本函数完全基于「牌面计数」，将牌与刻子都只按计数处理，
//    因此字牌做将（如 `5z 5z`）与字牌刻子（如 `1z 1z 1z`）天然成立，**不需要、也不允许**
//    为字牌写任何特例分支。字牌在判定中的唯一区别是「不能参与顺子」，该约束已经收在
//    Decompose 的顺子分支里（IsNumberFace 守卫）。
bool IsStandardShape(const FaceCounts& counts, int need_groups) {
  if (need_groups < 0) return false;
  int total = 0;
  for (uint8_t c : counts) total += c;
  if (total != need_groups * 3 + 2) return false;

  FaceCounts work = counts;
  for (int f = 0; f < kFaceCount; ++f) {
    if (work[f] >= 2) {
      work[f] -= 2;
      const bool ok = Decompose(work, need_groups);
      work[f] += 2;
      if (ok) return true;
    }
  }
  return false;
}

bool AllTilesSameSuit(const std::vector<Tile>& tiles, uint8_t suit) {
  for (Tile t : tiles) {
    if (SuitOf(FaceOf(t)) != suit) return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 牌张
// ---------------------------------------------------------------------------
Face FaceOf(Tile tile) { return static_cast<Face>(tile.id / kIdsPerFace); }
uint8_t SuitOf(Face face) { return static_cast<uint8_t>(face / kRankCount); }
uint8_t RankOf(Face face) { return static_cast<uint8_t>(face % kRankCount + 1); }
uint8_t CopyOf(Tile tile) { return static_cast<uint8_t>(tile.id % kIdsPerFace); }

Tile MakeTileFromFace(Face face, uint8_t copy) {
  return Tile{static_cast<uint8_t>(face * kIdsPerFace + (copy % kCopiesPerFace))};
}

Tile MakeTile(uint8_t suit, uint8_t rank, uint8_t copy) {
  const Face face = static_cast<Face>(suit * kRankCount + (rank - 1));
  return MakeTileFromFace(face, copy);
}

// 数牌 / 字牌判定：字牌面 27..33（SuitOf == 3）
bool IsNumberFace(Face face) { return face < kNumericFaceCount; }
bool IsHonorFace(Face face) { return face >= kNumericFaceCount && face < kFaceCount; }

std::string_view HonorName(Face face) {
  static constexpr std::string_view kNames[kHonorCount] = {"东", "南", "西", "北", "白", "发", "中"};
  if (!IsHonorFace(face)) return {};
  return kNames[face - kNumericFaceCount];
}

std::optional<Tile> ParseTile(std::string_view text) {
  // 合取正则（PROTOCOL.md §2.1）：`^([1-9][mps]|[1-7]z)$`
  if (text.size() != 2) return std::nullopt;
  const char rank_char = text[0];
  const char suit_char = text[1];
  if (rank_char < '1' || rank_char > '9') return std::nullopt;
  if (suit_char == 'z') {
    // 字牌仅 1z..7z（8z/9z 非法）
    if (rank_char > '7') return std::nullopt;
    const Face face =
        static_cast<Face>(kNumericFaceCount + (rank_char - '1'));  // 27..33
    return MakeTileFromFace(face);
  }
  uint8_t suit = 0;
  if (suit_char == 'm') {
    suit = 0;
  } else if (suit_char == 'p') {
    suit = 1;
  } else if (suit_char == 's') {
    suit = 2;
  } else {
    return std::nullopt;
  }
  return MakeTile(suit, static_cast<uint8_t>(rank_char - '0'));
}

std::string FormatTile(Tile tile) {
  const Face face = FaceOf(tile);
  std::string out;
  out.push_back(static_cast<char>('0' + RankOf(face)));
  out.push_back(SuitChar(SuitOf(face)));
  return out;
}

std::optional<std::vector<Tile>> ParseTiles(const std::vector<std::string>& texts) {
  std::vector<Tile> tiles;
  tiles.reserve(texts.size());
  for (const std::string& text : texts) {
    const auto tile = ParseTile(text);
    if (!tile.has_value()) return std::nullopt;
    tiles.push_back(*tile);
  }
  return tiles;
}

void SortTiles(std::vector<Tile>& tiles) {
  // face 已按 (suit, rank) 排列，故按 face 再按 copy 排序即符合 PROTOCOL.md §2.2
  std::sort(tiles.begin(), tiles.end(), [](Tile a, Tile b) {
    if (FaceOf(a) != FaceOf(b)) return FaceOf(a) < FaceOf(b);
    return CopyOf(a) < CopyOf(b);
  });
}

std::vector<Tile> SortedTiles(std::vector<Tile> tiles) {
  SortTiles(tiles);
  return tiles;
}

FaceCounts CountFaces(const std::vector<Tile>& tiles) {
  FaceCounts counts{};
  counts.fill(0);
  for (Tile tile : tiles) counts[FaceOf(tile)]++;
  return counts;
}

bool IsVerifiedHand(const std::vector<Tile>& tiles) {
  const FaceCounts counts = CountFaces(tiles);
  for (uint8_t c : counts) {
    if (c > kCopiesPerFace) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 牌山与发牌（RULESET §1.3/§1.4）
// ---------------------------------------------------------------------------
std::vector<Tile> BuildWall(uint64_t seed) {
  std::vector<Tile> wall;
  wall.reserve(kTileCount);
  // 按牌面编号升序生成：0..26 为数牌（m→p→s），27..33 为字牌（1z..7z）。
  // 该顺序与 RULESET §1.2 的「先数牌后字牌」排序口径一致。
  for (int face = 0; face < kFaceCount; ++face) {
    for (int copy = 0; copy < kCopiesPerFace; ++copy) {
      wall.push_back(MakeTileFromFace(static_cast<Face>(face), static_cast<uint8_t>(copy)));
    }
  }
  std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
  for (int i = static_cast<int>(wall.size()) - 1; i >= 1; --i) {
    std::uniform_int_distribution<int> dist(0, i);
    std::swap(wall[static_cast<size_t>(i)], wall[static_cast<size_t>(dist(rng))]);
  }
  return wall;
}

Deal DealTiles(const std::vector<Tile>& wall) {
  Deal deal;
  deal.wall = wall;
  size_t index = 0;
  for (int seat = 0; seat < 4; ++seat) {
    for (int i = 0; i < kInitialHandSize && index < wall.size(); ++i) {
      deal.hands[static_cast<size_t>(seat)].push_back(wall[index++]);
    }
  }
  // 庄家（seat 0）第 14 张
  if (index < wall.size()) deal.hands[0].push_back(wall[index++]);
  for (auto& hand : deal.hands) SortTiles(hand);
  deal.next_index = static_cast<int>(index);
  deal.remaining = static_cast<int>(wall.size()) - static_cast<int>(index);
  return deal;
}

// ---------------------------------------------------------------------------
// 副露
// ---------------------------------------------------------------------------
std::string MeldTypeName(MeldType type) {
  switch (type) {
    case MeldType::kPong:
      return "pong";
    case MeldType::kChow:
      return "chow";
    default:
      return "kong";
  }
}

int MeldCountOf(const std::vector<Meld>& melds) { return static_cast<int>(melds.size()); }

int KanCountOf(const std::vector<Meld>& melds) {
  int kans = 0;
  for (const Meld& meld : melds) {
    if (meld.type == MeldType::kKong) ++kans;
  }
  return kans;
}

bool IsValidMeld(const Meld& meld) {
  if (!IsVerifiedHand(meld.tiles)) return false;
  const FaceCounts counts = CountFaces(meld.tiles);
  switch (meld.type) {
    case MeldType::kPong:
    case MeldType::kKong: {
      const size_t expected = (meld.type == MeldType::kPong) ? 3u : 4u;
      if (meld.tiles.size() != expected) return false;
      for (uint8_t c : counts) {
        if (c != 0 && c != expected) return false;
      }
      return true;
    }
    case MeldType::kChow: {
      if (meld.tiles.size() != 3) return false;
      std::vector<Face> faces;
      for (Tile t : meld.tiles) faces.push_back(FaceOf(t));
      std::sort(faces.begin(), faces.end());
      if (SuitOf(faces[0]) != SuitOf(faces[2])) return false;
      return faces[1] == faces[0] + 1 && faces[2] == faces[0] + 2;
    }
  }
  return false;
}

bool CanPong(const std::vector<Tile>& hand, Tile discarded) {
  return CountFaces(hand)[FaceOf(discarded)] >= 2;
}

bool CanKong(const std::vector<Tile>& hand, Tile discarded, int remaining) {
  return remaining >= 1 && CountFaces(hand)[FaceOf(discarded)] == 3;
}

std::vector<std::array<Tile, 2>> ChowCombinations(const std::vector<Tile>& hand,
                                                 Tile discarded) {
  std::vector<std::array<Tile, 2>> combos;
  const Face face = FaceOf(discarded);
  // 字牌不可吃（RULESET §4.2 / PROTOCOL §4.6）：字牌只能构成刻子/杠，永不产生 chow 选项。
  if (IsHonorFace(face)) return combos;
  const uint8_t suit = SuitOf(face);
  const uint8_t rank = RankOf(face);
  const FaceCounts counts = CountFaces(hand);

  const int candidate_offsets[3][2] = {{-2, -1}, {-1, 1}, {1, 2}};
  for (const auto& offsets : candidate_offsets) {
    const int r1 = static_cast<int>(rank) + offsets[0];
    const int r2 = static_cast<int>(rank) + offsets[1];
    if (r1 < 1 || r1 > kRankCount || r2 < 1 || r2 > kRankCount) continue;
    if (r1 == static_cast<int>(rank) || r2 == static_cast<int>(rank)) continue;
    const Face f1 = static_cast<Face>(suit * kRankCount + (r1 - 1));
    const Face f2 = static_cast<Face>(suit * kRankCount + (r2 - 1));
    if (counts[f1] >= 1 && counts[f2] >= 1) {
      combos.push_back({MakeTileFromFace(f1), MakeTileFromFace(f2)});
    }
  }
  return combos;
}

bool CanChow(const std::vector<Tile>& hand, Tile discarded) {
  return !ChowCombinations(hand, discarded).empty();
}

// ---------------------------------------------------------------------------
// 和牌判定（RULESET §3）
// ---------------------------------------------------------------------------
bool CanWinExact(const std::vector<Tile>& concealed_with_winning_tile,
                 const std::vector<Meld>& melds) {
  const int need_groups = 4 - MeldCountOf(melds);
  if (need_groups < 0) return false;
  const size_t expected = static_cast<size_t>(need_groups * 3 + 2);
  if (concealed_with_winning_tile.size() != expected) return false;

  // 物理牌守恒：concealed + meld 牌面合计每面不超过 4 张
  std::vector<Tile> all = concealed_with_winning_tile;
  for (const Meld& meld : melds) {
    all.insert(all.end(), meld.tiles.begin(), meld.tiles.end());
  }
  if (!IsVerifiedHand(all)) return false;

  const FaceCounts counts = CountFaces(concealed_with_winning_tile);
  // 七对子同样对字牌一视同仁（字牌对子合法，如 `1z 1z 2z 2z`）；标准型见 IsStandardShape 的说明。
  if (melds.empty() && IsSevenPairs(concealed_with_winning_tile)) return true;
  return IsStandardShape(counts, need_groups);
}

bool CanWinWith(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                Tile winning_tile) {
  std::vector<Tile> tiles = concealed;
  tiles.push_back(winning_tile);
  return CanWinExact(tiles, melds);
}

bool IsWinningHand(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                   Tile winning_tile, bool is_tsumo) {
  if (is_tsumo) return CanWinExact(concealed, melds);
  return CanWinWith(concealed, melds, winning_tile);
}

bool IsSevenPairs(const std::vector<Tile>& concealed_with_winning_tile) {
  if (concealed_with_winning_tile.size() != 14) return false;
  const FaceCounts counts = CountFaces(concealed_with_winning_tile);
  int pairs = 0;
  for (uint8_t c : counts) {
    if (c == 0) continue;
    if (c != 2) return false;  // 4 张同牌面不算两对（RULESET §3.3）
    ++pairs;
  }
  return pairs == 7;
}

bool IsPureOneSuit(const std::vector<Tile>& concealed_with_winning_tile,
                   const std::vector<Meld>& melds) {
  // 清一色只对**数牌某一门**成立（RULESET §3.4）：含任何字牌即判否（本里程碑无「字一色」）。
  // 若不排除字牌，全字牌手牌会被判为清一色并拿到错误的 ×4 倍率，直接影响结算与零和守恒。
  uint8_t suit = 0xff;
  auto absorb = [&suit](const std::vector<Tile>& tiles) {
    for (Tile t : tiles) {
      const Face face = FaceOf(t);
      if (IsHonorFace(face)) return false;
      const uint8_t s = SuitOf(face);
      if (suit == 0xff) {
        suit = s;
      } else if (suit != s) {
        return false;
      }
    }
    return true;
  };
  if (!absorb(concealed_with_winning_tile)) return false;
  for (const Meld& meld : melds) {
    if (!absorb(meld.tiles)) return false;
  }
  return suit != 0xff;
}

std::vector<std::string> WinTags(const std::vector<Tile>& concealed_with_winning_tile,
                                 const std::vector<Meld>& melds) {
  std::vector<std::string> tags;
  const bool seven_pairs =
      melds.empty() && KanCountOf(melds) == 0 && IsSevenPairs(concealed_with_winning_tile);
  if (seven_pairs) tags.emplace_back("seven_pairs");
  if (IsPureOneSuit(concealed_with_winning_tile, melds)) tags.emplace_back("pure_one_suit");
  return tags;
}

// ---------------------------------------------------------------------------
// 动作可选项（RULESET §5）
// ---------------------------------------------------------------------------
std::vector<Option> ClaimOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                 Tile discarded, int remaining, bool is_next_player) {
  std::vector<Option> options;
  if (CanWinWith(hand, melds, discarded)) options.push_back(Option{"win", {}});
  if (CanKong(hand, discarded, remaining)) options.push_back(Option{"kong", {discarded}});
  if (CanPong(hand, discarded)) options.push_back(Option{"pong", {discarded}});
  if (is_next_player) {
    for (const auto& combo : ChowCombinations(hand, discarded)) {
      options.push_back(Option{"chow", {combo[0], combo[1]}});
    }
  }
  options.push_back(Option{"pass", {}});
  return options;
}

std::vector<Option> DiscardOptions(const std::vector<Tile>& concealed_with_winning_tile,
                                   const std::vector<Meld>& melds) {
  std::vector<Option> options;
  if (CanWinExact(concealed_with_winning_tile, melds)) {
    options.push_back(Option{"win", {}});
  }
  return options;
}

bool HasNonPassOption(const std::vector<Option>& options) {
  for (const Option& option : options) {
    if (option.action != "pass") return true;
  }
  return false;
}

std::optional<Tile> DefaultDiscard(const std::vector<Tile>& hand,
                                   std::optional<Tile> drawn_tile) {
  if (hand.empty()) return std::nullopt;
  if (drawn_tile.has_value()) {
    const Face drawn_face = FaceOf(*drawn_tile);
    for (Tile tile : hand) {
      if (FaceOf(tile) == drawn_face) return tile;  // 副本不可区分，取同面即可
    }
  }
  std::vector<Tile> sorted = SortedTiles(hand);
  return sorted.back();  // 排序后最后一张（最大牌）
}

// ---------------------------------------------------------------------------
// AI 弃牌策略：向听数 + 牌效率（t20）
//
// 准确性优先于速度：手牌 ≤ 14 张，用「最低牌面锚定 + 跳过」的暴力 DFS 枚举
// （面子 / 将 / 两面搭子 / 嵌张搭子 / 对子搭子），取最优分解。
// ---------------------------------------------------------------------------
namespace {

constexpr int kMaxShanten = 8;  // 最差情况（起手全无搭子）

// 标准型向听 DFS 上下文
struct ShantenCtx {
  int fixed_melds = 0;  // 副露已有的面子数
  int best = kMaxShanten;
};

// 用一个分解（melds 组面子 + partials 组搭子 + 是否有将）刷新最优向听
inline void UpdateShanten(ShantenCtx& ctx, int melds, int partials, bool has_pair) {
  if (melds + partials > 4) partials = 4 - melds;  // 面子/搭子合计 ≤ 4（将另计）
  if (partials < 0) partials = 0;
  const int s = 8 - 2 * (ctx.fixed_melds + melds) - partials - (has_pair ? 1 : 0);
  if (s < ctx.best) ctx.best = s;
}

// min_face 为「允许使用的最小牌面」：跳过某牌面后不再回头，保证终止。
void ShantenDfs(ShantenCtx& ctx, FaceCounts& counts, int min_face, int melds, int partials,
                bool has_pair, int target) {
  UpdateShanten(ctx, melds, partials, has_pair);
  if (ctx.best <= target) return;     // 已达目标（用于提前退出）
  if (melds + partials >= 5) return;  // 5 块已满（4 面子/搭子 + 将）

  int f = -1;
  for (int i = min_face; i < kFaceCount; ++i) {
    if (counts[i] != 0) {
      f = i;
      break;
    }
  }
  if (f < 0) return;

  const bool number = IsNumberFace(static_cast<Face>(f));
  const int rank0 = f % kRankCount;  // 0-based

  // A. 刻子（数牌与字牌都可用）
  if (counts[f] >= 3) {
    counts[f] -= 3;
    ShantenDfs(ctx, counts, f, melds + 1, partials, has_pair, target);
    counts[f] += 3;
  }
  // B. 顺子（**仅数牌**；字牌编号连续但不可成顺）
  if (number && rank0 <= kRankCount - 3 && counts[f + 1] != 0 && counts[f + 2] != 0) {
    counts[f] -= 1;
    counts[f + 1] -= 1;
    counts[f + 2] -= 1;
    ShantenDfs(ctx, counts, f, melds + 1, partials, has_pair, target);
    counts[f] += 1;
    counts[f + 1] += 1;
    counts[f + 2] += 1;
  }
  // C. 将（对子作头，最多一次）
  if (!has_pair && counts[f] >= 2) {
    counts[f] -= 2;
    ShantenDfs(ctx, counts, f, melds, partials, /*has_pair=*/true, target);
    counts[f] += 2;
  }
  // D. 两面/边张搭子（f, f+1）
  if (number && rank0 <= kRankCount - 2 && counts[f + 1] != 0) {
    counts[f] -= 1;
    counts[f + 1] -= 1;
    ShantenDfs(ctx, counts, f, melds, partials + 1, has_pair, target);
    counts[f] += 1;
    counts[f + 1] += 1;
  }
  // E. 嵌张搭子（f, f+2）
  if (number && rank0 <= kRankCount - 3 && counts[f + 2] != 0) {
    counts[f] -= 1;
    counts[f + 2] -= 1;
    ShantenDfs(ctx, counts, f, melds, partials + 1, has_pair, target);
    counts[f] += 1;
    counts[f + 2] += 1;
  }
  // F. 对子搭子（对子不作头，作为一组搭子）
  if (counts[f] >= 2) {
    counts[f] -= 2;
    ShantenDfs(ctx, counts, f, melds, partials + 1, has_pair, target);
    counts[f] += 2;
  }
  // G. 跳过该牌面（浮牌：如单张孤字牌）
  if (counts[f] != 0) {
    const uint8_t saved = counts[f];
    counts[f] = 0;
    ShantenDfs(ctx, counts, f + 1, melds, partials, has_pair, target);
    counts[f] = saved;
  }
}

// 七对子向听（仅门前清）：6 − 对数
int SevenPairsShanten(const FaceCounts& counts) {
  int pairs = 0;
  int distinct = 0;
  for (uint8_t c : counts) {
    if (c == 0) continue;
    ++distinct;
    if (c >= 2) ++pairs;
  }
  if (distinct < 7) return kMaxShanten;  // 牌面种类不足 7 种，七对子不成立
  if (pairs > 7) pairs = 7;
  return 6 - pairs;
}

}  // namespace

int Shanten(const std::vector<Tile>& concealed, const std::vector<Meld>& melds) {
  const int fixed_melds = MeldCountOf(melds);
  const FaceCounts counts = CountFaces(concealed);

  ShantenCtx ctx;
  ctx.fixed_melds = fixed_melds;
  FaceCounts work = counts;
  ShantenDfs(ctx, work, 0, 0, 0, /*has_pair=*/false, /*target=*/-1);
  int best = ctx.best;

  // 七对子只在门前清时成立（本里程碑无暗杠，故只判 melds 为空）。
  if (melds.empty()) {
    const int seven = SevenPairsShanten(counts);
    if (seven < best) best = seven;
  }
  return best;
}

namespace {

// 带 target 的版本：一旦结果 ≤ target 立即返回（用于剪枝）
int BestShantenAfterDrawImpl(const std::vector<Tile>& hand_after_discard,
                             const std::vector<Meld>& melds, int immediate, int target) {
  // 摸一张最多让向听改善 1 ⇒ immediate−1 是理论下界，达下界即可停止扫描。
  const FaceCounts own = CountFaces(hand_after_discard);
  std::vector<Face> order;
  order.reserve(kFaceCount);
  const auto push = [&order](int f) {
    if (f < 0 || f >= kFaceCount) return;
    const Face face = static_cast<Face>(f);
    if (std::find(order.begin(), order.end(), face) == order.end()) order.push_back(face);
  };
  // 先试「手里已有的牌」（可能补成对子/刻子）与「同门邻居」（可能补成顺子），最后兜底扫全部
  for (int f = 0; f < kFaceCount; ++f) {
    if (own[f] != 0) push(f);
  }
  for (int f = 0; f < kNumericFaceCount; ++f) {
    if (own[f] == 0) continue;
    const int rank0 = f % kRankCount;
    if (rank0 <= kRankCount - 2) push(f + 1);
    if (rank0 >= 1) push(f - 1);
    if (rank0 <= kRankCount - 3) push(f + 2);
    if (rank0 >= 2) push(f - 2);
  }
  for (int f = 0; f < kFaceCount; ++f) push(f);

  std::vector<Tile> probe = hand_after_discard;
  probe.push_back(Tile{0});  // 占位，逐面替换
  for (Face f : order) {
    probe.back() = MakeTileFromFace(f);
    const int s = Shanten(probe, melds);
    if (s <= target) return s;
  }
  return immediate;  // 没有可改善的牌（极端情形）
}

}  // namespace

int BestShantenAfterDraw(const std::vector<Tile>& hand_after_discard,
                         const std::vector<Meld>& melds) {
  const int immediate = Shanten(hand_after_discard, melds);
  return BestShantenAfterDrawImpl(hand_after_discard, melds, immediate, immediate - 1);
}

std::optional<Tile> ChooseDiscard(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                  std::optional<Tile> drawn_tile) {
  if (hand.empty()) return std::nullopt;

  // 1) 去重候选 + 每个候选「打牌后」的当前向听
  struct Candidate {
    Face face;
    int immediate;
    std::vector<Tile> rest;  // 打出该面后的门前手牌
  };
  std::vector<Candidate> candidates;
  for (Tile t : hand) {
    const Face f = FaceOf(t);
    bool seen = false;
    for (const Candidate& c : candidates) {
      if (c.face == f) {
        seen = true;
        break;
      }
    }
    if (seen) continue;
    Candidate cand;
    cand.face = f;
    bool removed = false;
    for (Tile x : hand) {
      if (!removed && FaceOf(x) == f) {
        removed = true;
        continue;
      }
      cand.rest.push_back(x);
    }
    cand.immediate = Shanten(cand.rest, melds);
    candidates.push_back(std::move(cand));
  }
  // 升序排列（评分并列时取更小牌面 ⇒ 冻结牌序），并按 immediate 升序做剪枝
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.immediate != b.immediate) return a.immediate < b.immediate;
    return a.face < b.face;
  });

  // 2) 依次精确评分。可证明安全的剪枝：摸一张最多改善 1 ⇒ score ≥ immediate−1；
  //    若 immediate−1 已经 > 当前最优，则后续候选（immediate 更大）都不可能更优，直接结束。
  int best_score = kMaxShanten + 2;
  Face best_face = candidates.front().face;
  for (const Candidate& cand : candidates) {
    if (cand.immediate - 1 > best_score) break;
    const int score = BestShantenAfterDrawImpl(cand.rest, melds, cand.immediate,
                                               std::min(best_score - 1, cand.immediate - 1));
    if (score < best_score) {
      best_score = score;
      best_face = cand.face;
    }
  }

  // 返回手牌中该牌面的实际牌；若该面正是刚摸到的牌，则返回摸到的这张（保留「打摸牌」语义）。
  // 无摸牌记录（nullopt）时按纯牌效率返回手牌中该面的牌。
  if (drawn_tile.has_value() && FaceOf(*drawn_tile) == best_face) return *drawn_tile;
  for (Tile t : hand) {
    if (FaceOf(t) == best_face) return t;
  }
  return hand.front();  // 理论不可达（best_face 必来自 hand）
}

std::optional<Tile> ChooseDiscard(const std::vector<Tile>& hand, const std::vector<Meld>& melds) {
  return ChooseDiscard(hand, melds, std::nullopt);
}

int BestDiscardShanten(const std::vector<Tile>& hand, const std::vector<Meld>& melds) {
  if (hand.empty()) return kMaxShanten;
  int best = kMaxShanten + 1;
  std::vector<Tile> rest = hand;
  std::vector<Face> seen;
  for (Tile t : hand) {
    const Face f = FaceOf(t);
    if (std::find(seen.begin(), seen.end(), f) != seen.end()) continue;
    seen.push_back(f);
    rest = hand;
    for (auto it = rest.begin(); it != rest.end(); ++it) {
      if (FaceOf(*it) == f) {
        rest.erase(it);
        break;
      }
    }
    const int s = Shanten(rest, melds);
    if (s < best) best = s;
  }
  return best;
}

std::optional<int> ShantenAfterMeld(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                    Tile claimed, const std::vector<Tile>& consume,
                                    MeldType type) {
  const Face claimed_face = FaceOf(claimed);
  const FaceCounts hand_counts = CountFaces(hand);

  // ---- 1) 校验 consume 合法可用，并算出需要从手牌扣除的牌面 ----
  FaceCounts to_remove{};
  to_remove.fill(0);
  std::vector<Tile> meld_tiles;
  switch (type) {
    case MeldType::kPong:
    case MeldType::kKong: {
      const int need = (type == MeldType::kPong) ? 2 : 3;
      if (consume.size() != static_cast<size_t>(need)) return std::nullopt;
      for (Tile t : consume) {
        if (FaceOf(t) != claimed_face) return std::nullopt;
      }
      if (hand_counts[claimed_face] < need) return std::nullopt;
      to_remove[claimed_face] = static_cast<uint8_t>(need);
      for (int i = 0; i < need; ++i) meld_tiles.push_back(claimed);
      meld_tiles.push_back(claimed);
      break;
    }
    case MeldType::kChow: {
      if (consume.size() != 2) return std::nullopt;
      const Face a = FaceOf(consume[0]);
      const Face b = FaceOf(consume[1]);
      if (a == b || !IsNumberFace(a) || !IsNumberFace(b)) return std::nullopt;
      std::vector<Face> trio = {a, b, claimed_face};
      std::sort(trio.begin(), trio.end());
      if (SuitOf(trio[0]) != SuitOf(trio[2])) return std::nullopt;           // 必须同一门数牌
      if (trio[1] != trio[0] + 1 || trio[2] != trio[1] + 1) return std::nullopt;  // 必须连续
      if (hand_counts[a] < 1 || hand_counts[b] < 1) return std::nullopt;
      to_remove[a] += 1;
      to_remove[b] += 1;
      meld_tiles = {consume[0], consume[1], claimed};
      break;
    }
  }

  // ---- 2) 从手牌扣除 consume（按牌面计数）----
  std::vector<Tile> concealed;
  concealed.reserve(hand.size());
  FaceCounts budget = to_remove;
  for (Tile t : hand) {
    const Face f = FaceOf(t);
    if (budget[f] > 0) {
      budget[f] -= 1;
      continue;
    }
    concealed.push_back(t);
  }

  std::vector<Meld> melds_after = melds;
  melds_after.push_back(Meld{type, SortedTiles(meld_tiles)});

  // ---- 3) 鸣牌后必须打一张；明杠还要补摸一张（取最优补牌，乐观口径）----
  if (type == MeldType::kKong) {
    int best = kMaxShanten + 1;
    std::vector<Tile> probe = concealed;
    probe.push_back(Tile{0});
    for (int f = 0; f < kFaceCount; ++f) {
      probe.back() = MakeTileFromFace(static_cast<Face>(f));
      const int s = BestDiscardShanten(probe, melds_after);
      if (s < best) best = s;
      if (best <= -1) break;
    }
    return best;
  }
  return BestDiscardShanten(concealed, melds_after);
}

// ---------------------------------------------------------------------------
// 算分（RULESET §6）
// ---------------------------------------------------------------------------
int MultiplierForTags(const std::vector<std::string>& tags) {
  int multiplier = 1;  // 底分 base = 1
  for (const std::string& tag : tags) {
    if (tag == "seven_pairs" || tag == "pure_one_suit") multiplier *= 4;
  }
  return multiplier;
}

ScoreResult ScoreWin(int winner_seat, int from_seat,
                     const std::vector<Tile>& concealed_with_winning_tile,
                     const std::vector<Meld>& melds) {
  ScoreResult result;
  const std::vector<std::string> tags =
      WinTags(concealed_with_winning_tile, melds);
  result.multiplier = MultiplierForTags(tags);

  if (from_seat < 0) {
    // 自摸：三家各付 2M，和牌者 +6M
    result.per_payer = result.multiplier * 2;
    result.total = result.multiplier * 6;
    for (int seat = 0; seat < 4; ++seat) {
      if (seat == winner_seat) continue;
      result.delta[static_cast<size_t>(seat)] -= result.per_payer;
    }
    result.delta[static_cast<size_t>(winner_seat)] += result.total;
  } else {
    // 荣和：放炮者付 M
    result.per_payer = result.multiplier;
    result.total = result.multiplier;
    result.delta[static_cast<size_t>(from_seat)] -= result.multiplier;
    result.delta[static_cast<size_t>(winner_seat)] += result.multiplier;
  }
  return result;
}

ScoreResult ScoreDraw() { return ScoreResult{}; }

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
const Option* FindOption(const std::vector<Option>& options, std::string_view action) {
  for (const Option& option : options) {
    if (option.action == action) return &option;
  }
  return nullptr;
}

bool OptionTilesMatch(const Option& option, const std::vector<Tile>& expected) {
  if (option.tiles.size() != expected.size()) return false;
  std::vector<Tile> a = SortedTiles(option.tiles);
  std::vector<Tile> b = SortedTiles(expected);
  for (size_t i = 0; i < a.size(); ++i) {
    if (FaceOf(a[i]) != FaceOf(b[i])) return false;  // 只比较牌面（副本不可区分）
  }
  return true;
}

}  // namespace mahjong
