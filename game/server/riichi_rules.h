// 日麻规则库（纯函数，不依赖 actor 运行时）
//
// 规格来源：game/docs/GAME_DESIGN_RIICHI_DRAFT.md（草案 §2.2 / §2.3）
// 复用：牌张编码、排序、和牌形态、向听、副露合法性全部沿用 mahjong_rules（34 面 / 136 张口径一致）。
//
// 与 classic 的三处结构性差异（草案 §2.2）：
//   1. 役种有「门前 / 副露」两档番数，且「门前清」不是役（它是其他役的条件）；
//   2. 算分是「符 × 番 → 点数表」，不是 R = 2^han；
//   3. 有役满（本文件用 Limit::kYakuman + 独立基数表达，不塞进 han）。
//
// 本库不读时钟、不用随机设备、不持有全局可变状态 —— 同一输入必得同一输出。
#ifndef GAME_SERVER_RIICHI_RULES_H_
#define GAME_SERVER_RIICHI_RULES_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mahjong_rules.h"

namespace mahjong {
namespace riichi {

// 起家点数（草案 §2.3）。房间把它写进 points_，规则库只提供常量。
inline constexpr int kStartPoints = 25000;
// 立直棒（供托）与每本场点数
inline constexpr int kRiichiStickPoints = 1000;
inline constexpr int kHonbaPoints = 300;
// 立直后至少要剩这么多张可摸牌才允许宣言（日麻通行的「四张以下不可立直」）
inline constexpr int kMinRiichiWall = 4;

// 满贯以上的分档。kNone = 未封顶（普通手）。
enum class Limit : uint8_t {
  kNone = 0,
  kMangan = 1,     // 满贯 2000 base
  kHaneman = 2,    // 跳满 3000
  kBaiman = 3,     // 倍满 4000
  kSanbaiman = 4,  // 三倍满 6000
  kYakuman = 5,    // 役满 8000
};

struct Yaku {
  protocol::YakuId id = protocol::YakuId::kInvalid;  // 线协议枚举（PROTOCOL v2.1 §1.3.1）
  int han = 0;

  friend bool operator==(const Yaku& a, const Yaku& b) { return a.id == b.id && a.han == b.han; }
  friend bool operator!=(const Yaku& a, const Yaku& b) { return !(a == b); }
};

// 判番 / 算分上下文。`from_seat < 0` ⇒ 自摸（与 classic 的 ScoreContext 同约定）。
struct WinContext {
  int winner_seat = 0;
  int from_seat = -1;
  int dealer_seat = 0;  // 庄家座位（自摸的按家分摊要用它，不能只靠 seat_wind 反推）
  int honba = 0;        // 本场数（每本 300 点：荣和放炮者付、自摸三家各 100）
  int round_wind = 0;   // 0=东 1=南 2=西 3=北（场风）
  int seat_wind = 0;   // 自风，同上编码
  bool is_riichi = false;
  bool is_ippatsu = false;
  bool is_rinshan = false;  // 岭上开花
  bool is_haitei = false;   // 海底摸月（自摸）
  bool is_houtei = false;   // 河底捞鱼（荣和）
  bool is_chankan = false;  // 抢杠
  bool is_tenhou = false;   // 天和（庄家配牌即和，役满）
  bool is_chiihou = false;  // 地和（闲家第一巡自摸，役满）
  std::vector<Tile> dora_indicators;
  std::vector<Tile> ura_indicators;  // 仅立直和牌计入

