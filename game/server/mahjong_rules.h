// 麻将规则库（纯函数，不依赖 actor 运行时）
//
// 规格来源：game/docs/RULESET.md v1.2（136 张口径：34 面 × 4 张，牌山 83）
// 协议口径：game/docs/PROTOCOL.md §2（牌编码与排序）
//
// 本库只做规则计算：牌张与牌山、发牌、和牌判定、副露合法性、动作可选项、算分。
// 不读时钟、不用随机设备、不持有全局可变状态 —— 同一输入必得同一输出（RULESET §8）。
#ifndef GAME_SERVER_MAHJONG_RULES_H_
#define GAME_SERVER_MAHJONG_RULES_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mahjong {

// ---------------------------------------------------------------------------
// 牌张：34 个牌面（数牌 3 花色 × 9 级 = 27 + 字牌 7 种），每面 4 张，共 136 张
//        —— 规格：game/docs/RULESET.md v1.2 §1.1/§1.2（34 面 / 136 张 / 牌山 83）
// ---------------------------------------------------------------------------
inline constexpr int kSuitCount = 3;                              // 数牌门数：m, p, s
inline constexpr int kRankCount = 9;                              // 数牌级 1..9
inline constexpr int kHonorCount = 7;                             // 字牌 1z..7z = 东/南/西/北/白/发/中
inline constexpr int kNumericFaceCount = kSuitCount * kRankCount;  // 27 种数牌（面 0..26）
inline constexpr int kFaceCount = kNumericFaceCount + kHonorCount; // 34 种牌面（面 27..33 为字牌）
inline constexpr int kCopiesPerFace = 4;
inline constexpr int kTileCount = kFaceCount * kCopiesPerFace;     // 136
inline constexpr int kInitialHandSize = 13;
inline constexpr int kWallSizeAfterDeal = kTileCount - (4 * kInitialHandSize) - 1;  // 83
inline constexpr uint8_t kHonorSuit = 3;                           // 字牌的花色编号（SuitOf(面) == 3）

static_assert(kTileCount == kFaceCount * kCopiesPerFace, "总张数必须 = 牌面数 × 每面副本数");
static_assert(kFaceCount == 34, "本里程碑冻结 34 种牌面（27 数牌 + 7 字牌）");
static_assert(kTileCount == 136, "本里程碑冻结 136 张牌");
static_assert(kWallSizeAfterDeal == 83, "发牌 53 张后牌山应剩 83 张");

// 牌面：数牌 `suit * 9 + (rank - 1)` ∈ [0,26]；字牌 27..33 依次为 1z..7z
using Face = uint8_t;

// 一张物理牌：id = face * 4 + copy，取值 0..135（RULESET §1.2）
struct Tile {
  uint8_t id = 0;

  friend bool operator==(Tile a, Tile b) { return a.id == b.id; }
  friend bool operator!=(Tile a, Tile b) { return a.id != b.id; }
  friend bool operator<(Tile a, Tile b) { return a.id < b.id; }
};

// 牌面/花色/级/副本
Face FaceOf(Tile tile);
uint8_t SuitOf(Face face);   // 0=m, 1=p, 2=s, 3=z（字牌）
uint8_t RankOf(Face face);   // 数牌 1..9；字牌 1..7（对应 1z..7z）
uint8_t CopyOf(Tile tile);   // 0..3
Tile MakeTileFromFace(Face face, uint8_t copy = 0);
Tile MakeTile(uint8_t suit, uint8_t rank, uint8_t copy = 0);

// 数牌 / 字牌判定（字牌不可成顺，是唯一需要按类别区分的地方）
bool IsNumberFace(Face face);  // 面 ∈ [0,26]
bool IsHonorFace(Face face);   // 面 ∈ [27,33]
// 字牌名（与 PROTOCOL/RULESET 的映射一致）：27→"东"、28→"南"、29→"西"、30→"北"、31→"白"、32→"发"、33→"中"；
// 非字牌返回空。
std::string_view HonorName(Face face);

// 编码：与 PROTOCOL.md §2.1 的合取正则 `^([1-9][mps]|[1-7]z)$` 一致。
// 合法：数牌 `1m`..`9s`、字牌 `1z`..`7z`；非法（返回 nullopt）：`0m`、`8z`、`10p`、`5x`、空串等。
std::optional<Tile> ParseTile(std::string_view text);
std::string FormatTile(Tile tile);
std::optional<std::vector<Tile>> ParseTiles(const std::vector<std::string>& texts);

// 排序键：**先数牌后字牌**（`m<p<s<z`），数牌内 rank 升序、字牌内 `1z`..`7z` 升序，同面副本升序
// （PROTOCOL.md §2.2 / RULESET §1.2）。AI 打牌、牌河与手牌展示都依赖该口径。
void SortTiles(std::vector<Tile>& tiles);
std::vector<Tile> SortedTiles(std::vector<Tile> tiles);

