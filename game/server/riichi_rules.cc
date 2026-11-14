// 日麻规则库实现。见 riichi_rules.h 顶部说明。
//
// 判番流程（与 classic 一致的「全拆法枚举」骨架）：
//   1. 役满检查（独立于番数，命中即只算役满，可叠加）；
//   2. 七对子（独立形态，固定 25 符）；
//   3. 标准型：EnumerateDecompositions 的每个拆法各算一遍「番 + 符 → 点数」，取点数最高者。
// 取「点数最高」而不是「番最高」：同番不同符时，日麻的实际收益按点数比较。

#include "riichi_rules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mahjong {
namespace riichi {
namespace {

using protocol::YakuId;

constexpr int kSuitFaces = kRankCount;  // 每门 9 个牌面
constexpr int kDragonBase = 31;         // 5z 白 / 6z 发 / 7z 中

bool IsTerminalFace(Face face) { return IsNumberFace(face) && (RankOf(face) == 1 || RankOf(face) == 9); }

bool IsHonor(Face face) { return face >= kNumericFaceCount; }

bool IsTerminalOrHonor(Face face) { return IsTerminalFace(face) || IsHonor(face); }

bool IsDragon(Face face) { return face >= kDragonBase && face < kFaceCount; }

bool IsWindFace(Face face) { return face >= kNumericFaceCount && face < kDragonBase; }

Face WindFaceOf(int wind) { return static_cast<Face>(kNumericFaceCount + (wind % 4)); }

// 绿一色：2s/3s/4s/6s/8s + 发（6z）
bool IsGreenFace(Face face) {
  if (face == 32) {
    return true;
  }
  if (!IsNumberFace(face) || SuitOf(face) != 2) {
    return false;
  }
  const uint8_t rank = RankOf(face);
  return rank == 2 || rank == 3 || rank == 4 || rank == 6 || rank == 8;
}

int Ceil100(int value) { return (value + 99) / 100 * 100; }

int RoundUp10(int value) { return (value + 9) / 10 * 10; }

bool IsKanMeld(MeldType type) {
  return type == MeldType::kKong || type == MeldType::kAnkan || type == MeldType::kKakan;
}

struct GroupInfo {
  bool is_run = false;
  bool from_meld = false;
  bool kan = false;
  bool open = false;  // 明面（顺子恒为明；刻子看副露类型，暗杠不算明）
  Face face = 0;
};

struct HandFacts {
  bool closed = false;
  bool has_terminal_or_honor = false;
  bool all_terminals_honors = true;  // 全部牌张都是幺九 / 字牌
  bool only_terminals = true;        // 只有老头牌（无字牌）
  bool all_honors = true;            // 只有字牌
  bool all_green = true;
  bool has_honor = false;
  bool single_suit = false;  // 只有一门数牌
  int suit = -1;
  int kans = 0;
};

enum class WaitKind : uint8_t { kNone, kRyanmen, kKanchan, kPenchan, kShanpon, kTanki };

struct PointResult {
  int base = 0;
  int points = 0;
  int per_payer = 0;
  Limit limit = Limit::kNone;
  std::array<int, 4> delta {};
};

GroupInfo InfoOf(const Group& group, const std::vector<Meld>& melds) {
  GroupInfo info;
  info.is_run = group.is_run;
  info.from_meld = group.from_meld;
  info.face = group.face;
  info.open = group.is_run;
  if (group.from_meld && group.meld_index >= 0 && static_cast<size_t>(group.meld_index) < melds.size()) {
    const MeldType type = melds[static_cast<size_t>(group.meld_index)].type;
    info.kan = IsKanMeld(type);
    info.open = group.is_run || type != MeldType::kAnkan;
  }
  return info;
}

HandFacts ComputeFacts(const std::vector<Tile>& concealed, const std::vector<Meld>& melds) {
  HandFacts facts;
  facts.closed = IsClosedHand(melds);
  facts.kans = KanCountOf(melds);

  std::array<int, kFaceCount> counts {};
  for (const Tile& tile : concealed) {
    counts[static_cast<size_t>(FaceOf(tile))] += 1;
  }
  for (const Meld& meld : melds) {
    for (const Tile& tile : meld.tiles) {
      counts[static_cast<size_t>(FaceOf(tile))] += 1;
    }
  }

  int suit_mask = 0;
  for (int face = 0; face < kFaceCount; ++face) {
    if (counts[static_cast<size_t>(face)] == 0) {
      continue;
    }
    const Face f = static_cast<Face>(face);
    if (!IsGreenFace(f)) {
      facts.all_green = false;
    }
    if (IsNumberFace(f)) {
      suit_mask |= 1 << SuitOf(f);
      facts.all_honors = false;
      if (IsTerminalFace(f)) {
        facts.has_terminal_or_honor = true;
      } else {
        facts.all_terminals_honors = false;
        facts.only_terminals = false;
      }
    } else {
      facts.has_honor = true;
      facts.has_terminal_or_honor = true;
      facts.only_terminals = false;
    }
  }
  // 只有一门数牌：suit_mask 里恰好一位
  facts.single_suit = suit_mask != 0 && (suit_mask & (suit_mask - 1)) == 0;
  if (facts.single_suit) {
    for (int suit = 0; suit < kSuitCount; ++suit) {
      if ((suit_mask & (1 << suit)) != 0) {
        facts.suit = suit;
      }
    }
  }
  return facts;
}

// 和牌张落在哪一组、是什么待ち形。顺子优先于刻子、刻子优先于将牌（与 WinGroupIndex 同口径）。
WaitKind WaitOf(const Decomposition& decomp, Face winning) {
  for (const Group& group : decomp.groups) {
    if (!group.is_run) {
      continue;
    }
    const uint8_t start = group.face;
    if (SuitOf(start) != SuitOf(winning)) {
      continue;
    }
    const uint8_t rank = RankOf(start);
    const uint8_t win_rank = RankOf(winning);
    if (rank > 7 || win_rank < rank || win_rank > rank + 2) {
      continue;
    }
    const int offset = win_rank - rank;
    if (offset == 1) {
      return WaitKind::kKanchan;
    }
    if (offset == 0) {
      return rank == 7 ? WaitKind::kPenchan : WaitKind::kRyanmen;  // 789 等 7
    }
    return rank == 1 ? WaitKind::kPenchan : WaitKind::kRyanmen;  // 123 等 3
  }
  for (const Group& group : decomp.groups) {
    if (!group.is_run && group.face == winning) {
      return WaitKind::kShanpon;
    }
  }
  if (decomp.pair == winning) {
    return WaitKind::kTanki;
  }
  return WaitKind::kNone;
}

bool PairIsYakuhai(Face pair, const WinContext& ctx) {
  return IsDragon(pair) || pair == WindFaceOf(ctx.round_wind) || pair == WindFaceOf(ctx.seat_wind);
}

// 单拆法的番种（不含宝牌、不含役满）。顺序 = 判定顺序 = 渲染顺序。
std::vector<Yaku> YakuForDecomposition(const Decomposition& decomp, const std::vector<Meld>& melds, Face winning,
                                       const WinContext& ctx, const HandFacts& facts, WaitKind wait, bool* out_pinfu) {
  std::vector<Yaku> yaku;
  auto push = [&yaku](YakuId id, int han) {
    if (han > 0) {
      yaku.push_back(Yaku {.id = id, .han = han});
    }
  };
  const bool closed = facts.closed;
  const bool tsumo = ctx.Tsumo();

  std::array<int, kSuitCount * kRankCount> run_starts {};
  int run_count = 0;
  int concealed_triplets = 0;
  int dragon_triplets = 0;
  int wind_triplets = 0;
  int yakuhai_han = 0;
  bool all_groups_terminal = true;        // 每一组都含老头牌
  bool all_groups_terminal_honor = true;  // 每一组都含幺九或字牌
  bool all_runs = true;

  for (const Group& group : decomp.groups) {
    const GroupInfo info = InfoOf(group, melds);
    if (info.is_run) {
      ++run_count;
      const uint8_t rank = RankOf(info.face);
      run_starts[static_cast<size_t>(SuitOf(info.face) * kSuitFaces + (rank - 1))] += 1;
      if (rank != 1 && rank != 7) {
        all_groups_terminal = false;
        all_groups_terminal_honor = false;
      }
      continue;
    }

    all_runs = false;
    const Face face = info.face;
    if (!IsTerminalOrHonor(face)) {
      all_groups_terminal = false;
      all_groups_terminal_honor = false;
    }
    if (!info.open) {
      const bool ron_completed = !tsumo && face == winning;
      if (!ron_completed) {
        ++concealed_triplets;
      }
    }
    if (IsDragon(face)) {
      ++dragon_triplets;
      ++yakuhai_han;
    } else if (IsWindFace(face)) {
      ++wind_triplets;
      if (face == WindFaceOf(ctx.round_wind)) {
        ++yakuhai_han;
      }
      if (face == WindFaceOf(ctx.seat_wind)) {
        ++yakuhai_han;  // 连风牌计 2 番
      }
    }
  }

  // ---- 立直系（全部要求门前清）----
  if (closed) {
    push(YakuId::kRiichi, ctx.is_riichi ? 1 : 0);
    push(YakuId::kIppatsu, (ctx.is_riichi && ctx.is_ippatsu) ? 1 : 0);
    push(YakuId::kTsumo, tsumo ? 1 : 0);  // 门前清自摸和
  }
  push(YakuId::kRinshan, ctx.is_rinshan ? 1 : 0);
  push(YakuId::kChankan, ctx.is_chankan ? 1 : 0);
  push(YakuId::kHaitei, (ctx.is_haitei && tsumo) ? 1 : 0);
  push(YakuId::kHoutei, (ctx.is_houtei && !tsumo) ? 1 : 0);

  // ---- 断幺九 ----
  push(YakuId::kTanyao, facts.has_terminal_or_honor ? 0 : 1);

  // ---- 平和：门前 + 四顺子 + 非役牌将 + 两面待ち ----
  const bool pinfu = closed && all_runs && !PairIsYakuhai(decomp.pair, ctx) && wait == WaitKind::kRyanmen;
  push(YakuId::kPinfu, pinfu ? 1 : 0);
  if (out_pinfu != nullptr) {
    *out_pinfu = pinfu;
  }

  // ---- 一杯口 / 二杯口 ----
  if (closed) {
    int repeated = 0;
    for (int count : run_starts) {
      if (count >= 2) {
        ++repeated;
      }
    }
    if (repeated >= 2) {
      push(YakuId::kRyanpeikou, 3);
    } else if (repeated == 1) {
      push(YakuId::kIipeikou, 1);
    }
  }

  // ---- 役牌 ----
  push(YakuId::kYakuhai, yakuhai_han);

  // ---- 三色同顺 / 一气通贯 ----
  bool sanshoku = false;
  for (int rank = 0; rank < kRankCount - 2; ++rank) {
    if (run_starts[static_cast<size_t>(rank)] > 0 && run_starts[static_cast<size_t>(kSuitFaces + rank)] > 0 &&
        run_starts[static_cast<size_t>(2 * kSuitFaces + rank)] > 0) {
      sanshoku = true;
    }
  }
  push(YakuId::kSanshokuDoujun, sanshoku ? (closed ? 2 : 1) : 0);

  bool ittsuu = false;
  for (int suit = 0; suit < kSuitCount; ++suit) {
    const size_t base = static_cast<size_t>(suit * kSuitFaces);
    if (run_starts[base] > 0 && run_starts[base + 3] > 0 && run_starts[base + 6] > 0) {
      ittsuu = true;
    }
  }
  push(YakuId::kIttsuu, ittsuu ? (closed ? 2 : 1) : 0);

  // ---- 混全带幺九 / 纯全带幺九（必须有顺子，否则走混老头）----
  const bool pair_terminal_or_honor = IsTerminalOrHonor(decomp.pair);
  if (run_count > 0 && pair_terminal_or_honor && all_groups_terminal_honor) {
    if (!facts.has_honor && IsTerminalFace(decomp.pair) && all_groups_terminal) {
      push(YakuId::kJunchan, closed ? 3 : 2);
    } else if (facts.has_honor) {
      push(YakuId::kChanta, closed ? 2 : 1);
    }
  }

  // ---- 对对和 / 三暗刻 / 三杠子 / 小三元 ----
  push(YakuId::kToitoi, run_count == 0 ? 2 : 0);
  push(YakuId::kSanankou, concealed_triplets >= 3 ? 2 : 0);
  push(YakuId::kSankantsu, facts.kans >= 3 ? 2 : 0);
  push(YakuId::kShousangen, (dragon_triplets == 2 && IsDragon(decomp.pair)) ? 2 : 0);

  // ---- 混老头 ----
  if (facts.all_terminals_honors && run_count == 0) {
    push(YakuId::kHonroutou, 2);
  }

  // ---- 混一色 / 清一色 ----
  if (facts.single_suit && facts.suit >= 0) {
    if (facts.has_honor) {
      push(YakuId::kHonitsu, closed ? 3 : 2);
    } else {
      push(YakuId::kChinitsu, closed ? 6 : 5);
    }
  }

  return yaku;
}

// 四暗刻单骑：把和牌张从 14 张里去掉后，剩 13 张应能写成「4 组暗刻 + 1 张孤张」。
// 这种形态只有唯一一种拆法（4×3 + 1，不含顺子），所以按计数直接判即可：
// 双碰荣和（3 暗刻 + 2 对子）去掉一张后是「3 组刻子 + 1 对 + 1 孤张」，count == 2 会把它挡掉。
bool IsTankiWait(const std::vector<Tile>& concealed, Tile winning) {
  std::array<int, kFaceCount> counts {};
  for (const Tile& tile : concealed) {
    counts[static_cast<size_t>(FaceOf(tile))] += 1;
  }
  const size_t win_face = static_cast<size_t>(FaceOf(winning));
  if (counts[win_face] == 0) {
    return false;
  }
  counts[win_face] -= 1;
  int triplets = 0;
  int singles = 0;
  for (int count : counts) {
    if (count == 0) {
      continue;
    }
    if (count == 3) {
      ++triplets;
    } else if (count == 1) {
      ++singles;
    } else {
      return false;
    }
  }
  return triplets == 4 && singles == 1;
}

// 纯正九莲宝灯（九面听）：去掉和牌张后恰好是 1112345678999（3-1-1-1-1-1-1-1-3）。
bool IsJunseiChuuren(const std::vector<Tile>& concealed, Tile winning, uint8_t suit) {
  static constexpr int kBase[9] = {3, 1, 1, 1, 1, 1, 1, 1, 3};
  std::array<int, 9> counts_by_rank {};
  for (const Tile& tile : concealed) {
    const Face face = FaceOf(tile);
    if (SuitOf(face) == suit) {
      counts_by_rank[static_cast<size_t>(RankOf(face) - 1)] += 1;
    }
  }
  const size_t win_rank = static_cast<size_t>(RankOf(FaceOf(winning)) - 1);
  if (counts_by_rank[win_rank] == 0) {
    return false;
  }
  counts_by_rank[win_rank] -= 1;
  for (int i = 0; i < 9; ++i) {
    if (counts_by_rank[static_cast<size_t>(i)] != kBase[i]) {
      return false;
    }
  }
  return true;
}

// 役满（独立于番数；同一手可叠加多个役满）。双倍役满用 `YakumanMultiplier` 折算：
// 每条款目的 han = 13 × 倍数（`kokushi_13` / `suuankou_tanki` / `chuuren_junsei` / `daisuushii` = 26），
// 这样 `win.han_breakdown.base`（Σ yaku.han）与 `win.han` 仍然相等，客户端不必知道「倍数」这回事。
std::vector<Yaku> DetectYakuman(const std::vector<Tile>& concealed, const std::vector<Meld>& melds, Tile winning,
                                const WinContext& ctx) {
  std::vector<Yaku> result;
  const bool tsumo = ctx.Tsumo();
  const HandFacts facts = ComputeFacts(concealed, melds);
  auto push = [&result](YakuId id) { result.push_back(Yaku {.id = id, .han = 13 * YakumanMultiplier(id)}); };

  if (facts.all_honors) {
    push(YakuId::kTsuuiisou);
  }
  if (facts.all_green) {
    push(YakuId::kRyuuiisou);
  }
  if (facts.only_terminals) {
    push(YakuId::kChinroutou);
  }
  if (facts.kans >= 4) {
    push(YakuId::kSuukantsu);
  }
  // 天和 / 地和：都是「第一巡自摸」的役满，由房间按牌河/副露状态置位（见 BuildRiichiContext）。
  if (ctx.is_tenhou) {
    push(YakuId::kTenhou);
  }
  if (ctx.is_chiihou) {
    push(YakuId::kChiihou);
  }

  if (melds.empty()) {
    // 国士无双：十三种幺九字各至少一张 + 其中一张成对
    static constexpr Face kOrphans[13] = {0, 8, 9, 17, 18, 26, 27, 28, 29, 30, 31, 32, 33};
    std::array<int, kFaceCount> counts {};
    for (const Tile& tile : concealed) {
      counts[static_cast<size_t>(FaceOf(tile))] += 1;
    }
    bool only_orphans = true;
    bool all_present = true;
    bool has_pair = false;
    for (int face = 0; face < kFaceCount; ++face) {
      const bool orphan = std::find(std::begin(kOrphans), std::end(kOrphans), face) != std::end(kOrphans);
      if (!orphan && counts[static_cast<size_t>(face)] > 0) {
        only_orphans = false;
      }
    }
    for (Face face : kOrphans) {
      const int count = counts[static_cast<size_t>(face)];
      if (count == 0) {
        all_present = false;
      }
      if (count == 2) {
        has_pair = true;
      }
    }
    if (only_orphans && all_present && has_pair) {
      // 国士十三面：和牌张正是那张「对子」⇒ 和牌前 13 张是十三种各一张（十三面听）。
      push(counts[static_cast<size_t>(FaceOf(winning))] == 2 ? YakuId::kKokushi13 : YakuId::kKokushi);
      return result;  // 国士无双不与其它役满复合
    }

    // 九莲宝灯：门前清、一门、1112345678999 + 任意一张
    if (facts.single_suit && facts.suit >= 0) {
      static constexpr int kBase[9] = {3, 1, 1, 1, 1, 1, 1, 1, 3};
      std::array<int, 9> counts_by_rank {};
      for (const Tile& tile : concealed) {
        const Face face = FaceOf(tile);
        if (SuitOf(face) == facts.suit) {
          counts_by_rank[static_cast<size_t>(RankOf(face) - 1)] += 1;
        }
      }
      bool ok = true;
      for (int i = 0; i < 9; ++i) {
        if (counts_by_rank[static_cast<size_t>(i)] < kBase[i]) {
          ok = false;
        }
      }
      if (ok) {
        push(IsJunseiChuuren(concealed, winning, static_cast<uint8_t>(facts.suit)) ? YakuId::kChuurenJunsei
                                                                                   : YakuId::kChuuren);
      }
    }
  }

  // 刻子类役满要走拆法（只看第一个拆法：多拆法不会改变「是否有 4 个暗刻」这类事实）
  for (const Decomposition& decomp : EnumerateDecompositions(concealed, melds)) {
    int concealed_triplets = 0;
    int dragon_triplets = 0;
    int wind_triplets = 0;
    for (const Group& group : decomp.groups) {
      if (group.is_run) {
        continue;
      }
      const GroupInfo info = InfoOf(group, melds);
      if (!info.open) {
        const bool ron_completed = !tsumo && info.face == FaceOf(winning);
        if (!ron_completed) {
          ++concealed_triplets;
        }
      }
      if (IsDragon(info.face)) {
        ++dragon_triplets;
      }
      if (IsWindFace(info.face)) {
        ++wind_triplets;
      }
    }
    if (concealed_triplets >= 4) {
      // 四暗刻单骑：单骑（和牌张进将）比双碰多一番，记双倍役满。
      push(melds.empty() && IsTankiWait(concealed, winning) ? YakuId::kSuuankouTanki : YakuId::kSuuankou);
    }
    if (dragon_triplets >= 3) {
      push(YakuId::kDaisangen);
    }
    if (wind_triplets >= 4) {
      push(YakuId::kDaisuushii);
    } else if (wind_triplets == 3 && IsWindFace(decomp.pair)) {
      push(YakuId::kShousuushii);
    }
    break;
  }

  return result;
}

// 七对子形态（独立于标准型；固定 25 符）
std::vector<Yaku> YakuOfSevenPairs(const WinContext& ctx, const HandFacts& facts) {
  std::vector<Yaku> yaku;
  auto push = [&yaku](YakuId id, int han) {
    if (han > 0) {
      yaku.push_back(Yaku {.id = id, .han = han});
    }
  };
  push(YakuId::kRiichi, (facts.closed && ctx.is_riichi) ? 1 : 0);
  push(YakuId::kIppatsu, (facts.closed && ctx.is_riichi && ctx.is_ippatsu) ? 1 : 0);
  push(YakuId::kTsumo, (facts.closed && ctx.Tsumo()) ? 1 : 0);
  push(YakuId::kChiitoitsu, 2);
  push(YakuId::kTanyao, facts.has_terminal_or_honor ? 0 : 1);
  if (facts.single_suit && facts.suit >= 0) {
    push(YakuId::kHonitsu, facts.has_honor ? 3 : 0);
    push(YakuId::kChinitsu, facts.has_honor ? 0 : 6);
  }
  return yaku;
}

int FuForDecomposition(const Decomposition& decomp, const std::vector<Meld>& melds, const HandFacts& facts,
                       WaitKind wait, bool pinfu, bool tsumo, const WinContext& ctx) {
  int fu = 20;
  for (const Group& group : decomp.groups) {
    const GroupInfo info = InfoOf(group, melds);
    if (info.is_run) {
      continue;
    }
    const bool terminal = IsTerminalOrHonor(info.face);
    if (info.kan) {
      fu += info.open ? (terminal ? 16 : 8) : (terminal ? 32 : 16);
    } else {
      fu += info.open ? (terminal ? 4 : 2) : (terminal ? 8 : 4);
    }
  }
  if (IsDragon(decomp.pair)) {
    fu += 2;
  }
  if (decomp.pair == WindFaceOf(ctx.round_wind)) {
    fu += 2;
  }
  if (decomp.pair == WindFaceOf(ctx.seat_wind)) {
    fu += 2;
  }
  if (wait == WaitKind::kKanchan || wait == WaitKind::kPenchan || wait == WaitKind::kTanki) {
    fu += 2;
  }
  if (pinfu && tsumo) {
    return 20;  // 平和自摸固定 20 符
  }
  if (tsumo) {
    fu += 2;
  }
  if (facts.closed && !tsumo) {
    fu += 10;
  }
  if (!facts.closed && fu == 20) {
    fu = 30;  // 副露的 20 符按 30 符
  }
  return RoundUp10(fu);
}

// 按 base 计算支付（役满与普通手共用）
PointResult Pay(int base, bool dealer, bool tsumo, int winner_seat, int from_seat, int dealer_seat, int honba) {
  PointResult out;
  out.base = base;
  if (tsumo) {
    const int from_dealer = Ceil100(2 * base) + 100 * honba;
    const int from_other = Ceil100(base) + 100 * honba;
    if (dealer) {
      out.per_payer = from_dealer;
      for (int seat = 0; seat < 4; ++seat) {
        if (seat == winner_seat) {
          continue;
        }
        out.delta[static_cast<size_t>(seat)] -= from_dealer;
      }
      out.points = from_dealer * 3;
    } else {
      out.per_payer = from_other;
      for (int seat = 0; seat < 4; ++seat) {
        if (seat == winner_seat) {
          continue;
        }
        out.delta[static_cast<size_t>(seat)] -= seat == dealer_seat ? from_dealer : from_other;
      }
      out.points = from_dealer + from_other * 2;
    }
    out.delta[static_cast<size_t>(winner_seat)] += out.points;
  } else {
    const int total = Ceil100((dealer ? 6 : 4) * base) + 300 * honba;
    out.per_payer = total;
    out.points = total;
    out.delta[static_cast<size_t>(from_seat)] -= total;
    out.delta[static_cast<size_t>(winner_seat)] += total;
  }
  return out;
}

// 番 + 符 → base（含满贯以上分档）
int BaseFor(int han, int fu, Limit* limit) {
  if (han >= 13) {
    *limit = Limit::kYakuman;
    return 8000;
  }
  if (han >= 11) {
    *limit = Limit::kSanbaiman;
    return 6000;
  }
  if (han >= 8) {
    *limit = Limit::kBaiman;
    return 4000;
  }
  if (han >= 6) {
    *limit = Limit::kHaneman;
    return 3000;
  }
  int base = fu * (1 << (2 + han));
  if (base >= 2000) {
    *limit = Limit::kMangan;
    return 2000;
  }
  *limit = Limit::kNone;
  return base;
}

}  // namespace

bool IsClosedHand(const std::vector<Meld>& melds) {
  for (const Meld& meld : melds) {
    if (BreaksMenzen(meld.type)) {
      return false;
    }
  }
  return true;
}

Face DoraFaceFromIndicator(Tile indicator) {
  const Face face = FaceOf(indicator);
  if (IsNumberFace(face)) {
    const uint8_t suit = SuitOf(face);
    const uint8_t rank = RankOf(face);
    const uint8_t next = rank == 9 ? 1 : static_cast<uint8_t>(rank + 1);
    return static_cast<Face>(suit * kRankCount + (next - 1));
  }
  if (IsWindFace(face)) {
    const int index = face - kNumericFaceCount;
    return static_cast<Face>(kNumericFaceCount + (index + 1) % 4);
  }
  const int index = face - kDragonBase;
  return static_cast<Face>(kDragonBase + (index + 1) % 3);
}

int YakuHan(YakuId id, bool closed) {
  switch (id) {
    case YakuId::kRiichi:
    case YakuId::kIppatsu:
    case YakuId::kTsumo:
    case YakuId::kPinfu:
    case YakuId::kIipeikou:
    case YakuId::kTanyao:
    case YakuId::kYakuhai:
    case YakuId::kRinshan:
    case YakuId::kChankan:
    case YakuId::kHaitei:
    case YakuId::kHoutei:
    case YakuId::kDora:
    case YakuId::kUraDora:
    case YakuId::kAkaDora:
      return 1;
    case YakuId::kChiitoitsu:
    case YakuId::kToitoi:
    case YakuId::kSanankou:
    case YakuId::kSankantsu:
    case YakuId::kShousangen:
    case YakuId::kHonroutou:
      return 2;
    case YakuId::kSanshokuDoujun:
    case YakuId::kIttsuu:
    case YakuId::kChanta:
      return closed ? 2 : 1;
    case YakuId::kJunchan:
      return closed ? 3 : 2;
    case YakuId::kRyanpeikou:
      return 3;
    case YakuId::kHonitsu:
      return closed ? 3 : 2;
    case YakuId::kChinitsu:
      return closed ? 6 : 5;
    default:
      return 0;
  }
}

bool IsYakumanYaku(YakuId id) {
  switch (id) {
    case YakuId::kTenhou:
    case YakuId::kChiihou:
    case YakuId::kKokushi:
    case YakuId::kKokushi13:
    case YakuId::kSuuankou:
    case YakuId::kSuuankouTanki:
    case YakuId::kDaisangen:
    case YakuId::kTsuuiisou:
    case YakuId::kRyuuiisou:
    case YakuId::kChinroutou:
    case YakuId::kChuuren:
    case YakuId::kChuurenJunsei:
    case YakuId::kSuukantsu:
    case YakuId::kShousuushii:
    case YakuId::kDaisuushii:
      return true;
    default:
      return false;
  }
}

int YakumanMultiplier(YakuId id) {
  // 双倍役满四兄弟：国士十三面 / 四暗刻单骑 / 纯正九莲 / 大四喜。
  switch (id) {
    case YakuId::kKokushi13:
    case YakuId::kSuuankouTanki:
    case YakuId::kChuurenJunsei:
    case YakuId::kDaisuushii:
      return 2;
    case YakuId::kTenhou:
    case YakuId::kChiihou:
    case YakuId::kKokushi:
    case YakuId::kSuuankou:
    case YakuId::kDaisangen:
    case YakuId::kTsuuiisou:
    case YakuId::kRyuuiisou:
    case YakuId::kChinroutou:
    case YakuId::kChuuren:
    case YakuId::kSuukantsu:
    case YakuId::kShousuushii:
      return 1;
    default:
      return 0;
  }
}

bool CanDeclareRiichi(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                      int remaining) {
  if (!IsClosedHand(melds)) {
    return false;
  }
  if (remaining >= 0 && remaining < kMinRiichiWall) {
    return false;
  }
  if (concealed_with_winning_tile.size() % 3 != 2) {
    return false;  // 必须是行动态（3n+2）
  }
  std::array<bool, kFaceCount> tried {};
  for (const Tile& tile : concealed_with_winning_tile) {
    const Face face = FaceOf(tile);
    if (tried[static_cast<size_t>(face)]) {
      continue;
    }
    tried[static_cast<size_t>(face)] = true;
    std::vector<Tile> rest = concealed_with_winning_tile;
    const auto it =
        std::find_if(rest.begin(), rest.end(), [face](const Tile& candidate) { return FaceOf(candidate) == face; });
    if (it != rest.end()) {
      rest.erase(it);
    }
    if (!WaitingTiles(rest, melds).empty()) {
      return true;
    }
  }
  return false;
}

namespace {

// 幺九字 13 面（1m 9m 1p 9p 1s 9s 东南西北白发中）
constexpr Face kKokushiFaces[13] = {0, 8, 9, 17, 18, 26, 27, 28, 29, 30, 31, 32, 33};

bool IsKokushiFace(Face face) {
  return std::find(std::begin(kKokushiFaces), std::end(kKokushiFaces), face) != std::end(kKokushiFaces);
}

}  // namespace

bool IsKokushiHand(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds) {
  if (!melds.empty() || concealed_with_winning_tile.size() != 14) {
    return false;  // 国士必须是门前清的 14 张
  }
  std::array<int, kFaceCount> counts {};
  for (Tile tile : concealed_with_winning_tile) {
    const Face face = FaceOf(tile);
    if (!IsKokushiFace(face)) {
      return false;  // 混进任何非幺九字就不是国士
    }
    ++counts[static_cast<size_t>(face)];
  }
  int pairs = 0;
  for (Face face : kKokushiFaces) {
    const int count = counts[static_cast<size_t>(face)];
    if (count == 0) {
      return false;  // 少一种
    }
    if (count == 2) {
      ++pairs;
    } else if (count != 1) {
      return false;  // 三张以上不成立
    }
  }
  return pairs == 1;
}

bool IsRiichiWinningHand(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                         Tile winning_tile, bool tsumo) {
  if (IsKokushiHand(concealed_with_winning_tile, melds)) {
    return true;
  }
  return IsWinningHand(concealed_with_winning_tile, melds, winning_tile, tsumo);
}

std::vector<Tile> RiichiWaitingTiles(const std::vector<Tile>& concealed, const std::vector<Meld>& melds) {
  std::vector<Tile> waits = WaitingTiles(concealed, melds);
  if (!melds.empty() || concealed.size() != 13) {
    return waits;
  }
  std::array<int, kFaceCount> counts {};
  for (Tile tile : concealed) {
    const Face face = FaceOf(tile);
    if (!IsKokushiFace(face)) {
      return waits;  // 含非幺九字 ⇒ 不可能在等国士
    }
    ++counts[static_cast<size_t>(face)];
  }
  int present = 0;
  Face missing = static_cast<Face>(0);
  int pairs = 0;
  for (Face face : kKokushiFaces) {
    const int count = counts[static_cast<size_t>(face)];
    if (count == 0) {
      missing = face;
    } else {
      ++present;
      if (count == 2) {
        ++pairs;
      }
    }
  }
  // 十三面：13 种各一张，摸/和任何幺九字都成；单骑：缺一种且已有对子
  if (present == 13) {
    for (Face face : kKokushiFaces) {
      waits.push_back(MakeTileFromFace(face));
    }
  } else if (present == 12 && pairs == 1) {
    waits.push_back(MakeTileFromFace(missing));
  }
  std::sort(waits.begin(), waits.end(), [](Tile lhs, Tile rhs) { return FaceOf(lhs) < FaceOf(rhs); });
  waits.erase(std::unique(waits.begin(), waits.end(), [](Tile lhs, Tile rhs) { return FaceOf(lhs) == FaceOf(rhs); }),
              waits.end());
  return waits;
}

bool RemoveOneFaceIn(std::vector<Tile>* tiles, Face face) {
  const auto target = std::find_if(tiles->begin(), tiles->end(), [face](Tile tile) { return FaceOf(tile) == face; });
  if (target == tiles->end()) {
    return false;
  }
  tiles->erase(target);
  return true;
}

std::vector<Face> RiichiDiscardFaces(const std::vector<Tile>& concealed_with_winning_tile,
                                     const std::vector<Meld>& melds) {
  std::vector<Face> faces;
  if (!IsClosedHand(melds)) {
    return faces;  // 鸣过牌就没有宣言牌可言
  }
  for (const Tile tile : concealed_with_winning_tile) {
    const Face face = FaceOf(tile);
    if (std::find(faces.begin(), faces.end(), face) != faces.end()) {
      continue;  // 同面只看一次：赤 5 与普通 5 同面，打出后听不听牌完全一致
    }
    std::vector<Tile> rest = concealed_with_winning_tile;
    RemoveOneFaceIn(&rest, face);
    if (!RiichiWaitingTiles(rest, melds).empty()) {
      faces.push_back(face);
    }
  }
  std::sort(faces.begin(), faces.end());
  return faces;
}

bool CanAnkanAfterRiichi(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                         Tile drawn, Face face, int remaining) {
  if (remaining <= 0) {
    return false;  // 没有补牌可摸
  }
  const int copies =
      static_cast<int>(std::count_if(concealed_with_winning_tile.begin(), concealed_with_winning_tile.end(),
                                     [face](Tile tile) { return FaceOf(tile) == face; }));
  if (copies < 4) {
    return false;
  }
  // 暗杠前的等待态：14 张去掉刚摸到的那张（立直者一定刚摸过牌）
  std::vector<Tile> before = concealed_with_winning_tile;
  if (!RemoveOneFaceIn(&before, FaceOf(drawn))) {
    return false;
  }
  const std::vector<Tile> waits_before = WaitingTiles(before, melds);
  if (waits_before.empty()) {
    return false;  // 本来就不到听牌（立直者不该出现，防御性拒绝）
  }
  // 暗杠后的等待态：10 张 + 暗杠组（`WaitingTiles` 每组按 3 张计，与杠无关）
  std::vector<Tile> after = concealed_with_winning_tile;
  for (int i = 0; i < 4; ++i) {
    RemoveOneFaceIn(&after, face);
  }
  std::vector<Meld> melds_after = melds;
  Meld ankan;
  ankan.type = MeldType::kAnkan;
  for (int i = 0; i < 4; ++i) {
    ankan.tiles.push_back(MakeTileFromFace(face));
  }
  melds_after.push_back(std::move(ankan));
  return WaitingTiles(after, melds_after) == waits_before;
}

std::vector<Option> DiscardOptions(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                                   int remaining, bool riichi_declared, std::optional<Tile> drawn_tile) {
  std::vector<Option> options;
  const std::vector<Option> base = mahjong::DiscardOptions(concealed_with_winning_tile, melds, remaining);
  for (const Option& option : base) {
    if (riichi_declared && (option.action == protocol::Action::kAnkan || option.action == protocol::Action::kKakan)) {
      continue;  // 立直后 kakan 不可能（没有碰组），ankan 由下面的「不改变听牌」口径重算
    }
    options.push_back(option);
  }
  // 国士无双：共享的 `DiscardOptions` 按番数制的牌型判和，认不出国士，这里补一条 win。
  if (FindOption(options, protocol::Action::kWin) == nullptr && IsKokushiHand(concealed_with_winning_tile, melds)) {
    options.insert(options.begin(), Option {.action = protocol::Action::kWin, .tiles = {}});  // 顺序仍是 win 在最前
  }
  if (riichi_declared && drawn_tile.has_value()) {
    for (int index = 0; index < kFaceCount; ++index) {
      const Face face = static_cast<Face>(index);
      if (!CanAnkanAfterRiichi(concealed_with_winning_tile, melds, *drawn_tile, face, remaining)) {
        continue;
      }
      Option ankan;
      ankan.action = protocol::Action::kAnkan;
      ankan.tiles = {MakeTileFromFace(face)};  // 与 base 同形：恰 1 个牌面元素
      options.push_back(std::move(ankan));
    }
  }
  if (!riichi_declared && CanDeclareRiichi(concealed_with_winning_tile, melds, remaining)) {
    Option riichi_option;
    riichi_option.action = protocol::Action::kRiichi;
    options.push_back(riichi_option);
  }
  return options;
}

std::vector<Option> ClaimOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds, Tile discarded,
                                 int remaining, bool is_next_player, bool furiten) {
  std::vector<Option> options = mahjong::ClaimOptions(hand, melds, discarded, remaining, is_next_player);
  if (FindOption(options, protocol::Action::kWin) == nullptr) {
    // 国士无双：补上共享实现认不出的荣和
    std::vector<Tile> full = hand;
    full.push_back(discarded);
    if (IsKokushiHand(full, melds)) {
      options.insert(options.begin(), Option {.action = protocol::Action::kWin, .tiles = {}});
    }
  }
  if (furiten) {
    options.erase(std::remove_if(options.begin(), options.end(),
                                 [](const Option& option) { return option.action == protocol::Action::kWin; }),
                  options.end());
  }
  return options;
}

