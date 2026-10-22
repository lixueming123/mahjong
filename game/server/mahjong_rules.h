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
  std::array<std::vector<Tile>, 4> hands;  // hands[dealer] 为庄家（14 张），其余 13 张
  std::vector<Tile> wall;                  // 完整牌山（136 张，摸牌顺序）
  int next_index = 0;                      // 下一次摸牌的墙下标（发牌后为 53）
  int remaining = 0;                       // 牌山剩余（发牌后为 83）
};

// `dealer` ∈ [0,3]：庄家坐位拿 14 张、其余三家 13 张（B1 起庄家可轮转；越界按 0 处理）。
// 发牌顺序与张数不变：四家轮流 13 张（共 52 张）→ 第 53 张给庄家 ⇒ 牌山恒剩 83 张。
Deal DealTiles(const std::vector<Tile>& wall, int dealer);

// 连庄判定（§3.3 S4）：庄家和牌或荒庄 ⇒ 连庄（庄家不变）；闲家和牌 ⇒ 轮庄。
// **判定所需的「谁和牌」由调用方先归纳成 RoundResult**，规则库不接触房间状态。
enum class RoundResult : uint8_t {
  kDealerWin = 0,      // 庄家和牌
  kNonDealerWin = 1,   // 闲家和牌
  kDrawExhausted = 2,  // 荒庄（无和牌者）
};
int NextDealer(int dealer, RoundResult result);
// 把房间的结算原因（"win_ron" / "win_tsumo" / "draw_exhausted"）归纳成 RoundResult：
// 荒庄一律 kDrawExhausted；其余按「和牌者是否为庄家」区分。未知 reason 视为闲家和牌（保守轮庄）。
RoundResult ClassifyRound(std::string_view reason, int winner_seat, int dealer);

// ---------------------------------------------------------------------------
// 副露（RULESET §4 / GAME_DESIGN_V2 §4）：碰 / 吃 / 明杠 / 暗杠 / 加杠
// ---------------------------------------------------------------------------
// ⚠️ **张数公式逐字不变**（GAME_DESIGN_V2 §4.1）：
//     逻辑张数 = concealed + 3×melds ∈ {13, 14}
//     物理张数 = concealed + 3×melds + kans = (13 或 14) + kans
// 一个杠（明杠 kong / 暗杠 ankan / 加杠 kakan）在 `melds` 里**都只记 1 组、账面 3 张**，
// 第 4 张由 `kans` 单独补回。三者的差别**只在「第 4 张从哪来、什么时候来」**，
// 对台账零影响 —— 因此绝不可给暗杠/加杠单开一套张数公式。
enum class MeldType : uint8_t {
  kPong = 0,   // 碰（3 张，明）
  kChow = 1,   // 吃（3 张，明）
  kKong = 2,   // 明杠（4 张，第 4 张来自他人打出的牌）
  kAnkan = 3,  // 暗杠（4 张，第 4 张起于手中已有的 4 张同面牌）—— **不破门清**
  kKakan = 4,  // 加杠（4 张，第 4 张起于手中、就地升级已有 pong）—— **必然破门清**
};

struct Meld {
  MeldType type = MeldType::kPong;
  std::vector<Tile> tiles;  // 全部物理牌面，已排序；pong/chow 3 张，kong/ankan/kakan 4 张
};

std::string MeldTypeName(MeldType type);
// 是否为杠（三种杠任一）：kans = 杠数 = kong + ankan + kakan 的**组数**（旧「kans = 明杠数」作废）
bool IsKanType(MeldType type);
// 是否破门清：pong / chow / kong / kakan 破；ankan 不破（§1.2 #1）
bool BreaksMenzen(MeldType type);
int MeldCountOf(const std::vector<Meld>& melds);  // 组数（每个杠 1 组）
int KanCountOf(const std::vector<Meld>& melds);
bool IsValidMeld(const Meld& meld);