// 牌面计数（长度 34）
using FaceCounts = std::array<uint8_t, kFaceCount>;
FaceCounts CountFaces(const std::vector<Tile>& tiles);
// 校验：任一牌面不超过 4 张
bool IsVerifiedHand(const std::vector<Tile>& tiles);

// ---------------------------------------------------------------------------
// 牌山与发牌（RULESET §1.3/§1.4）
// ---------------------------------------------------------------------------
// 108/136 之争已裁决为 136（captain，恢复 7 种字牌）：mt19937(seed) + 降序 Fisher–Yates，
// 逐字按 RULESET §1.3。
std::vector<Tile> BuildWall(uint64_t seed);

struct Deal {
  std::array<std::vector<Tile>, 4> hands;  // hands[0] 为庄家（14 张），其余 13 张
  std::vector<Tile> wall;                  // 完整牌山（136 张，摸牌顺序）
  int next_index = 0;                      // 下一次摸牌的墙下标（发牌后为 53）
  int remaining = 0;                       // 牌山剩余（发牌后为 83）
};

Deal DealTiles(const std::vector<Tile>& wall);

// ---------------------------------------------------------------------------
// 副露（RULESET §4）：本里程碑仅碰 / 吃 / 明杠
// ---------------------------------------------------------------------------
enum class MeldType : uint8_t { kPong = 0, kChow = 1, kKong = 2 };

struct Meld {
  MeldType type = MeldType::kPong;
  std::vector<Tile> tiles;  // 全部物理牌面，已排序；pong/chow 3 张，kong 4 张
};

std::string MeldTypeName(MeldType type);
int MeldCountOf(const std::vector<Meld>& melds);
int KanCountOf(const std::vector<Meld>& melds);
bool IsValidMeld(const Meld& meld);

bool CanPong(const std::vector<Tile>& hand, Tile discarded);
bool CanKong(const std::vector<Tile>& hand, Tile discarded, int remaining);
bool CanChow(const std::vector<Tile>& hand, Tile discarded);
// 顺子组合枚举：{a,b} ∈ {{r-2,r-1},{r-1,r+1},{r+1,r+2}}（RULESET §4.2）
std::vector<std::array<Tile, 2>> ChowCombinations(const std::vector<Tile>& hand, Tile discarded);

// ---------------------------------------------------------------------------
// 和牌判定（RULESET §3）：标准型（4 面子 + 1 将）、七对子、清一色标签
// ---------------------------------------------------------------------------
// concealed 已包含和牌张（逻辑 14 张 = concealed + 3×melds）
bool CanWinExact(const std::vector<Tile>& concealed_with_winning_tile,
                 const std::vector<Meld>& melds);
// 荣和形态：concealed 为门前手牌（不含和牌张）
bool CanWinWith(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                Tile winning_tile);
// 统一入口（RULESET §9）：is_tsumo 为真时 concealed 已含和牌张
bool IsWinningHand(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                   Tile winning_tile, bool is_tsumo);

bool IsSevenPairs(const std::vector<Tile>& concealed_with_winning_tile);
bool IsPureOneSuit(const std::vector<Tile>& concealed_with_winning_tile,
                   const std::vector<Meld>& melds);

// 标签顺序固定：seven_pairs 在前，pure_one_suit 在后（RULESET §3.5）
std::vector<std::string> WinTags(const std::vector<Tile>& concealed_with_winning_tile,
                                 const std::vector<Meld>& melds);

// ---------------------------------------------------------------------------
// 动作可选项与超时默认动作（RULESET §5）
// ---------------------------------------------------------------------------
struct Option {
  std::string action;       // "win" | "kong" | "pong" | "chow" | "pass"
  std::vector<Tile> tiles;  // win/pass 为空；kong/pong=[T]；chow=[a,b]
};

// 顺序固定 win → kong → pong → chow → pass（PROTOCOL.md §5.7）
std::vector<Option> ClaimOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                 Tile discarded, int remaining, bool is_next_player);
// 打牌窗口：[] 或 [{"win",{}}]（PROTOCOL.md §5.7）
std::vector<Option> DiscardOptions(const std::vector<Tile>& concealed_with_winning_tile,
                                   const std::vector<Meld>& melds);
bool HasNonPassOption(const std::vector<Option>& options);

// 超时/托管默认打牌（RULESET §5.4）：有摸牌记录打刚摸到的牌，否则打排序后最后一张。
// **本函数是 GAME_DESIGN §5.6.2 冻结的确定性回退（超时代打 / 人类托管），语义不得改动**；
// AI 的牌效率选牌请用 ChooseDiscard。
std::optional<Tile> DefaultDiscard(const std::vector<Tile>& hand, std::optional<Tile> drawn_tile);