// 日麻 AI 弃牌：评分与共享 `ChooseDiscard` 同口径，只在并列处按「役 / 振听」决胜。
std::optional<Tile> ChooseDiscard(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                  const std::vector<Tile>& own_discards, const WinContext& ctx, bool can_riichi,
                                  std::optional<Tile> drawn_tile) {
  if (hand.empty()) {
    return std::nullopt;
  }
  // 每个去重牌面一个候选。评分沿用共享实现的 `BestShantenAfterDraw`；只有**评分并列**的候选
  // 才需要判役 —— 判役比算向听贵得多（逐张 `DetectYaku`），不该为没希望的候选付这笔钱。
  struct Candidate {
    Face face;
    int score;               // 打这张之后的最优向听（越小越好）
    int yaku_waits = 0;      // 听牌张里有役的枚数（荣和口径）
    bool furiten = false;    // 打完是否落舍て牌振听
    std::vector<Tile> rest;  // 打出该面之后的门前手牌（判役时才用）
  };
  std::vector<Candidate> candidates;
  for (Tile tile : hand) {
    const Face face = FaceOf(tile);
    bool seen = false;
    for (const Candidate& candidate : candidates) {
      if (candidate.face == face) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    Candidate candidate;
    candidate.face = face;
    candidate.rest = hand;
    RemoveOneFaceIn(&candidate.rest, face);
    candidate.score = BestShantenAfterDraw(candidate.rest, melds);
    candidates.push_back(std::move(candidate));
  }
  // 第一关键字：向听评分。只留评分最优的那批进决胜。
  int best_score = candidates.front().score;
  for (const Candidate& candidate : candidates) {
    best_score = std::min(best_score, candidate.score);
  }
  std::vector<size_t> finalists;
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (candidates[index].score != best_score) {
      continue;
    }
    finalists.push_back(index);
  }
  // 第二关键字：这一手听牌到底能不能和。只有并列者才判役。
  for (size_t index : finalists) {
    Candidate& candidate = candidates[index];
    const std::vector<Tile> waits = RiichiWaitingTiles(candidate.rest, melds);
    if (waits.empty()) {
      continue;
    }
    const std::vector<Tile> live = WinningWaits(candidate.rest, melds, ctx, waits);
    if (!live.empty()) {
      candidate.yaku_waits = static_cast<int>(live.size());
    } else if (can_riichi) {
      // 无役但门清可立直：立直本身就是役，整手听牌都活了，只是不如「已经有役」稳。
      candidate.yaku_waits = static_cast<int>(waits.size());
    }
    candidate.furiten = IsPermanentFuriten(candidate.rest, melds, own_discards);
  }
  // 决胜顺序固定：有役枚数多者优先 → 未振听优先 → 冻结牌序最小牌面（保证确定性）。
  std::sort(finalists.begin(), finalists.end(), [&candidates](size_t lhs, size_t rhs) {
    if (candidates[lhs].yaku_waits != candidates[rhs].yaku_waits) {
      return candidates[lhs].yaku_waits > candidates[rhs].yaku_waits;
    }
    if (candidates[lhs].furiten != candidates[rhs].furiten) {
      return !candidates[lhs].furiten;
    }
    return candidates[lhs].face < candidates[rhs].face;
  });
  const Face chosen = candidates[finalists.front()].face;
  // 打摸牌语义：选中牌面正是刚摸到的，就把那**一张物理牌**打出去（赤 5 与普通 5 同面）。
  if (drawn_tile.has_value() && FaceOf(*drawn_tile) == chosen) {
    return *drawn_tile;
  }
  for (Tile tile : hand) {
    if (FaceOf(tile) == chosen) {
      return tile;
    }
  }
  return hand.front();  // 理论不可达
}

