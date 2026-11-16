// tenhou_rules_check：用天凤真实牌谱的**和牌结算**给本仓库的日麻引擎做回归。
//
// 输入是 `game/tools/tenhou_convert.py` 产出的用例 JSON（每项 = 一手真实和牌 + 上下文 +
// 天凤给出的符/番/点数/满贯分档/役种集合）。本工具把同一手牌喂给 `riichi::ScoreHand`，逐项比对。
//
// 为什么值得存在：`riichi_rules_test.cc` 里的用例是人写出来的，覆盖的是「我们想到的情形」；
// 这里覆盖的是**天凤真实打出来的牌**，包括我们想不到的组合（一发 + 抢杠、海底 + 杠、副露混
// 赤牌…）。而且上下文**全部由事件流推导**（见 tenhou_convert.py 顶部表格），不是从天凤的
// `yaku` 反推 —— 否则就是拿答案喂自己。
//
// 用法：
//   tenhou_rules_check build/tenhou_cases.json
//   tenhou_rules_check build/tenhou_cases.json --max-fail 5   # 只列前 5 条差异
//
// 退出码 0 = 全部一致；1 = 有差异（并打印逐项 diff）；2 = 输入读不了。
//
// 已知口径差异（本工具**按引擎口径**比较，不迁就天凤）：
//   * 天凤的 `yaku` 把宝牌/里宝/赤宝牌（52/53/54）也算一条，本工具把它们拆成独立字段比较；
//   * 天凤的役牌按「自风/场风/三元」分别给 id（10..20），本仓库统一成 `yakuhai`，
//     因此役种集合按 `yakuhai` 归一后比较，番数则由引擎自己算。

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"
#include "mahjong_rules.h"
#include "protocol_enums.h"
#include "riichi_rules.h"