// ---------------------------------------------------------------------------
// AI 弃牌策略：向听数 + 牌效率（t20）
// ---------------------------------------------------------------------------
// 向听数（口径冻结）：
//   -1 = **和牌形**（逻辑 14 张：`concealed + 3×melds == 14`，即 4 面子 + 1 将）；
//    0 = 听牌（13 张口径：`concealed + 3×melds == 13`）；
//    n = n 向听。
// 标准型：`8 − 2×面子数 − (搭子数 + 对子数)`（面子/搭子合计 ≤ 4、另有 1 组将时再 −1）；
// 七对子：`6 − 对数`（仅门前清），两者取较小者。
// `melds` 的每组副露计入面子数；`concealed` 为门前手牌（含刚摸到的牌时按 14 张口径）。
// 字牌只能作刻子/对子，**永不参与顺子**。
int Shanten(const std::vector<Tile>& concealed, const std::vector<Meld>& melds);

// 打出一张后、再从 34 个牌面中摸一张所能达到的**最优向听**（越小越好）。
// `hand_after_discard` 为打牌后的门前手牌（13 张口径）。
int BestShantenAfterDraw(const std::vector<Tile>& hand_after_discard,
                         const std::vector<Meld>& melds);

// 牌效率弃牌：对每个**去重后的候选打牌**，以其 `BestShantenAfterDraw` 作为评分，取评分最小者。
// 完全并列时按**冻结牌序**（先数牌后字牌、字牌 1z..7z，即牌面编号升序）取最小牌面 ⇒ 结果确定。
//
// 契约（供 room.cc 接线，覆盖全部三种选牌情形）：
//   - `hand` 为门前手牌且**已包含刚摸到的牌**（行动态，14 张口径：`hand.size() + 3×melds == 14`）；
//     庄家第一打（起手 14 张）同样适用。
//   - `drawn_tile` **可为 `std::nullopt`**，用于「无摸牌记录」的情形（庄家第一打、碰/吃后的待打牌）：
//     `nullopt` 时按**纯牌效率**选牌，评分与牌面选择完全一致（`drawn_tile` 只影响同面副本的归属，
//     不影响评分，也不需要调用方自行写兜底）。
//   - 返回值为打出的那张牌；**仅当 `hand` 为空时返回 `std::nullopt`**。
//   - 本函数与 `DefaultDiscard` 职责分离：后者是超时代打/人类托管的冻结确定性回退，语义不变。
std::optional<Tile> ChooseDiscard(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                  std::optional<Tile> drawn_tile);
// 无摸牌记录的便捷重载（等价于传 std::nullopt）
std::optional<Tile> ChooseDiscard(const std::vector<Tile>& hand, const std::vector<Meld>& melds);

// 鸣牌门槛辅助（供 AI 鸣牌决策使用，避免 t21 再改本文件）：
// 计算「用手牌 `consume` 鸣 `claimed` 组成 `type` 副露，随后再打出最优一张」所能达到的
// **最优向听**（13 张口径，越小越好）。
//   - `hand`：鸣牌前门前手牌（等待态，`hand.size() + 3×melds == 13`）
//   - `claimed`：被鸣的那张牌；`consume`：本次鸣牌从手牌中消耗的牌
//       （`pong`/`kong` 传 3 张同面中的 2/3 张；`chow` 传组合 `{a,b}`）
//   - `type`：`kPong` / `kChow` / `kKong`；明杠会**补摸一张**，本函数按「补牌的最坏已知信息」处理：
//       取 34 面中**最优**的补牌（即明杠后的最优向听），与 `BestShantenAfterDraw` 同样的乐观口径
//   - 手牌不足以完成该鸣牌、或牌型不合法 ⇒ 返回 `std::nullopt`
std::optional<int> ShantenAfterMeld(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                    Tile claimed, const std::vector<Tile>& consume, MeldType type);

// 打出一张后的最优向听（行动态 14 张口径 ⇒ 13 张口径），供鸣牌门槛与调试使用
int BestDiscardShanten(const std::vector<Tile>& hand, const std::vector<Meld>& melds);

// ---------------------------------------------------------------------------
// 算分（RULESET §6）
// ---------------------------------------------------------------------------
struct ScoreResult {
  std::array<int, 4> delta{};  // 四家得失分，和恒为 0
  int multiplier = 0;          // M ∈ {1,4,16}
  int per_payer = 0;           // 荣和 = M；自摸 = 2M
  int total = 0;               // 荣和 = M；自摸 = 6M
};

int MultiplierForTags(const std::vector<std::string>& tags);
// from_seat < 0 表示自摸；否则为放炮者座位
ScoreResult ScoreWin(int winner_seat, int from_seat,
                     const std::vector<Tile>& concealed_with_winning_tile,
                     const std::vector<Meld>& melds);
ScoreResult ScoreDraw();

// 供 t4 使用：从 JSON 语义的角度判断选项是否包含指定动作
const Option* FindOption(const std::vector<Option>& options, std::string_view action);
bool OptionTilesMatch(const Option& option, const std::vector<Tile>& expected);

}  // namespace mahjong

#endif  // GAME_SERVER_MAHJONG_RULES_H_
