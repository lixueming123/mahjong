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
  if (IsNumberFace(static_cast<Face>(first)) && rank <= kRankCount - 3 && counts[first + 1] != 0 &&
      counts[first + 2] != 0) {
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
  return Tile {static_cast<uint8_t>(face * kIdsPerFace + (copy % kCopiesPerFace))};
}

Tile MakeTile(uint8_t suit, uint8_t rank, uint8_t copy) {
  const Face face = static_cast<Face>(suit * kRankCount + (rank - 1));
  return MakeTileFromFace(face, copy);
}

bool IsRedFive(Tile tile) {
  const Face face = FaceOf(tile);
  return SuitOf(face) != kHonorSuit && RankOf(face) == 5 && CopyOf(tile) == kRedFiveCopy;
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
  // 合取正则（PROTOCOL.md §2.1）：`^((0|[1-9])[mps]|[1-7]z)$`
  if (text.size() != 2) return std::nullopt;
  const char rank_char = text[0];
  const char suit_char = text[1];
  if (rank_char == '0') {
    // 赤 5（v1.18）：`0m` / `0p` / `0s`，解析成该面 copy == kRedFiveCopy 的那一张物理牌。
    uint8_t suit = 0;
    if (suit_char == 'm') {
      suit = 0;
    } else if (suit_char == 'p') {
      suit = 1;
    } else if (suit_char == 's') {
      suit = 2;
    } else {
      return std::nullopt;  // 0z 非法
    }
    return MakeTile(suit, 5, kRedFiveCopy);
  }
  if (rank_char < '1' || rank_char > '9') return std::nullopt;
  if (suit_char == 'z') {
    // 字牌仅 1z..7z（8z/9z 非法）
    if (rank_char > '7') return std::nullopt;
    const Face face = static_cast<Face>(kNumericFaceCount + (rank_char - '1'));  // 27..33
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

std::string FormatTile(Tile tile, bool red_fives) {
  const Face face = FaceOf(tile);
  std::string out;
  if (red_fives && IsRedFive(tile)) {
    out.push_back('0');
  } else {
    out.push_back(static_cast<char>('0' + RankOf(face)));
  }
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
  FaceCounts counts {};
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

Deal DealTiles(const std::vector<Tile>& wall, int dealer) {
  Deal deal;
  deal.wall = wall;
  if (dealer < 0 || dealer >= 4) dealer = 0;  // 越界按 0 处理（不抛错：规则库不制造异常路径）
  size_t index = 0;
  for (int seat = 0; seat < 4; ++seat) {
    for (int i = 0; i < kInitialHandSize && index < wall.size(); ++i) {
      deal.hands[static_cast<size_t>(seat)].push_back(wall[index++]);
    }
  }
  // 庄家第 14 张（B1：庄家由参数指定，不再硬编码 hands[0]）
  if (index < wall.size()) deal.hands[static_cast<size_t>(dealer)].push_back(wall[index++]);
  for (auto& hand : deal.hands) SortTiles(hand);
  deal.next_index = static_cast<int>(index);
  deal.remaining = static_cast<int>(wall.size()) - static_cast<int>(index);
  return deal;
}

int NextDealer(int dealer, RoundResult result) {
  if (result == RoundResult::kDealerWin || result == RoundResult::kDrawExhausted) {
    return dealer;  // 连庄：庄家不变，hand_no 也不动（GAME_DESIGN_V2 §3.3 判据 A/B）
  }
  return (dealer + 1) % 4;  // 轮庄（判据 C）
}

RoundResult ClassifyRound(protocol::RoundEndReason reason, int winner_seat, int dealer) {
  // 荒庄没有和牌者，一律视为庄家荒庄 ⇒ 连庄（GAME_DESIGN_V2 §3.3 关键点 4 的显式消歧）
  if (reason == protocol::RoundEndReason::kDrawExhausted) return RoundResult::kDrawExhausted;
  return winner_seat == dealer ? RoundResult::kDealerWin : RoundResult::kNonDealerWin;
}

// ---------------------------------------------------------------------------
// 副露
// ---------------------------------------------------------------------------
// 三种杠都是「1 组、账面 3 张、第 4 张由 kans 补」—— 张数公式对三者完全一致（见头文件注释）
bool IsKanType(MeldType type) {
  return type == MeldType::kKong || type == MeldType::kAnkan || type == MeldType::kKakan;
}

// 门清 = melds 中不含 pong/chow/kong/kakan。**ankan 不破门清**（GAME_DESIGN_V2 §1.2 #1）。
// kakan 必然起于 pong，故「kakan 破门清」是结构结论，这里只是把它写死一次。
bool BreaksMenzen(MeldType type) {
  switch (type) {
    case MeldType::kAnkan:
      return false;
    case MeldType::kPong:
    case MeldType::kChow:
    case MeldType::kKong:
    case MeldType::kKakan:
      return true;
  }
  return true;
}

int MeldCountOf(const std::vector<Meld>& melds) { return static_cast<int>(melds.size()); }

int KanCountOf(const std::vector<Meld>& melds) {
  int kans = 0;
  for (const Meld& meld : melds) {
    if (IsKanType(meld.type)) ++kans;  // 明杠 + 暗杠 + 加杠（旧「仅明杠」口径作废）
  }
  return kans;
}

bool IsValidMeld(const Meld& meld) {
  if (!IsVerifiedHand(meld.tiles)) return false;
  const FaceCounts counts = CountFaces(meld.tiles);
  switch (meld.type) {
    case MeldType::kPong:
    case MeldType::kKong:
    case MeldType::kAnkan:
    case MeldType::kKakan: {
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

std::vector<Face> AnkanFaces(const std::vector<Tile>& hand) {
  const FaceCounts counts = CountFaces(hand);
  std::vector<Face> faces;
  for (int f = 0; f < kFaceCount; ++f) {
    if (counts[f] == 4) faces.push_back(static_cast<Face>(f));
  }
  return faces;
}

bool CanAnkan(const std::vector<Tile>& hand) { return !AnkanFaces(hand).empty(); }

std::vector<int> KakanMeldIndices(const std::vector<Tile>& hand, const std::vector<Meld>& melds) {
  const FaceCounts counts = CountFaces(hand);
  std::vector<int> indices;
  for (size_t i = 0; i < melds.size(); ++i) {
    const Meld& meld = melds[i];
    if (meld.type != MeldType::kPong || meld.tiles.empty()) continue;
    const Face face = FaceOf(meld.tiles.front());
    if (counts[face] >= 1) indices.push_back(static_cast<int>(i));
  }
  return indices;
}

bool CanKakan(const std::vector<Tile>& hand, const std::vector<Meld>& melds) {
  return !KakanMeldIndices(hand, melds).empty();
}

namespace {

// 从手牌中移除 `count` 张 `face`（副本不可区分，取前 count 张即可）
std::vector<Tile> RemoveFace(const std::vector<Tile>& hand, Face face, int count) {
  std::vector<Tile> out;
  out.reserve(hand.size());
  int remaining = count;
  for (Tile t : hand) {
    if (remaining > 0 && FaceOf(t) == face) {
      --remaining;
      continue;
    }
    out.push_back(t);
  }
  return out;
}

}  // namespace

std::optional<KongResult> ApplyAnkan(const std::vector<Tile>& hand, const std::vector<Meld>& melds, Face face) {
  std::vector<Tile> tiles;
  for (Tile t : hand) {
    if (FaceOf(t) == face) tiles.push_back(t);
  }
  if (tiles.size() != 4) return std::nullopt;  // 必须恰 4 张（5 张已由 IsVerifiedHand 排除）
  KongResult result;
  result.hand = RemoveFace(hand, face, 4);
  result.melds = melds;
  result.melds.push_back(Meld {.type = MeldType::kAnkan, .tiles = SortedTiles(tiles)});
  return result;
}

std::optional<KongResult> ApplyKakan(const std::vector<Tile>& hand, const std::vector<Meld>& melds, int meld_index) {
  if (meld_index < 0 || meld_index >= MeldCountOf(melds)) return std::nullopt;
  const Meld& target = melds[static_cast<size_t>(meld_index)];
  if (target.type != MeldType::kPong || target.tiles.empty()) return std::nullopt;
  const Face face = FaceOf(target.tiles.front());
  // 赤 5（v1.18）：优先把**普通 5** 拿去加杠，赤牌留在手里（与房间侧 `TakeFaces` 同口径）。
  std::optional<Tile> fourth;
  for (Tile t : hand) {
    if (FaceOf(t) != face) continue;
    if (!fourth.has_value() || (IsRedFive(*fourth) && !IsRedFive(t))) {
      fourth = t;
    }
  }
  if (!fourth.has_value()) return std::nullopt;

  KongResult result;
  // 扣掉的必须是上面选中的那一张（赤 5 与普通 5 是两张不同的物理牌，不能按牌面随便扣）
  result.hand = hand;
  const auto chosen = std::find(result.hand.begin(), result.hand.end(), *fourth);
  if (chosen == result.hand.end()) return std::nullopt;
  result.hand.erase(chosen);
  result.melds = melds;
  Meld& upgraded = result.melds[static_cast<size_t>(meld_index)];
  upgraded.type = MeldType::kKakan;   // 就地升级：组序不变、MeldCountOf 不变
  upgraded.tiles.push_back(*fourth);  // 第 4 张追加在末尾
  SortTiles(upgraded.tiles);
  return result;
}

bool CanPong(const std::vector<Tile>& hand, Tile discarded) { return CountFaces(hand)[FaceOf(discarded)] >= 2; }

bool CanKong(const std::vector<Tile>& hand, Tile discarded, int remaining) {
  return remaining >= 1 && CountFaces(hand)[FaceOf(discarded)] == 3;
}

std::vector<std::array<Tile, 2>> ChowCombinations(const std::vector<Tile>& hand, Tile discarded) {
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

bool CanChow(const std::vector<Tile>& hand, Tile discarded) { return !ChowCombinations(hand, discarded).empty(); }

// ---------------------------------------------------------------------------
// 和牌判定（RULESET §3）
// ---------------------------------------------------------------------------
bool CanWinExact(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds) {
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

bool CanWinWith(const std::vector<Tile>& concealed, const std::vector<Meld>& melds, Tile winning_tile) {
  std::vector<Tile> tiles = concealed;
  tiles.push_back(winning_tile);
  return CanWinExact(tiles, melds);
}

bool IsWinningHand(const std::vector<Tile>& concealed, const std::vector<Meld>& melds, Tile winning_tile,
                   bool is_tsumo) {
  if (is_tsumo) return CanWinExact(concealed, melds);
  return CanWinWith(concealed, melds, winning_tile);
}

std::vector<Tile> WaitingTiles(const std::vector<Tile>& concealed, const std::vector<Meld>& melds) {
  std::vector<Tile> waits;
  // 语义边界（见头文件）：只在等待态（13 逻辑张）有定义；14 张（行动态）与其它张数一律空。
  if (concealed.size() + 3 * melds.size() != 13) {
    return waits;
  }
  for (int face = 0; face < kFaceCount; ++face) {
    std::vector<Tile> with_win = concealed;
    with_win.push_back(MakeTileFromFace(static_cast<Face>(face)));
    if (CanWinExact(with_win, melds)) {
      waits.push_back(MakeTileFromFace(static_cast<Face>(face)));
    }
  }
  // face 升序即为 §2.2 冻结牌序（m<p<s<z、rank 升序），无需再排。
  return waits;
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

bool IsPureOneSuit(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds) {
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

// ---------------------------------------------------------------------------
// 全拆法枚举（GAME_DESIGN_V2 §1.1 口径 2）
// ---------------------------------------------------------------------------
namespace {

// 副露组 → Group。chow 是顺子；pong / kong / ankan / kakan 都是「刻子类」的一组。
Group GroupOfMeld(const Meld& meld, int meld_index) {
  Group group;
  group.from_meld = true;
  group.meld_index = meld_index;
  if (meld.type == MeldType::kChow && !meld.tiles.empty()) {
    group.is_run = true;
    Face lowest = FaceOf(meld.tiles.front());
    for (Tile t : meld.tiles) lowest = std::min(lowest, FaceOf(t));
    group.face = lowest;
  } else {
    group.is_run = false;
    group.face = meld.tiles.empty() ? Face {} : FaceOf(meld.tiles.front());
  }
  return group;
}

// 回溯序 = §1.3.4：最小未用牌面锚定，刻子分支先于顺子分支
void CollectDecompositions(FaceCounts& counts, int need_groups, std::vector<Group>& pending,
                           const std::array<Group, 4>& meld_groups, int meld_count, std::vector<Decomposition>& out) {
  if (need_groups == 0) {
    for (uint8_t c : counts) {
      if (c != 0) return;
    }
    Decomposition decomposition;
    int index = 0;
    for (int i = 0; i < meld_count; ++i) {
      decomposition.groups[static_cast<size_t>(index++)] = meld_groups[static_cast<size_t>(i)];
    }
    for (const Group& group : pending) {
      decomposition.groups[static_cast<size_t>(index++)] = group;
    }
    out.push_back(decomposition);
    return;
  }
  int first = -1;
  for (int f = 0; f < kFaceCount; ++f) {
    if (counts[static_cast<size_t>(f)] != 0) {
      first = f;
      break;
    }
  }
  if (first < 0) return;

  // A. 刻子（先于顺子：与既有 Decompose 的 A/B 分支次序逐字一致）
  if (counts[static_cast<size_t>(first)] >= 3) {
    counts[static_cast<size_t>(first)] -= 3;
    pending.push_back(Group {.from_meld = false, .meld_index = -1, .is_run = false, .face = static_cast<Face>(first)});
    CollectDecompositions(counts, need_groups - 1, pending, meld_groups, meld_count, out);
    pending.pop_back();
    counts[static_cast<size_t>(first)] += 3;
  }
  // B. 顺子（字牌编号连续但绝不成顺，故用 IsNumberFace 兜住，理由同 Decompose）
  const int rank = first % kRankCount;  // 0-based
  if (IsNumberFace(static_cast<Face>(first)) && rank <= kRankCount - 3 && counts[static_cast<size_t>(first + 1)] != 0 &&
      counts[static_cast<size_t>(first + 2)] != 0) {
    counts[static_cast<size_t>(first)] -= 1;
    counts[static_cast<size_t>(first + 1)] -= 1;
    counts[static_cast<size_t>(first + 2)] -= 1;
    pending.push_back(Group {.from_meld = false, .meld_index = -1, .is_run = true, .face = static_cast<Face>(first)});
    CollectDecompositions(counts, need_groups - 1, pending, meld_groups, meld_count, out);
    pending.pop_back();
    counts[static_cast<size_t>(first)] += 1;
    counts[static_cast<size_t>(first + 1)] += 1;
    counts[static_cast<size_t>(first + 2)] += 1;
  }
}

}  // namespace

std::vector<Decomposition> EnumerateDecompositions(const std::vector<Tile>& concealed_with_winning_tile,
                                                   const std::vector<Meld>& melds) {
  std::vector<Decomposition> out;
  const int meld_count = MeldCountOf(melds);
  const int need_groups = 4 - meld_count;
  if (need_groups < 0) return out;
  if (concealed_with_winning_tile.size() != static_cast<size_t>(need_groups * 3 + 2)) return out;
  if (!IsVerifiedHand(concealed_with_winning_tile)) return out;

  std::array<Group, 4> meld_groups {};
  for (int i = 0; i < meld_count; ++i) {
    meld_groups[static_cast<size_t>(i)] = GroupOfMeld(melds[static_cast<size_t>(i)], i);
  }
  FaceCounts work = CountFaces(concealed_with_winning_tile);
  std::vector<Group> pending;
  pending.reserve(static_cast<size_t>(need_groups));
  // ① 将牌牌面按面编号升序逐个尝试
  for (int f = 0; f < kFaceCount; ++f) {
    if (work[static_cast<size_t>(f)] < 2) continue;
    work[static_cast<size_t>(f)] -= 2;
    const size_t before = out.size();
    CollectDecompositions(work, need_groups, pending, meld_groups, meld_count, out);
    for (size_t i = before; i < out.size(); ++i) {
      out[i].pair = static_cast<Face>(f);
    }
    work[static_cast<size_t>(f)] += 2;
  }
  return out;
}

namespace {

bool GroupContainsFace(const Group& group, Face face) {
  if (!group.is_run) return group.face == face;
  const int offset = static_cast<int>(face) - static_cast<int>(group.face);
  return offset >= 0 && offset < 3;
}

}  // namespace

int WinGroupIndex(const Decomposition& decomposition, Face winning_face) {
  // 顺子组优先：荣和时「能和成顺子就按顺子算」，即三暗刻取对玩家最有利的落组
  for (int i = 0; i < 4; ++i) {
    const Group& group = decomposition.groups[static_cast<size_t>(i)];
    if (group.is_run && GroupContainsFace(group, winning_face)) return i;
  }
  for (int i = 0; i < 4; ++i) {
    const Group& group = decomposition.groups[static_cast<size_t>(i)];
    if (!group.is_run && group.face == winning_face) return i;
  }
  if (decomposition.pair == winning_face) return kWinInPair;
  return -1;
}

// ---------------------------------------------------------------------------
// 判番（GAME_DESIGN_V2 §1.2）
// ---------------------------------------------------------------------------
namespace {

// 5z(白) / 6z(发) / 7z(中) —— 面 31..33
bool IsDragonFace(Face face) { return face >= kNumericFaceCount + 4 && face < kFaceCount; }

struct HandFacts {
  bool menzen = false;
  bool tanyao = false;
  bool honitsu = false;
  bool pure = false;
  bool rinshan = false;
};

HandFacts ComputeFacts(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                       const ScoreContext& ctx) {
  HandFacts facts;
  facts.menzen = true;
  for (const Meld& meld : melds) {
    if (BreaksMenzen(meld.type)) facts.menzen = false;
  }
  std::vector<Tile> all = concealed_with_winning_tile;
  for (const Meld& meld : melds) all.insert(all.end(), meld.tiles.begin(), meld.tiles.end());

  bool tanyao = true;
  int suit = -1;
  bool single_suit = true;
  bool any_honor = false;
  for (Tile tile : all) {
    const Face face = FaceOf(tile);
    if (IsHonorFace(face)) {
      any_honor = true;
      tanyao = false;
      continue;  // 字牌不参与「数牌门数」的统计
    }
    const uint8_t tile_suit = SuitOf(face);
    if (suit < 0) {
      suit = tile_suit;
    } else if (suit != tile_suit) {
      single_suit = false;
    }
    const uint8_t rank = RankOf(face);
    if (rank == 1 || rank == kRankCount) tanyao = false;
  }
  facts.tanyao = tanyao;
  // 混一色：**恰含一门数牌** 且 至少 1 张字牌（全字牌不算，§1.4）
  facts.honitsu = single_suit && suit >= 0 && any_honor;
  // 清一色：沿用既有 IsPureOneSuit（零字牌 + 单门数牌），与 honitsu 结构互斥
  facts.pure = IsPureOneSuit(concealed_with_winning_tile, melds);
  facts.rinshan = ctx.Tsumo() && ctx.is_rinshan;
  return facts;
}

struct GroupView {
  bool is_run = false;
  bool concealed = true;  // 暗：门前组 + ankan。pong / chow / kong / kakan 都是明
  Face face = 0;
};

GroupView ViewOfGroup(const Group& group, const std::vector<Meld>& melds) {
  GroupView view;
  view.is_run = group.is_run;
  view.face = group.face;
  if (group.from_meld && group.meld_index >= 0 && group.meld_index < static_cast<int>(melds.size())) {
    view.concealed = (melds[static_cast<size_t>(group.meld_index)].type == MeldType::kAnkan);
  } else {
    view.concealed = true;
  }
  return view;
}

void PushYaku(std::vector<Yaku>& yaku, protocol::YakuId id, int han) {
  if (han <= 0) return;
  yaku.push_back(Yaku {.id = id, .han = han});
}

// 单个拆法的番种列表（**按 §1.2 表序**，同一 id 至多一条）
std::vector<Yaku> YakuOfDecomposition(const Decomposition& decomposition, const std::vector<Meld>& melds,
                                      const HandFacts& facts, const ScoreContext& ctx, Face winning_face) {
  std::array<GroupView, 4> groups {};
  for (int i = 0; i < 4; ++i) {
    groups[static_cast<size_t>(i)] = ViewOfGroup(decomposition.groups[static_cast<size_t>(i)], melds);
  }

  bool all_runs = true;
  bool all_triplets = true;
  int yakuhai = 0;
  int ankou = 0;
  std::array<uint8_t, kFaceCount> run_starts {};
  run_starts.fill(0);
  for (const GroupView& group : groups) {
    if (group.is_run) {
      all_triplets = false;
      run_starts[static_cast<size_t>(group.face)]++;
    } else {
      all_runs = false;
      if (IsDragonFace(group.face)) ++yakuhai;
      if (group.concealed) ++ankou;  // 暗刻 = ankan + 门前刻子（排除 pong/kong/kakan）
    }
  }

  // 三暗刻：荣和时，由和牌张补成的那一组若是刻子**不计**暗刻（落组口径见 WinGroupIndex）
  if (!ctx.Tsumo() && ankou > 0) {
    const int win_group = WinGroupIndex(decomposition, winning_face);
    if (win_group >= 0 && win_group < 4) {
      const GroupView& group = groups[static_cast<size_t>(win_group)];
      if (!group.is_run && group.concealed) --ankou;
    }
  }

  // 三色同顺：m/p/s 各一组、起点 rank 相同的顺子
  bool sanshoku = false;
  for (int rank0 = 0; rank0 <= kRankCount - 3 && !sanshoku; ++rank0) {
    bool all_suits = true;
    for (int suit = 0; suit < kSuitCount; ++suit) {
      if (run_starts[static_cast<size_t>(suit * kRankCount + rank0)] == 0) all_suits = false;
    }
    sanshoku = all_suits;
  }
  // 一气通贯：同一门的 123 + 456 + 789
  bool ittsuu = false;
  for (int suit = 0; suit < kSuitCount && !ittsuu; ++suit) {
    const size_t base = static_cast<size_t>(suit * kRankCount);
    ittsuu = run_starts[base] != 0 && run_starts[base + 3] != 0 && run_starts[base + 6] != 0;
  }
  // 一杯口：门清且两组同花色同起点的顺子（no ryanpeikou：多条也只计 1 番）
  bool iipeikou = false;
  if (facts.menzen) {
    for (uint8_t count : run_starts) {
      if (count >= 2) iipeikou = true;
    }
  }
  // 小三元：白发中两组为刻子/杠、第三组为将牌
  int dragon_triplets = 0;
  for (const GroupView& group : groups) {
    if (!group.is_run && IsDragonFace(group.face)) ++dragon_triplets;
  }
  const bool shousangen = dragon_triplets == 2 && IsDragonFace(decomposition.pair);

  std::vector<Yaku> yaku;
  // ---- 以下 push 次序 = §1.2 表序，不得调整 ----
  PushYaku(yaku, protocol::YakuId::kMenzen, facts.menzen ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kTanyao, facts.tanyao ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kPinfu, (facts.menzen && all_runs && IsNumberFace(decomposition.pair)) ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kIipeikou, iipeikou ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kYakuhai, yakuhai);  // 多条合并为一条，han = 组数（§1.3.3）
  PushYaku(yaku, protocol::YakuId::kRinshan, facts.rinshan ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kSanshokuDoujun, sanshoku ? 2 : 0);
  PushYaku(yaku, protocol::YakuId::kIttsuu, ittsuu ? 2 : 0);
  PushYaku(yaku, protocol::YakuId::kToitoi, all_triplets ? 2 : 0);
  PushYaku(yaku, protocol::YakuId::kSanankou, ankou >= 3 ? 2 : 0);
  PushYaku(yaku, protocol::YakuId::kHonitsu, facts.honitsu ? 2 : 0);
  PushYaku(yaku, protocol::YakuId::kShousangen, shousangen ? 2 : 0);
  // 一色组：两边都判，绝不双命中（结构上互斥，§1.3.2 —— 不写互斥分支）
  PushYaku(yaku, protocol::YakuId::kPureOneSuit, facts.pure ? 5 : 0);
  return yaku;
}

// 七对子形态的番种（与顺子类/刻子类天然互斥，只可能与 #1/#2/#6/#11/#14 叠加）
std::vector<Yaku> YakuOfSevenPairs(const HandFacts& facts) {
  std::vector<Yaku> yaku;
  PushYaku(yaku, protocol::YakuId::kMenzen, facts.menzen ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kTanyao, facts.tanyao ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kRinshan, facts.rinshan ? 1 : 0);
  PushYaku(yaku, protocol::YakuId::kHonitsu, facts.honitsu ? 2 : 0);
  PushYaku(yaku, protocol::YakuId::kSevenPairs, 2);
  PushYaku(yaku, protocol::YakuId::kPureOneSuit, facts.pure ? 5 : 0);
  return yaku;
}

}  // namespace

int YakuHan(protocol::YakuId id) {
  // 冻结番数表（GAME_DESIGN_V2 §1.2）；yakuhai 返回**单组**的 1 番
  switch (id) {
    case protocol::YakuId::kMenzen:
    case protocol::YakuId::kTanyao:
    case protocol::YakuId::kPinfu:
    case protocol::YakuId::kIipeikou:
    case protocol::YakuId::kYakuhai:
    case protocol::YakuId::kRinshan:
      return 1;
    case protocol::YakuId::kSanshokuDoujun:
    case protocol::YakuId::kIttsuu:
    case protocol::YakuId::kToitoi:
    case protocol::YakuId::kSanankou:
    case protocol::YakuId::kHonitsu:
    case protocol::YakuId::kShousangen:
    case protocol::YakuId::kSevenPairs:
      return 2;
    case protocol::YakuId::kPureOneSuit:
      return 5;
    default:
      return 0;
  }
}

int YakuHanTotal(const std::vector<Yaku>& yaku) {
  int total = 0;
  for (const Yaku& item : yaku) total += item.han;
  return total;
}

std::vector<Yaku> DetectYaku(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                             Tile winning_tile, const ScoreContext& ctx) {
  const HandFacts facts = ComputeFacts(concealed_with_winning_tile, melds, ctx);
  const Face winning_face = FaceOf(winning_tile);

  // 口径 3：han(手牌) = max over 拆法 Σ番种(D)，禁止跨拆法拼凑；同分取枚举序第一个
  std::vector<Yaku> best;
  int best_han = 0;  // 无起和番：0 番合法
  for (const Decomposition& decomposition : EnumerateDecompositions(concealed_with_winning_tile, melds)) {
    std::vector<Yaku> yaku = YakuOfDecomposition(decomposition, melds, facts, ctx, winning_face);
    const int han = YakuHanTotal(yaku);
    if (han > best_han) {
      best_han = han;
      best = std::move(yaku);
    }
  }
  if (melds.empty() && IsSevenPairs(concealed_with_winning_tile)) {
    std::vector<Yaku> yaku = YakuOfSevenPairs(facts);
    const int han = YakuHanTotal(yaku);
    if (han > best_han) {
      best_han = han;
      best = std::move(yaku);
    }
  }
  return best;
}

std::vector<protocol::YakuId> WinTags(const std::vector<Tile>& concealed_with_winning_tile,
                                      const std::vector<Meld>& melds, std::optional<Tile> winning_tile,
                                      std::optional<ScoreContext> ctx) {
  ScoreContext effective = ctx.value_or(ScoreContext {});
  Tile tile = Tile {};
  if (winning_tile.has_value()) {
    tile = *winning_tile;
  } else if (!concealed_with_winning_tile.empty()) {
    tile = concealed_with_winning_tile.front();
  }
  std::vector<protocol::YakuId> tags;
  for (const Yaku& yaku : DetectYaku(concealed_with_winning_tile, melds, tile, effective)) {
    tags.push_back(yaku.id);
  }
  return tags;
}

// ---------------------------------------------------------------------------
// 动作可选项（RULESET §5）
// ---------------------------------------------------------------------------
std::vector<Option> ClaimOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds, Tile discarded,
                                 int remaining, bool is_next_player) {
  std::vector<Option> options;
  if (CanWinWith(hand, melds, discarded)) options.push_back(Option {.action = protocol::Action::kWin, .tiles = {}});
  if (CanKong(hand, discarded, remaining))
    options.push_back(Option {.action = protocol::Action::kKong, .tiles = {discarded}});
  if (CanPong(hand, discarded)) options.push_back(Option {.action = protocol::Action::kPong, .tiles = {discarded}});
  if (is_next_player) {
    for (const auto& combo : ChowCombinations(hand, discarded)) {
      options.push_back(Option {.action = protocol::Action::kChow, .tiles = {combo[0], combo[1]}});
    }
  }
  options.push_back(Option {.action = protocol::Action::kPass, .tiles = {}});
  return options;
}

std::vector<Option> DiscardOptions(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                                   int remaining) {
  std::vector<Option> options;
  if (CanWinExact(concealed_with_winning_tile, melds)) {
    options.push_back(Option {.action = protocol::Action::kWin, .tiles = {}});
  }
  // 暗杠 / 加杠只在**本家行动态**（逻辑 14 张）提供，且需要 ≥ 1 张补牌
  // （`remaining < 1` 时不提供，与 CanKong 的守卫同源，GAME_DESIGN_V2 §4.2/§4.3）。
  const size_t logical = concealed_with_winning_tile.size() + 3u * static_cast<size_t>(MeldCountOf(melds));
  // 顺序固定 win → ankan → kakan（R6）；组内按牌面升序 / meld 下标升序，保持确定性。
  // B1.1 裁决 G：ankan/kakan 的选项 tiles 均为**恰好 1 个牌面元素**（与 kong 同形，
  // PROTOCOL v1.6 §4.6 冻结：元素多于 1 个 ⇒ ILLEGAL_ACTION）。
  if (logical == 14u && remaining >= 1) {
    for (Face face : AnkanFaces(concealed_with_winning_tile)) {
      options.push_back(Option {.action = protocol::Action::kAnkan, .tiles = {MakeTileFromFace(face)}});
    }
    for (int meld_index : KakanMeldIndices(concealed_with_winning_tile, melds)) {
      const Meld& meld = melds[static_cast<size_t>(meld_index)];
      const Face face = FaceOf(meld.tiles.front());
      Tile fourth {};
      for (Tile tile : concealed_with_winning_tile) {
        if (FaceOf(tile) == face) {
          fourth = tile;
          break;
        }
      }
      options.push_back(Option {.action = protocol::Action::kKakan, .tiles = {fourth}});
    }
  }
  return options;
}

bool HasNonPassOption(const std::vector<Option>& options) {
  for (const Option& option : options) {
    if (option.action != protocol::Action::kPass) return true;
  }
  return false;
}

std::optional<Tile> DefaultDiscard(const std::vector<Tile>& hand, std::optional<Tile> drawn_tile) {
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
void ShantenDfs(ShantenCtx& ctx, FaceCounts& counts, int min_face, int melds, int partials, bool has_pair, int target) {
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
int BestShantenAfterDrawImpl(const std::vector<Tile>& hand_after_discard, const std::vector<Meld>& melds, int immediate,
                             int target) {
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
  probe.push_back(Tile {0});  // 占位，逐面替换
  for (Face f : order) {
    probe.back() = MakeTileFromFace(f);
    const int s = Shanten(probe, melds);
    if (s <= target) return s;
  }
  return immediate;  // 没有可改善的牌（极端情形）
}

}  // namespace

int BestShantenAfterDraw(const std::vector<Tile>& hand_after_discard, const std::vector<Meld>& melds) {
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
    const int score =
        BestShantenAfterDrawImpl(cand.rest, melds, cand.immediate, std::min(best_score - 1, cand.immediate - 1));
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

std::optional<int> ShantenAfterMeld(const std::vector<Tile>& hand, const std::vector<Meld>& melds, Tile claimed,
                                    const std::vector<Tile>& consume, MeldType type) {
  const Face claimed_face = FaceOf(claimed);
  const FaceCounts hand_counts = CountFaces(hand);

  // ---- 1) 校验 consume 合法可用，并算出需要从手牌扣除的牌面 ----
  FaceCounts to_remove {};
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
      if (SuitOf(trio[0]) != SuitOf(trio[2])) return std::nullopt;                // 必须同一门数牌
      if (trio[1] != trio[0] + 1 || trio[2] != trio[1] + 1) return std::nullopt;  // 必须连续
      if (hand_counts[a] < 1 || hand_counts[b] < 1) return std::nullopt;
      to_remove[a] += 1;
      to_remove[b] += 1;
      meld_tiles = {consume[0], consume[1], claimed};
      break;
    }
    case MeldType::kAnkan:
    case MeldType::kKakan:
      // 暗杠/加杠不是「鸣他人打出的牌」的路径（它们是本家行动态的自主动作），
      // 本函数只建模鸣牌门槛；暗杠/加杠后的最优向听不在本批范围内。
      return std::nullopt;
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
  melds_after.push_back(Meld {.type = type, .tiles = SortedTiles(meld_tiles)});

  // ---- 3) 鸣牌后必须打一张；明杠还要补摸一张（取最优补牌，乐观口径）----
  if (type == MeldType::kKong) {
    int best = kMaxShanten + 1;
    std::vector<Tile> probe = concealed;
    probe.push_back(Tile {0});
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
// 算分（GAME_DESIGN_V2 §2）
// ---------------------------------------------------------------------------
namespace {

constexpr int kHanCap = 6;  // 封顶 6 番（庄家加成受同一封顶，§2.5）

bool HasYaku(const std::vector<Yaku>& yaku, protocol::YakuId id) {
  for (const Yaku& item : yaku) {
    if (item.id == id) return true;
  }
  return false;
}

}  // namespace

int MultiplierForHan(int han) {
  if (han > kHanCap) han = kHanCap;
  if (han < 0) han = 0;
  return 1 << han;  // R = 2^han ∈ {1, 2, 4, 8, 16, 32, 64}
}

ScoreResult ScoreHand(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                      Tile winning_tile, const ScoreContext& ctx) {
  ScoreResult result;
  if (ctx.winner_seat < 0 || ctx.winner_seat >= 4) return result;  // 非法座位 ⇒ 全 0（不制造异常）

  result.yaku = DetectYaku(concealed_with_winning_tile, melds, winning_tile, ctx);
  result.dealer_bonus = ctx.winner_is_dealer ? 1 : 0;
  result.rinshan = HasYaku(result.yaku, protocol::YakuId::kRinshan);

  // han = 番种合计 + 庄家(1)，受**同一**封顶（B1.1 裁决：自摸不是番种，不计入合计；
  // "自摸 ×2" 已并入下方支付公式的 2R/6R）
  int han = YakuHanTotal(result.yaku) + result.dealer_bonus;
  if (han > kHanCap) han = kHanCap;
  if (han < 0) han = 0;
  result.han = han;
  result.multiplier = MultiplierForHan(han);

  if (ctx.Tsumo()) {
    // 自摸：三家各付 2R，和牌者 +6R（3×(−2R) + 6R = 0）
    result.per_payer = result.multiplier * 2;
    result.total = result.multiplier * 6;
    for (int seat = 0; seat < 4; ++seat) {
      if (seat == ctx.winner_seat) continue;
      result.delta[static_cast<size_t>(seat)] -= result.per_payer;
    }
    result.delta[static_cast<size_t>(ctx.winner_seat)] += result.total;
  } else {
    if (ctx.from_seat < 0 || ctx.from_seat >= 4) return result;  // 荣和必须给出合法放炮者
    // 荣和：放炮者 −R、和牌者 +R（−R + R = 0）
    result.per_payer = result.multiplier;
    result.total = result.multiplier;
    result.delta[static_cast<size_t>(ctx.from_seat)] -= result.multiplier;
    result.delta[static_cast<size_t>(ctx.winner_seat)] += result.multiplier;
  }
  return result;
}

ScoreResult ScoreWin(int winner_seat, int from_seat, const std::vector<Tile>& concealed_with_winning_tile,
                     const std::vector<Meld>& melds, std::optional<Tile> winning_tile) {
  ScoreContext ctx;
  ctx.winner_seat = winner_seat;
  ctx.from_seat = from_seat;
  ctx.winner_is_dealer = false;  // 旧 4 参签名无庄家信息；B2 用 ScoreHand 传 ScoreContext
  ctx.is_rinshan = false;
  const Tile tile =
      winning_tile.value_or(concealed_with_winning_tile.empty() ? Tile {} : concealed_with_winning_tile.front());
  return ScoreHand(concealed_with_winning_tile, melds, tile, ctx);
}

ScoreResult ScoreDraw() { return ScoreResult {}; }

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
const Option* FindOption(const std::vector<Option>& options, protocol::Action action) {
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