bool CanPong(const std::vector<Tile>& hand, Tile discarded);
bool CanKong(const std::vector<Tile>& hand, Tile discarded, int remaining);
bool CanChow(const std::vector<Tile>& hand, Tile discarded);
// 顺子组合枚举：{a,b} ∈ {{r-2,r-1},{r-1,r+1},{r+1,r+2}}（RULESET §4.2）
std::vector<std::array<Tile, 2>> ChowCombinations(const std::vector<Tile>& hand, Tile discarded);

// ---- 暗杠 / 加杠的可用性（纯查询，不含 `remaining >= 1` 的牌山守卫；守卫由调用方叠加）----
// 手中恰有某面 4 张 ⇒ 该面可暗杠。返回牌面升序、去重。
std::vector<Face> AnkanFaces(const std::vector<Tile>& hand);
bool CanAnkan(const std::vector<Tile>& hand);
// 已有 pong 且手中有同面第 4 张 ⇒ 该 pong 组可加杠。返回可加杠的 **meld 下标**（升序）。
std::vector<int> KakanMeldIndices(const std::vector<Tile>& hand, const std::vector<Meld>& melds);
bool CanKakan(const std::vector<Tile>& hand, const std::vector<Meld>& melds);

// ---- 杠的落地（纯函数，便于单测张数推导；房间只负责补牌与开窗口）----
// 加杠是**就地升级**：该 pong 组的 type 改 kakan、tiles 追加第 4 张到末尾、**组序不变**、
// MeldCountOf 不变、KanCountOf +1。
struct KongResult {
  std::vector<Tile> hand;   // 扣牌后的门前手牌（未含补牌）
  std::vector<Meld> melds;  // 更新后的副露（组序稳定）
};
// 暗杠：手牌移除该面 4 张，追加一组 ankan（已排序，追加在末尾）。
// 条件不成立（该面不是恰 4 张）⇒ nullopt。
std::optional<KongResult> ApplyAnkan(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                     Face face);
// 加杠：手牌移除第 4 张（1 张），就地升级 melds[meld_index]。
// 条件不成立（下标越界 / 非 pong / 手中无同面牌）⇒ nullopt。
std::optional<KongResult> ApplyKakan(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                     int meld_index);

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

// ---------------------------------------------------------------------------
// 全拆法枚举（GAME_DESIGN_V2 §1.1 口径 2）：判番的唯一基础
// ---------------------------------------------------------------------------
// 一组面子。副露组（melds 引用）与门前组统一表达，**不区分** kong/ankan/kakan
// ——它们都是「刻子类」的一组（账面 3 张，第 4 张由 kans 补）。
struct Group {
  bool from_meld = false;  // true ⇒ 引用 melds[meld_index]；false ⇒ 门前拆出的一组
  int meld_index = -1;
  bool is_run = false;  // true = 顺子；false = 刻子/杠
  Face face = 0;        // 顺子：最小牌面；刻子/杠：该牌面
};

// 和牌张落在将牌上时 WinGroupIndex 的返回值
inline constexpr int kWinInPair = -2;

struct Decomposition {
  std::array<Group, 4> groups{};  // 固定 4 组（副露在前，按 melds 原序；门前组按枚举序）
  Face pair = 0;                  // 将牌牌面
};

// 枚举「4 组面子 + 1 将」的**全部**拆法。`melds` 每组各占一组面子，
// 故 concealed 的张数必须为 `(4 − MeldCountOf(melds))×3 + 2`，否则返回空。
//
// **产出顺序冻结**（GAME_DESIGN_V2 §1.3.4，与既有 `Decompose` 的回溯序逐一一致）：
//   ① 将牌牌面按面编号 0..33 升序逐个尝试；
//   ② 每个将牌定下后，面子按「本拆法最小未用牌面」依次展开；
//   ③ 同一最小牌面下，**刻子分支先于顺子分支**。
// 「同番数拆法取第一个」因此是确定的。
std::vector<Decomposition> EnumerateDecompositions(
    const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds);