std::array<int, 4> DrawPenalty(const std::array<bool, 4>& tenpai) {
  std::array<int, 4> delta {};
  int tenpai_count = 0;
  for (bool flag : tenpai) {
    if (flag) {
      ++tenpai_count;
    }
  }
  if (tenpai_count == 0 || tenpai_count == 4) {
    return delta;  // 全听或全不听：无人收付
  }
  const int per_noten = 3000 / (4 - tenpai_count);
  const int per_tenpai = 3000 / tenpai_count;
  for (int seat = 0; seat < 4; ++seat) {
    delta[static_cast<size_t>(seat)] = tenpai[static_cast<size_t>(seat)] ? per_tenpai : -per_noten;
  }
  return delta;
}

bool IsBusted(const std::array<int, 4>& points) {
  for (int value : points) {
    if (value < 0) {
      return true;
    }
  }
  return false;
}

KyoutakuAward AwardLeftoverKyoutaku(const std::array<int, 4>& points, int sticks) {
  if (sticks <= 0) {
    return KyoutakuAward {};
  }
  int best = 0;
  for (int seat = 1; seat < 4; ++seat) {
    // 严格大于才替换 ⇒ 同点保留席位号小的那个（起家优先，与场终排名同口径）
    if (points[static_cast<size_t>(seat)] > points[static_cast<size_t>(best)]) {
      best = seat;
    }
  }
  KyoutakuAward award;
  award.seat = best;
  award.sticks = sticks;
  award.points = sticks * kRiichiStickPoints;
  return award;
}