namespace mahjong {
namespace {

struct CaseFailure {
  std::string where;  // "log_id#round"
  std::string field;
  std::string expected;
  std::string actual;
};

std::string ToString(int64_t value) { return std::to_string(value); }

// ---- 牌码 → Tile（赤 5 的副本号在本仓库是 3，天凤用 0，由 Python 侧已换算）----
std::optional<Tile> TileOf(const std::string& code) { return ParseTile(code); }

std::vector<Tile> TilesOf(const json::Value& array) {
  std::vector<Tile> tiles;
  if (!array.IsArray()) {
    return tiles;
  }
  for (const json::Value& entry : array.AsArray()) {
    if (!entry.IsString()) {
      continue;
    }
    if (const std::optional<Tile> tile = TileOf(entry.AsString()); tile.has_value()) {
      tiles.push_back(*tile);
    }
  }
  SortTiles(tiles);
  return tiles;
}

std::optional<MeldType> MeldTypeOf(const std::string& name) {
  if (name == "chow") {
    return MeldType::kChow;
  }
  if (name == "pong") {
    return MeldType::kPong;
  }
  if (name == "kong") {
    return MeldType::kKong;
  }
  if (name == "ankan") {
    return MeldType::kAnkan;
  }
  if (name == "kakan") {
    return MeldType::kKakan;
  }
  return std::nullopt;
}

std::string LimitName(riichi::Limit limit) {
  switch (limit) {
    case riichi::Limit::kNone:
      return "none";
    case riichi::Limit::kMangan:
      return "mangan";
    case riichi::Limit::kHaneman:
      return "haneman";
    case riichi::Limit::kBaiman:
      return "baiman";
    case riichi::Limit::kSanbaiman:
      return "sanbaiman";
    case riichi::Limit::kYakuman:
      return "yakuman";
  }
  return "none";
}

const json::Value* Find(const json::Value& object, const std::string& key) { return object.Find(key); }

bool BoolOf(const json::Value& object, const std::string& key) {
  const json::Value* value = Find(object, key);
  return value != nullptr && value->IsBool() && value->AsBool();
}

int64_t IntOf(const json::Value& object, const std::string& key) {
  const json::Value* value = Find(object, key);
  return value != nullptr && value->IsNumber() ? value->AsInt() : 0;
}

std::string StrOf(const json::Value& object, const std::string& key) {
  const json::Value* value = Find(object, key);
  return value != nullptr && value->IsString() ? value->AsString() : std::string();
}

// 天凤的 `yaku` 名字（Python 侧已按本仓库的 `kYakuNames` 归一）→ 排序后的唯一集合。
std::vector<std::string> YakuSetOf(const json::Value& array) {
  std::map<std::string, int> seen;
  if (array.IsArray()) {
    for (const json::Value& entry : array.AsArray()) {
      if (entry.IsString()) {
        seen[entry.AsString()] += 1;
      }
    }
  }
  std::vector<std::string> names;
  names.reserve(seen.size());
  for (const auto& [name, count] : seen) {
    names.push_back(name);
  }
  return names;
}

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (size_t index = 0; index < items.size(); ++index) {
    if (index != 0) {
      out += ",";
    }
    out += items[index];
  }
  return out;
}

// 引擎返回的 yaku[]（已含 dora/ura/aka 三条）→ 只保留「役」的名字集合。
std::vector<std::string> EngineYakuSetOf(const riichi::ScoreResult& result) {
  std::map<std::string, int> seen;
  for (const riichi::Yaku& yaku : result.yaku) {
    switch (yaku.id) {
      case protocol::YakuId::kDora:
      case protocol::YakuId::kUraDora:
      case protocol::YakuId::kAkaDora:
        continue;
      default:
        break;
    }
    seen[std::string(protocol::NameOf(yaku.id))] += yaku.han;
  }
  std::vector<std::string> names;
  names.reserve(seen.size());
  for (const auto& [name, han] : seen) {
    names.push_back(name);
  }
  return names;
}

void CheckCase(const json::Value& item, std::vector<CaseFailure>* failures) {
  const json::Value* context = Find(item, "context");
  const json::Value* expected = Find(item, "expected");
  if (context == nullptr || expected == nullptr) {
    failures->push_back({"(no source)", "structure", "context+expected", "missing"});
    return;
  }
  std::string where = "(unknown)";
  if (const json::Value* source = Find(item, "source"); source != nullptr) {
    where = StrOf(*source, "log_id") + "#" + ToString(IntOf(*source, "round"));
  }

  // ---- 手牌与副露 ----
  const std::vector<Tile> concealed = TilesOf(*Find(item, "concealed"));
  std::vector<Meld> melds;
  if (const json::Value* meld_array = Find(item, "melds");
      meld_array != nullptr && meld_array->IsArray()) {
    for (const json::Value& entry : meld_array->AsArray()) {
      const std::optional<MeldType> type = MeldTypeOf(StrOf(entry, "type"));
      if (!type.has_value()) {
        failures->push_back({where, "meld.type", StrOf(entry, "type"), "unparsable"});
        return;
      }
      Meld meld;
      meld.type = *type;
      meld.tiles = TilesOf(*Find(entry, "tiles"));
      melds.push_back(std::move(meld));
    }
  }

  std::optional<Tile> winning;
  if (const json::Value* tile = Find(item, "winning_tile"); tile != nullptr && tile->IsString()) {
    winning = TileOf(tile->AsString());
  }
  if (!winning.has_value()) {
    failures->push_back({where, "winning_tile", "one tile", "unparsable"});
    return;
  }

  // ---- 上下文 → WinContext ----
  riichi::WinContext ctx;
  ctx.winner_seat = static_cast<int>(IntOf(*context, "seat"));
  // 天凤用 `fromWho == who` 表示自摸，引擎用 `from_seat < 0` —— 这里换算。
  ctx.from_seat = BoolOf(*context, "tsumo") ? -1 : static_cast<int>(IntOf(*context, "from_seat"));
  ctx.dealer_seat = static_cast<int>(IntOf(*context, "dealer_seat"));
  ctx.round_wind = static_cast<int>(IntOf(*context, "round_wind"));
  // 自风走**生产路径**的 `riichi::SeatWindOf`（房间也是这么算的），不在这里绕过 ——
  // 天凤口径是「只由相对庄家的座位决定」，若引擎公式与它不同，这里就该失败。
  ctx.seat_wind = riichi::SeatWindOf(ctx.round_wind, ctx.winner_seat, ctx.dealer_seat);
  ctx.honba = static_cast<int>(IntOf(*context, "honba"));
  ctx.is_riichi = BoolOf(*context, "riichi");
  ctx.is_ippatsu = BoolOf(*context, "ippatsu");
  ctx.is_rinshan = BoolOf(*context, "rinshan");
  ctx.is_chankan = BoolOf(*context, "chankan");
  ctx.is_haitei = BoolOf(*context, "haitei");
  ctx.is_houtei = BoolOf(*context, "houtei");
  ctx.is_tenhou = BoolOf(*context, "tenhou");
  ctx.is_chiihou = BoolOf(*context, "chiihou");
  ctx.dora_indicators = TilesOf(*Find(item, "dora_indicators"));
  ctx.ura_indicators = TilesOf(*Find(item, "ura_indicators"));

  // ---- 和牌形（先确认引擎认这手牌，否则后面比番数没有意义）----
  // 天凤的 `AGARI.hai` **恒含和牌张**（荣和亦然），所以形态检查走「concealed 已含和牌张」的
  // 语义；这与 `ctx.from_seat`（决定支付走荣和还是自摸）是两件事。
  std::vector<Tile> hand = concealed;
  SortTiles(hand);
  if (!riichi::IsRiichiWinningHand(hand, melds, *winning, /*tsumo=*/true)) {
    failures->push_back({where, "win_shape", "valid", "engine says not a winning hand"});
    return;
  }

  const riichi::ScoreResult result = riichi::ScoreHand(hand, melds, *winning, ctx);

  // ---- 逐项比对 ----
  const int64_t expected_fu = IntOf(*expected, "fu");
  // 役满的符恒为 0（天凤也报 0），引擎同样返回 0 —— 直接比
  if (static_cast<int64_t>(result.fu) != expected_fu) {
    failures->push_back({where, "fu", ToString(expected_fu), ToString(result.fu)});
  }

  // 天凤的 `han` 字段（Python 侧已剔除宝牌/里宝/赤宝牌）只含**役**的番数；引擎的 `result.han`
  // 是含宝牌的总番数。两边都换算到「只含役」再比，宝牌另用 dora/ura/aka 三个字段单独比。
  const int64_t expected_han = IntOf(*expected, "han");
  const int64_t actual_base_han =
      static_cast<int64_t>(result.han) - result.dora - result.ura_dora - result.aka;
  if (actual_base_han != expected_han) {
    failures->push_back({where, "han(役)", ToString(expected_han), ToString(actual_base_han)});
  }

  // 引擎的 `points` 含本场（`Pay()` 里 +300×honba 已计入），天凤 `ten[1]` 不含 —— 换算后比。
  const int64_t honba_bonus = 300 * IntOf(*context, "honba");
  const int64_t expected_points = IntOf(*expected, "points");
  const int64_t actual_points = static_cast<int64_t>(result.points) - honba_bonus;
  if (actual_points != expected_points) {
    failures->push_back({where, "points", ToString(expected_points), ToString(actual_points)});
  }

  const std::string expected_limit = StrOf(*expected, "limit");
  const std::string actual_limit = LimitName(result.limit);
  if (expected_limit != actual_limit) {
    failures->push_back({where, "limit", expected_limit, actual_limit});
  }

  const int64_t expected_dora = IntOf(*expected, "dora");
  if (static_cast<int64_t>(result.dora) != expected_dora) {
    failures->push_back({where, "dora", ToString(expected_dora), ToString(result.dora)});
  }
  const int64_t expected_ura = IntOf(*expected, "ura_dora");
  if (static_cast<int64_t>(result.ura_dora) != expected_ura) {
    failures->push_back({where, "ura_dora", ToString(expected_ura), ToString(result.ura_dora)});
  }
  const int64_t expected_aka = IntOf(*expected, "aka_dora");
  if (static_cast<int64_t>(result.aka) != expected_aka) {
    failures->push_back({where, "aka_dora", ToString(expected_aka), ToString(result.aka)});
  }

  const json::Value* yaku_array = Find(*expected, "yaku");
  const std::vector<std::string> expected_yaku =
      YakuSetOf(yaku_array != nullptr ? *yaku_array : json::Value());
  const std::vector<std::string> actual_yaku = EngineYakuSetOf(result);
  if (expected_yaku != actual_yaku) {
    failures->push_back({where, "yaku", Join(expected_yaku), Join(actual_yaku)});
  }
}

}  // namespace
}  // namespace mahjong