  bool Tsumo() const { return from_seat < 0; }
};

struct ScoreResult {
  std::array<int, 4> delta {};  // 四家得失分；不含供托/本场时为 0 和
  int han = 0;                  // 含宝牌，不含役满折算
  int fu = 0;
  int points = 0;       // 和牌者总收入（荣和 = 放炮者付出；自摸 = 三家之和）
  int base = 0;         // 点数公式里的 base（满贯以上为分档值）
  Limit limit = Limit::kNone;
  int yakuman_count = 0;  // 役满倍数合计（双倍役满记 2，可叠加多个役满）
  int dora = 0;
  int ura_dora = 0;
  int aka = 0;  // 赤宝牌张数（每张 1 番；役满时不加算）
  std::vector<Yaku> yaku;
  int per_payer = 0;  // 荣和 = points；自摸 = 闲家付出（庄家为 2×base）
};

// 指示牌 → 宝牌牌面（数牌 9→1；风 东→南→西→北→东；三元 白→发→中→白）。非法输入原样返回。
Face DoraFaceFromIndicator(Tile indicator);

// 番数表（供测试与展示）。`closed` 决定门前/副露两档；非日麻番种/加成 id 返回 0。
int YakuHan(protocol::YakuId id, bool closed);
bool IsYakumanYaku(protocol::YakuId id);

// ---- 双倍役满（v1.18）----------------------------------------------------------
// 四种「双倍役满」各记 2 倍：`kokushi_13`（国士十三面）、`suuankou_tanki`（四暗刻单骑）、
// `chuuren_junsei`（纯正九莲宝灯）、`daisuushii`（大四喜）。其余役满 1 倍，非役满 0。
// 一手多役满时**倍数相加**（大四喜 + 字一色 = 3 倍 = 48000 / 闲家自摸每家 16000）。
int YakumanMultiplier(protocol::YakuId id);

// ---- 场风 / 自风 --------------------------------------------------------------
// 每「場」4 局（东场 4 局或南场 4 局）。半庄 = 东场 + 南场 = 8 局。
// 编码与 WinContext 一致：0=东 1=南 2=西 3=北。
inline constexpr int kRoundsPerWind = 4;

// round_index（0 起、只在该局轮庄时自增）→ 场风。东场 0..3 ⇒ 0，南场 4..7 ⇒ 1。
int RoundWindOf(int round_index);

// 自风 = 「庄家的自风等于场风」，其余按逆时针加一。
// 东场庄家=东(0)、南场庄家=南(1)；即 (round_wind + (seat - dealer + 4) % 4) % 4。
// **不能用 (seat - dealer) % 4**：那在东场对、到南场就整体差一圈（南家被当成东家，役牌判错）。
int SeatWindOf(int round_wind, int seat, int dealer);

// 手牌是否为门前清（无碰/吃/明杠/加杠；暗杠不破门清）。
bool IsClosedHand(const std::vector<Meld>& melds);

// ---- 国士无双 ------------------------------------------------------------------
// classic 没有这个役，所以它**不在**共享的 `CanWinExact` / `WaitingTiles` 里（那两层是
// 番数制的冻结口径）。日麻侧必须自己把国士的牌型补上，否则会出现「判番能认出 kokushi，
// 但牌型门禁先把它拒了」—— 表现为：手里已经是国士却看不到「胡」、听牌条空、也无法立直。
bool IsKokushiHand(const std::vector<Tile>& concealed_with_winning_tile,
                   const std::vector<Meld>& melds);

// 日麻和牌形：标准型 / 七对子（共享实现）∪ 国士无双。
bool IsRiichiWinningHand(const std::vector<Tile>& concealed_with_winning_tile,
                         const std::vector<Meld>& melds, Tile winning_tile, bool tsumo);

// 日麻等待张：标准型 / 七对子（共享 `WaitingTiles`）∪ 国士的等待张。
// 国士：13 种幺九字齐全 ⇒ 十三面听；只缺一种 ⇒ 单骑那张。
std::vector<Tile> RiichiWaitingTiles(const std::vector<Tile>& concealed,
                                     const std::vector<Meld>& melds);

// 可作立直宣言牌的牌面（v1.21）：`concealed_with_winning_tile` 里每一张「打出去之后仍在听牌」
// 的牌面各收一次，升序去重。副露非门前时返回空。
// 客户端拿它在手牌上做标记 —— 立直的宣言牌不是随便哪张都行，选错会被服务端拒（ILLEGAL_ACTION）。
std::vector<Face> RiichiDiscardFaces(const std::vector<Tile>& concealed_with_winning_tile,
                                     const std::vector<Meld>& melds);

// 立直后能否暗杠这一面：暗杠**不得改变听牌**（等待牌型与等待张都要一致）。
// `concealed_with_winning_tile` 是 14 张行动态，`drawn` 是刚摸到的那张（立直者必然有）。
// 判据：暗杠前（14 张去掉刚摸的 = 13 张）与暗杠后（10 张 + 暗杠组）的 WaitingTiles 必须相同。
bool CanAnkanAfterRiichi(const std::vector<Tile>& concealed_with_winning_tile,
                         const std::vector<Meld>& melds, Tile drawn, Face face, int remaining);

// 打牌窗口的可选项（日麻版）：顺序 win → riichi → ankan → kakan。
// 立直后：kakan 不再产出（立直手不可能有碰组），只有「不改变听牌」的暗杠会被放行
// （`drawn_tile` 未知时保守处理 —— 一个杠都不给）。
// `CanDeclareRiichi` 为真时追加一条 `{"riichi", []}`（宣告与打牌是同一条 action）。
std::vector<Option> DiscardOptions(const std::vector<Tile>& concealed_with_winning_tile,
                                   const std::vector<Meld>& melds, int remaining, bool riichi_declared,
                                   std::optional<Tile> drawn_tile = std::nullopt);

// 是否可宣言立直：门前清、点数足够由调用方判断、牌山够厚、且存在一张牌打出后听牌。
bool CanDeclareRiichi(const std::vector<Tile>& concealed_with_winning_tile,
                      const std::vector<Meld>& melds, int remaining);

// 鸣牌窗口的可选项（日麻版）。`furiten` 为真时不产出 win（振听）。
std::vector<Option> ClaimOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                 Tile discarded, int remaining, bool is_next_player, bool furiten);

// 有役的等待张：从 `WaitingTiles`（纯牌型口径）里筛出「按**荣和**至少有一个役」的牌面。
// 口径说明：
//   * 不含门前清自摸这类**自摸役** —— 提示语是「别人打出这张，我能荣和吗」；
//   * 已立直（`ctx.is_riichi`）时立直本身就是役，因此恒等于全部等待张；
//   * 宝牌不算役（与 ScoreHand / 房间窗口过滤同口径）。
// 用于听牌帧的 `waits[].yaku` / `no_yaku`（PROTOCOL v1.13 §12.8），**不参与**流局罚符判定
// （罚符看的是「形式听牌」，即 WaitingTiles 非空）。
std::vector<Tile> WinningWaits(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                               const WinContext& ctx);

// 重载：`waits` 由调用方传入（通常来自 `RiichiWaitingTiles`）。
// tenpai 帧里「等待张」已经被算过一次，本重载让它不必再算第二遍；语义与上面完全一致，
// 前提是 `waits` 必须等于 `RiichiWaitingTiles(concealed, melds)`。
std::vector<Tile> WinningWaits(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                               const WinContext& ctx, const std::vector<Tile>& waits);

// ---- AI 弃牌（日麻版）----------------------------------------------------------------
// 评分口径与共享的 `ChooseDiscard` **完全一致**（`BestShantenAfterDraw`，越小越好），只有
// **并列时**的牌面选择不同：共享实现按冻结牌序取最小牌面，日麻这里按「这手听牌能不能和」
// 决胜。原因：日麻有「无役不能和」，而向听数完全看不见役 —— 一个 1 向听的无役副露手比
// 一个 2 向听的断幺手更「近」，但前者永远和不了。
//
// 决胜顺序（逐级只在上一步并列的候选里比较）：
//   1. 等待张里**有役**的枚数（荣和口径，`WinningWaits` 同源）；
//   2. 门清且能立直时，无役听牌也算活（立直本身就是役），按等待张数计；
//   3. 仍未分出的，按「未振听优先」；
//   4. 再并列 ⇒ 冻结牌序最小牌面（与共享实现一致，保证确定性）。
//
// `ctx` 只需场风/自风/立直/宝牌指示牌这类「判役用得到的状态」；`can_riichi` 由调用方按
// 「门前 + 点数够 + 牌山够厚 + 未振听」算好（规则库不该自己去读房间的点数）。
// `own_discards` 用来判「打这张会不会落舍て牌振听」—— 它属于房间的公开状态，同样由调用方给。
// `drawn_tile` 只用于返回**同一张物理牌**（打摸牌语义），不影响选牌结果。
std::optional<Tile> ChooseDiscard(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                  const std::vector<Tile>& own_discards, const WinContext& ctx, bool can_riichi,
                                  std::optional<Tile> drawn_tile = std::nullopt);

// 抢杠窗口的可选项（v1.15）：只有 **win / pass** —— 别人加杠的那张牌不能被吃/碰/大明杠。
// 抢杠本身是 1 番役，所以「无役」的手牌也能抢；因此判役时必须把 `is_chankan` 算进去
// （本函数内部强制置位，调用方的 ctx 只提供场风/自风/立直等状态）。
std::vector<Option> ChankanOptions(const std::vector<Tile>& hand, const std::vector<Meld>& melds,
                                   Tile tile, const WinContext& ctx, bool furiten);

// 流局听牌罚符：3000 点由未听牌家平摊给听牌家（1 家听 1000/家、2 家听 1500/家、3 家听 3000）。
// 0 或 4 家听牌时无人收付。返回四家 delta，和恒为 0。
std::array<int, 4> DrawPenalty(const std::array<bool, 4>& tenpai);

// 击飞（v1.19，雀魂口径）：任一持点**严格小于 0** 即为击飞，本场立即终局。
// 恰好 0 点不算（0 点继续对局，只是立直不了）；负点不截断，照实进顺位与精算。
bool IsBusted(const std::array<int, 4>& points);

// 场终的剩余供托归属（v1.20，雀魂口径）：台面上的立直棒全部归**一位**。
// 立直宣言时那 1000 点已经从各家持点里扣走（delta 记 −1000），棒挂在供托上等下一次和牌；
// 若本场以荒庄或击飞收尾，没有和牌者来收，棒就会凭空消失（四家持点之和不再等于 100000）。
// 一位 = 持点最高者；**同点取席位号小的**（起家优先），与 match_end.rankings 的排序同口径。
// `sticks <= 0` 时 seat = -1、points = 0（无棒可归）。`points` = sticks × 1000。
struct KyoutakuAward {
  int seat = -1;
  int sticks = 0;
  int points = 0;
};
KyoutakuAward AwardLeftoverKyoutaku(const std::array<int, 4>& points, int sticks);

// 振听判定：门前手牌的听牌张里是否有任何一张已在自家牌河中（同巡振听的持久部分）。
// `concealed` 为等待态（13 张口径）。
bool IsPermanentFuriten(const std::vector<Tile>& concealed, const std::vector<Meld>& melds,
                        const std::vector<Tile>& own_discards);

// 算分入口。`concealed_with_winning_tile` 为逻辑 14 张（已含和牌张）。
ScoreResult ScoreHand(const std::vector<Tile>& concealed_with_winning_tile,
                      const std::vector<Meld>& melds, Tile winning_tile, const WinContext& ctx);

// 判番（不含宝牌；宝牌在 ScoreHand 里按指示牌叠加）。测试与 UI 用。
std::vector<Yaku> DetectYaku(const std::vector<Tile>& concealed_with_winning_tile,
                             const std::vector<Meld>& melds, Tile winning_tile,
                             const WinContext& ctx);

// 符计算（单测逐档对照）。役满 / 七对子有独立口径。
int FuOf(const std::vector<Tile>& concealed_with_winning_tile, const std::vector<Meld>& melds,
         Tile winning_tile, const WinContext& ctx);

}  // namespace riichi
}  // namespace mahjong

#endif  // GAME_SERVER_RIICHI_RULES_H_