int RoundWindOf(int round_index) {
  if (round_index < 0) {
    return 0;
  }
  return (round_index / kRoundsPerWind) % 4;
}

int SeatWindOf(int round_wind, int seat, int dealer) {
  const int offset = ((seat - dealer) % 4 + 4) % 4;
  return ((round_wind % 4 + 4) % 4 + offset) % 4;
}

std::vector<Option> ChankanOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds, Tile tile,
                                   const WinContext& ctx, bool furiten) {
  std::vector<Option> options;
  if (!furiten && IsRiichiWinningHand(hand, melds, tile, /*is_tsumo=*/false)) {
    std::vector<Tile> full = hand;
    full.push_back(tile);
    SortTiles(full);
    WinContext chankan = ctx;
    chankan.is_chankan = true;  // 抢杠 1 番：这一条让「无役」的手牌也能抢
    if (chankan.from_seat < 0) {
      chankan.from_seat = (ctx.winner_seat + 1) % 4;  // 荣和口径（本函数不关心放炮者是谁）
    }
    if (!DetectYaku(full, melds, tile, chankan).empty()) {
      Option win;
      win.action = protocol::Action::kWin;
      options.push_back(win);
    }
  }
  Option pass;
  pass.action = protocol::Action::kPass;
  options.push_back(pass);
  return options;
}