int main(int argc, char** argv) {
  using namespace mahjong;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <cases.json> [--max-fail N]\n", argv[0]);
    return 2;
  }
  int max_fail = 20;
  for (int index = 2; index + 1 < argc; ++index) {
    if (std::string(argv[index]) == "--max-fail") {
      max_fail = std::atoi(argv[index + 1]);
    }
  }

  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  std::string error;
  const std::optional<json::Value> document = json::Value::Parse(buffer.str(), &error);
  if (!document.has_value()) {
    std::fprintf(stderr, "cannot parse %s: %s\n", argv[1], error.c_str());
    return 2;
  }
  const json::Value* cases = document->Find("cases");
  if (cases == nullptr || !cases->IsArray()) {
    std::fprintf(stderr, "%s has no \"cases\" array\n", argv[1]);
    return 2;
  }

  std::vector<CaseFailure> failures;
  int total = 0;
  for (const json::Value& item : cases->AsArray()) {
    ++total;
    CheckCase(item, &failures);
  }

  if (failures.empty()) {
    std::printf("[tenhou] %d/%d 手全部一致（符/番/点数/满贯分档/役种/宝牌）\n", total, total);
    return 0;
  }

  std::printf("[tenhou] %d 手中有 %zu 处差异：\n", total, failures.size());
  int shown = 0;
  for (const CaseFailure& failure : failures) {
    if (shown >= max_fail) {
      std::printf("  ... 其余 %zu 处省略（--max-fail 调整）\n", failures.size() - static_cast<size_t>(shown));
      break;
    }
    std::printf("  %s %s: 天凤=%s 引擎=%s\n", failure.where.c_str(), failure.field.c_str(),
                failure.expected.c_str(), failure.actual.c_str());
    ++shown;
  }
  return 1;
}