// 和牌张落在哪一组：返回 groups 下标；落在将牌返回 `kWinInPair`；该拆法不含此牌面返回 -1。
// 选择口径（确定、且对玩家最有利）：**顺子组优先于刻子组**，其次刻子组，最后将牌。
// 唯一消费者是三暗刻（荣和时由和牌张补成的刻子不计暗刻），此口径即「能和成顺子就按顺子算」。
int WinGroupIndex(const Decomposition& decomposition, Face winning_face);

// ---------------------------------------------------------------------------
// 判番（GAME_DESIGN_V2 §1.2 的 14 个番种；序 = 判定顺序 = 数组顺序 = 渲染顺序）
// ---------------------------------------------------------------------------
// `id` 一律 ASCII 小写下划线；**同一 id 在一份结果里至多一条**（yakuhai 合并为一条，han = 组数）。
struct Yaku {
  std::string id;
  int han = 0;

  friend bool operator==(const Yaku& a, const Yaku& b) {
    return a.id == b.id && a.han == b.han;
  }
  friend bool operator!=(const Yaku& a, const Yaku& b) { return !(a == b); }
};

// 番种表（仅供测试与展示，不参与判定）：未知 id 返回 0。yakuhai 返回**单组**的 1 番。
int YakuHan(std::string_view id);
int YakuHanTotal(const std::vector<Yaku>& yaku);

// 判番上下文。**自摸不是番种**（B1.1 裁决）：不计入番种合计，"自摸 ×2" 已并入支付公式的
// 2R/6R；`from_seat < 0` 仅用于区分荣和/自摸的支付路径（沿用既有约定，不设 is_tsumo 字段，
// 避免同一事实两个出口）。
struct ScoreContext {
  int winner_seat = 0;
  int from_seat = -1;           // < 0 ⇒ 自摸；否则为放炮者座位
  bool winner_is_dealer = false;  // 庄家加成 +1 番（不是番种）
  bool is_rinshan = false;        // 和牌张是否本次杠的补牌

  bool Tsumo() const { return from_seat < 0; }
};

// 判番：返回**有序**（表序）的番种列表。0 番合法（返回空列表）。
// `han(手牌) = max over 拆法 Σ番种(D)`，**禁止跨拆法拼凑**；同分取枚举序第一个。
// 自摸/庄家**不在**返回列表内（自摸由 ctx.Tsumo() 派生，庄家在算分时叠加）。
std::vector<Yaku> DetectYaku(const std::vector<Tile>& concealed_with_winning_tile,
                             const std::vector<Meld>& melds, Tile winning_tile,
                             const ScoreContext& ctx);

// 兼容封装：返回番种 id 列表（= DetectYaku 的 id 序列），供既有断言与协议 `win_type` 使用。
// `winning_tile` / `ctx` 省略时按「自摸、非庄家、非杠上开花」解释（保守默认；
// 需要精确判番（三暗刻的荣和排除、杠上开花）请显式传入）。
std::vector<std::string> WinTags(const std::vector<Tile>& concealed_with_winning_tile,
                                 const std::vector<Meld>& melds,
                                 std::optional<Tile> winning_tile = std::nullopt,
                                 std::optional<ScoreContext> ctx = std::nullopt);

// ---------------------------------------------------------------------------
// 动作可选项与超时默认动作（RULESET §5）
// ---------------------------------------------------------------------------
struct Option {
  std::string action;       // "win" | "kong" | "pong" | "chow" | "ankan" | "kakan" | "pass"
  std::vector<Tile> tiles;  // win/pass 为空；kong/pong=[T]；chow=[a,b]；
                            // ankan/kakan=[恰好 1 个牌面元素]（与 kong 同形，PROTOCOL v1.6 §4.6：
                            // 元素多于 1 个 ⇒ ILLEGAL_ACTION；UI 显示 4 张是客户端的事）
};