std::vector<Tile> WinningWaits(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                               const WinContext& ctx) {
  if (concealed.size() + 3 * melds.size() != 13) {
    return {};  // 非等待态：没有「等待张」可谈
  }
  return WinningWaits(concealed, melds, ctx, RiichiWaitingTiles(concealed, melds));
}

std::vector<Tile> WinningWaits(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                               const WinContext& ctx, const std::vector<Tile>& waits) {
  if (concealed.size() + 3 * melds.size() != 13) {
    return {};  // 非等待态：没有「等待张」可谈
  }
  if (ctx.is_riichi) {
    // 立直本身就是役：全部等待张都能荣和（免掉逐张判番，这是「立直治无役」的实现口径）。
    return waits;
  }
  std::vector<Tile> result;
  // 荣和口径：放炮者是谁不影响判番，取一个与和牌者不同的座位即可（`from_seat >= 0` ⇒ 非自摸）。
  const int any_other = (ctx.winner_seat + 1) % 4;
  for (Tile wait : waits) {
    std::vector<Tile> hand = concealed;
    hand.push_back(wait);
    SortTiles(hand);
    WinContext ron = ctx;
    ron.from_seat = any_other;
    ron.is_haitei = false;   // 海底摸月是自摸役
    ron.is_rinshan = false;  // 岭上开花是自摸役
    if (!DetectYaku(hand, melds, wait, ron).empty()) {
      result.push_back(wait);
    }
  }
  return result;
}

bool IsPermanentFuriten(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                        const std::vector<Tile>& own_discards) {
  if (own_discards.empty()) {
    return false;
  }
  const std::vector<Tile> waits = RiichiWaitingTiles(concealed, melds);
  for (const Tile& discard : own_discards) {
    const Face face = FaceOf(discard);
    for (const Tile& wait : waits) {
      if (FaceOf(wait) == face) {
        return true;
      }
    }
  }
  return false;
}

std::vector<Yaku> DetectYaku(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                             Tile winning_tile, const WinContext& ctx) {
  if (const std::vector<Yaku> yakuman = DetectYakuman(concealed_with_winning_tile, melds, winning_tile, ctx);
      !yakuman.empty()) {
    return yakuman;
  }
  const HandFacts facts = ComputeFacts(concealed_with_winning_tile, melds);
  if (melds.empty() && IsSevenPairs(concealed_with_winning_tile)) {
    return YakuOfSevenPairs(ctx, facts);
  }
  std::vector<Yaku> best;
  int best_han = -1;
  for (const Decomposition& decomp : EnumerateDecompositions(concealed_with_winning_tile, melds)) {
    bool pinfu = false;
    const WaitKind wait = WaitOf(decomp, FaceOf(winning_tile));
    std::vector<Yaku> yaku = YakuForDecomposition(decomp, melds, FaceOf(winning_tile), ctx, facts, wait, &pinfu);
    int han = 0;
    for (const Yaku& item : yaku) {
      han += item.han;
    }
    if (han > best_han) {
      best_han = han;
      best = std::move(yaku);
    }
  }
  return best;
}