// 顺序固定 win → kong → pong → chow → pass（PROTOCOL.md §5.7）
std::vector<Option> ClaimOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                 Tile discarded, int remaining, bool is_next_player);
// 打牌窗口（GAME_DESIGN_V2 §5.2 / 本批 R6）：顺序固定 **win → ankan → kakan**（其后隐含 discard，
// 不是 option）。`remaining` 为牌山剩余，暗杠/加杠都要求 ≥ 1 张补牌。
// `remaining` 省略（默认 −1 = 「牌山未知」）时**不产出** ankan/kakan —— 这是 B2 未接线前的安全默认，
// 保证房间行为与本批之前完全一致；B2 接线时显式传 `remaining_`。
std::vector<Option> DiscardOptions(const std::vector<Tile>& concealed_with_winning_tile,
                                   const std::vector<Meld>& melds, int remaining = -1);
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
// 算分（GAME_DESIGN_V2 §2，B1.1 裁决版）
// ---------------------------------------------------------------------------
// 主公式（逐项可算，零舍入）：
//   番种合计 = Σ yaku[].han —— **自摸不是番种**（已从番种表移除），
//              "自摸 ×2" 只活在支付公式的 2R/6R 里，不再计入番数（B1.1 裁决 B）
//   庄家加成 = winner_is_dealer ? 1 : 0        —— 不是番种，受**同一**封顶
//   封顶     ⇒ han = min(番种合计 + 庄家加成, 6)
//   倍率     ⇒ R = 2^han ∈ {1, 2, 4, 8, 16, 32, 64}
//   荣和：放炮者 −R、和牌者 +R；自摸：三家各 −2R、和牌者 +6R；荒庄：全 0。四家 delta 之和恒 0。
//   ⇒ 0 番荣和 = ±1、0 番自摸 = 三家 −2 / 和牌者 +6 —— 与旧倍率制逐位相等（回归锚点）。
struct ScoreResult {
  std::array<int, 4> delta{};  // 四家得失分，和恒为 0
  int multiplier = 0;          // R = 2^han，han ∈ [0,6] ⇒ R ∈ {1..64}（荒庄为 0）
  int per_payer = 0;           // 荣和 = R；自摸 = 2R
  int total = 0;               // 荣和 = R；自摸 = 6R
  int han = 0;                 // 封顶后的最终番数（含庄家加成；**不含自摸**）
  std::vector<Yaku> yaku;      // 有序番种（**不含**自摸/庄家）
  int dealer_bonus = 0;        // 庄家加成（0/1）
  bool rinshan = false;        // 杠上开花是否成立（与 yaku 中的 "rinshan" 同源）
};

// han → 倍率：`2^min(han, 6)`（han ≤ 0 视为 0）。整张 7 行表：0..6 → 1,2,4,8,16,32,64。
int MultiplierForHan(int han);

// 算分入口（§2.1 + §2.2）。`concealed_with_winning_tile` 为**逻辑 14 张**（已含和牌张）。
ScoreResult ScoreHand(const std::vector<Tile>& concealed_with_winning_tile,
                      const std::vector<Meld>& melds, Tile winning_tile,
                      const ScoreContext& ctx);
ScoreResult ScoreDraw();

// 兼容转发（旧 4 参签名）。`winning_tile` 可选：缺省时三暗刻无法执行「荣和排除和牌张补成的刻子」，
// 因此**房间与任何真实结算都必须显式传**；`winner_is_dealer` 不在旧签名里，故一律按闲家算
// （庄家加成由 B2 用 `ScoreHand` + `ScoreContext` 接线）。
ScoreResult ScoreWin(int winner_seat, int from_seat,
                     const std::vector<Tile>& concealed_with_winning_tile,
                     const std::vector<Meld>& melds,
                     std::optional<Tile> winning_tile = std::nullopt);

// 供 t4 使用：从 JSON 语义的角度判断选项是否包含指定动作
const Option* FindOption(const std::vector<Option>& options, std::string_view action);
bool OptionTilesMatch(const Option& option, const std::vector<Tile>& expected);

}  // namespace mahjong

#endif  // GAME_SERVER_MAHJONG_RULES_H_