int FuOf(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds, Tile winning_tile,
         const WinContext& ctx) {
  const HandFacts facts = ComputeFacts(concealed_with_winning_tile, melds);
  if (melds.empty() && IsSevenPairs(concealed_with_winning_tile)) {
    return 25;
  }
  if (!DetectYakuman(concealed_with_winning_tile, melds, winning_tile, ctx).empty()) {
    return 0;
  }
  int best_fu = 0;
  int best_han = -1;
  for (const Decomposition& decomp : EnumerateDecompositions(concealed_with_winning_tile, melds)) {
    const WaitKind wait = WaitOf(decomp, FaceOf(winning_tile));
    bool pinfu = false;
    const std::vector<Yaku> yaku = YakuForDecomposition(decomp, melds, FaceOf(winning_tile), ctx, facts, wait, &pinfu);
    int han = 0;
    for (const Yaku& item : yaku) {
      han += item.han;
    }
    const int fu = FuForDecomposition(decomp, melds, facts, wait, pinfu, ctx.Tsumo(), ctx);
    if (han > best_han || (han == best_han && fu > best_fu)) {
      best_han = han;
      best_fu = fu;
    }
  }
  return best_fu > 0 ? best_fu : 30;
}

ScoreResult ScoreHand(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
                      Tile winning_tile, const WinContext& ctx) {
  ScoreResult result;
  if (ctx.winner_seat < 0 || ctx.winner_seat >= 4) {
    return result;
  }
  const bool dealer = ctx.winner_seat == ctx.dealer_seat;
  const HandFacts facts = ComputeFacts(concealed_with_winning_tile, melds);

  // ---- 1. 役满：命中即只算役满（可叠加）----
  if (const std::vector<Yaku> yakuman = DetectYakuman(concealed_with_winning_tile, melds, winning_tile, ctx);
      !yakuman.empty()) {
    result.yaku = yakuman;
    result.yakuman_count = 0;
    for (const Yaku& item : yakuman) {
      result.yakuman_count += YakumanMultiplier(item.id);  // 双倍役满记 2，可叠加
    }
    result.han = 13 * result.yakuman_count;
    result.fu = 0;
    result.limit = Limit::kYakuman;
    const PointResult pay = Pay(8000 * result.yakuman_count, dealer, ctx.Tsumo(), ctx.winner_seat, ctx.from_seat,
                                ctx.dealer_seat, ctx.honba);
    result.base = pay.base;
    result.points = pay.points;
    result.per_payer = pay.per_payer;
    result.delta = pay.delta;
    return result;
  }

  // ---- 2. 宝牌（含副露与杠的全部物理牌）----
  std::array<int, kFaceCount> counts {};
  for (const Tile& tile : concealed_with_winning_tile) {
    counts[static_cast<size_t>(FaceOf(tile))] += 1;
  }
  for (const Meld& meld : melds) {
    for (const Tile& tile : meld.tiles) {
      counts[static_cast<size_t>(FaceOf(tile))] += 1;
    }
  }
  int dora = 0;
  for (const Tile& indicator : ctx.dora_indicators) {
    dora += counts[static_cast<size_t>(DoraFaceFromIndicator(indicator))];
  }
  int ura_dora = 0;
  if (ctx.is_riichi) {
    for (const Tile& indicator : ctx.ura_indicators) {
      ura_dora += counts[static_cast<size_t>(DoraFaceFromIndicator(indicator))];
    }
  }
  result.dora = dora;
  result.ura_dora = ura_dora;

  // ---- 2b. 赤宝牌（v1.18）：每张 1 番。赤牌是**物理牌属性**（5 的第 4 张），
  // 与指示牌无关；副露/杠里的赤牌同样计入。役满不计赤宝牌（上面已提前返回）。
  int aka = 0;
  for (const Tile& tile : concealed_with_winning_tile) {
    if (IsRedFive(tile)) {
      ++aka;
    }
  }
  for (const Meld& meld : melds) {
    for (const Tile& tile : meld.tiles) {
      if (IsRedFive(tile)) {
        ++aka;
      }
    }
  }
  result.aka = aka;

  // ---- 3. 七对子 / 标准型：取点数最高的拆法 ----
  const Face winning_face = FaceOf(winning_tile);
  int best_points = -1;
  ScoreResult best;
  auto consider = [&](const std::vector<Yaku>& base_yaku, int fu) {
    if (base_yaku.empty()) {
      // 日麻无役不能和：宝牌不是役。房间已在窗口层过滤（HasYakuForWin），这里是第二道防线。
      return;
    }
    int han = dora + ura_dora + aka;
    for (const Yaku& item : base_yaku) {
      han += item.han;
    }
    if (han > 13) {
      han = 13;
    }
    ScoreResult candidate;
    candidate.yaku = base_yaku;
    if (dora > 0) {
      candidate.yaku.push_back(Yaku {.id = YakuId::kDora, .han = dora});
    }
    if (ura_dora > 0) {
      candidate.yaku.push_back(Yaku {.id = YakuId::kUraDora, .han = ura_dora});
    }
    if (aka > 0) {
      candidate.yaku.push_back(Yaku {.id = YakuId::kAkaDora, .han = aka});
    }
    candidate.han = han;
    candidate.fu = fu;
    candidate.dora = dora;
    candidate.ura_dora = ura_dora;
    candidate.aka = aka;
    Limit limit = Limit::kNone;
    const int base = BaseFor(han, fu, &limit);
    const PointResult pay = Pay(base, dealer, ctx.Tsumo(), ctx.winner_seat, ctx.from_seat, ctx.dealer_seat, ctx.honba);
    candidate.base = base;
    candidate.limit = limit;
    candidate.points = pay.points;
    candidate.per_payer = pay.per_payer;
    candidate.delta = pay.delta;
    if (pay.points > best_points) {
      best_points = pay.points;
      best = std::move(candidate);
    }
  };

  if (melds.empty() && IsSevenPairs(concealed_with_winning_tile)) {
    consider(YakuOfSevenPairs(ctx, facts), 25);
  } else {
    for (const Decomposition& decomp : EnumerateDecompositions(concealed_with_winning_tile, melds)) {
      const WaitKind wait = WaitOf(decomp, winning_face);
      bool pinfu = false;
      std::vector<Yaku> yaku = YakuForDecomposition(decomp, melds, winning_face, ctx, facts, wait, &pinfu);
      const int fu = FuForDecomposition(decomp, melds, facts, wait, pinfu, ctx.Tsumo(), ctx);
      consider(yaku, fu);
    }
  }

  if (best_points < 0) {
    return result;  // 不成和牌形（调用方应先判 IsWinningHand）
  }
  return best;
}

}  // namespace riichi
}  // namespace mahjong
