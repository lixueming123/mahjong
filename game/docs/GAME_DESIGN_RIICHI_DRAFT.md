# 日麻玩法接入：差距分析与实施草案（GAME_DESIGN_RIICHI_DRAFT）

- 文档 ID：GAME_DESIGN_RIICHI_DRAFT
- 版本：**v0.1-draft**
- 状态：**草案，未冻结**。本文不替代 `GAME_DESIGN.md` v1.10 / `RULESET.md` v1.3（两者继续作为现行麻将玩法的权威源头）。日麻是否成为一个独立 `mode` 由 §6 的待裁决项决定。
- 作者 / 负责人：待指派（建议 game-architect 牵头，tech-architect 出架构评估）
- 上游：`GAME_DESIGN.md` v1.10、`RULESET.md` v1.3、`PROTOCOL.md` v1.6、`UI_UX_SPEC.md`
- 关联代码：`game/server/mahjong_rules.{h,cc}`、`game/server/room.{h,cc}`、`game/server/protocol.h`、`game/client/game_state.py`

---

## 0. 结论摘要（先看这五条）

1. **牌张与牌型层可以直接复用**：34 面 / 136 张、4 座位、13 张起手、吃碰杠、`EnumerateDecompositions`、`Shanten`、`IsWinningHand` 与日麻口径一致，不需要重写。
2. **算分体系必须整体另起**：现行是「番数制 + 封顶 6 番 + `R = 2^han`」（`mahjong_rules.cc:1344` `kHanCap=6`），日麻是「符 × 番 → 点数表 + 满贯/役满分档」。两者在数值上无法共存于同一个 `ScoreHand`，**不要给现有函数加 `mode` 分支**。
3. **役种表从 14 扩到约 36**，且半数役种需要「门前 / 副露降番」两档番数；现行 `Yaku` 只有 `{id, han}`，表达不了役满，也表达不了「同一役两种番数」。
4. **立直、振听、宝牌是三块全新的房间状态**，其中宝牌最伤：牌山要切出 14 张王牌，而 `remaining_ = kTileCount - next_index_`（`RULESET.md` §1.5 标为「实现红线」）的前提被打破。
5. **零和假设会被打破**：供托（立直棒）与本场（连庄）引入第三方池，「四家 delta 之和恒 0」不再成立，`GameSettlement.totals`、`match_results.final_score` 的语义都要重新定义。

---

## 1. 复用面 / 改造面对照

| 层 | 现状位置 | 日麻动作 | 规模 |
| --- | --- | --- | --- |
| 牌张编码、排序、解析 | `mahjong_rules.h` §牌张 | 直接复用 | — |
| 牌山生成 `BuildWall` | 同上 | 复用 RNG 与洗牌，**切分改** | S |
| 发牌 `DealTiles` | 同上 | 52 张配牌 + 庄家首摸（现行是 53 张直发） | S |
| 和牌型判定 `IsWinningHand` / `EnumerateDecompositions` | 同上 | 直接复用 | — |
| 向听数 `Shanten` | 同上 | 直接复用 | — |
| 副露合法性 `ClaimOptions` | 同上 | 复用，但**需加振听参数** | M |
| 役种表 `DetectYaku` | `mahjong_rules.cc:826` 起 | **重写**（14 → ~36，含降番与役满） | L |
| 算分 `ScoreHand` | `mahjong_rules.cc:1361` | **另起** `ScoreRiichiHand` | L |
| 动作可选项 / 窗口 | `room.cc` | 加 `riichi`，加振听过滤 | M |
| 牌山与王牌 | `room.cc` | **切 14 张王牌 + 宝牌/里宝指示牌** | L |
| 场风自风、本场、供托 | 无 | **全新** | M |
| 流局 | `ScoreDraw()` 全 0 | 听牌罚符 3000 | M |
| 协议 | `protocol.h` | 加字段 + 加 `riichi` 动作 + 改 `win` 载荷 | M |
| 客户端 | `game_state.py` / `render/*` | 立直按钮、宝牌、点数制、结算改「符×番」 | L |
| AI | `ChooseDiscard` / `ChooseClaimAnswer` | 加立直判断与现物防守 | M |
| 战绩 | `db_mysql.cc:244` `match_results` | 加 `mode` 列，`final_score` 语义改 | S |

---

## 2. 逐层差距

### 2.1 牌张与牌山（复用为主，一处被打破）

- 牌面集合、编码正则、排序键、`kTileCount=136` 全部一致，**不动**。
- 日麻配牌是 52 张（每人 13），庄家第 14 张来自**第一次摸牌**；现行实现是发牌时直发 53 张（`RULESET.md` §1.4 第 2 条）。日麻侧建议改回「52 张 + 正常摸牌」，否则「第一摸自摸 / 杠上开花」的事件语义无处安放。
- **王牌**：日麻要保留 14 张（岭上 4 + 宝牌指示 5 + 里宝指示 5），可摸牌变为 `136 − 52 − 14 = 70`。现行 `kWallSizeAfterDeal = 83` 与 `remaining_ = kTileCount - next_index_`（`room.cc:920`）都建立「没有王牌」的前提，日麻必须显式区分「可摸区」与「王牌区」，并把 `remaining` 改成 `drawable_total - drawn` 的派生值。
- 岭上牌从王牌头部取，宝牌指示牌在开局翻开第一张、杠后翻下一张。

### 2.2 役种表（14 → 约 36）

现行 14 役（`mahjong_rules.cc:826-840`）：`menzen / tanyao / pinfu / iipeikou / yakuhai / rinshan / sanshoku_doujun / ittsuu / toitoi / sanankou / honitsu / shousangen / pure_one_suit / seven_pairs`。

日麻的差异不在「数量」，而在**三条结构性差异**：

1. **门前 / 副露降番**：三色同顺 2/1、一气通贯 2/1、混全带幺九 2/1、纯全带幺九 3/2、混一色 3/2、清一色 6/5。现行实现是单一 flat 番数，`Yaku{id, han}` 表达不了「同一役两个番数」。
2. **「门前清」不是役**：现行有一个独立的 `menzen` 1 番役，日麻没有这个东西（门前清是其他役的**条件**与**加番项**）。这条必须删，否则日麻会在「门清 + 断幺」上多送 1 番。
3. **役满**：国士无双、四暗刻、大三元、字一色、绿一色、清老头、九莲宝灯、四杠子、小四喜、大四喜、天和、地和。现行 `han` 是 `int` 且封顶 6，役满需要独立的 `Limit` 概念。

还需要新增的动作相关役：**立直（1）、一发（1）、门前清自摸和（1）、抢杠（1）、海底摸月（1）、河底捞鱼（1）**。岭上开花已有。

> ⚠️ 现行 `RULESET.md` §12 明确「抢杠和**永久不做**」。日麻必须做，因此该条款只能对 `mode=riichi` 失效，**不能**被无条件删除——现行玩法的冻结数值与测试依赖它。

`yakuhai` 现状只认箭牌（`IsDragonFace`，面 31..33 = 5z/6z/7z，见 `mahjong_rules.cc:686`），**没有风牌**。日麻的役牌包含场风、自风、三元牌，需要新的 `ScoreContext` 字段承载 `round_wind` / `seat_wind`。

### 2.3 算分（两套体系不可共存）

| 项 | 现行（`RULESET.md` §6） | 日麻 |
| --- | --- | --- |
| 输入 | 番种合计（自摸不算番） | 番 + 符 |
| 公式 | `R = 2^min(han,6)` | `base = fu × 2^(2+han)`，再按满贯/跳满/倍满/三倍满/役满分档 |
| 荣和 | 放炮者 −R，和牌者 +R | 放炮者付 `4×base`（庄 `6×base`），向上取整到百 |
| 自摸 | 三家各 −2R，和牌者 +6R | 庄家：每家 `2×base`；闲家：庄家 `2×base`、闲家 `1×base`，各自进位 |
| 封顶 | 6 番 | 无封顶，走分档表 |
| 起分 | 从 0 累加 | 每人 25000，返 30000 |
| 流局 | 全 0 | 听牌家平分 3000 罚符 |

结论：`ScoreResult` 的 `multiplier / per_payer / total` 三个字段是番数制的产物，日麻应返回 `{fu, han, limit, points, delta[]}`。建议抽一个**两者共用的上层结构** `ScoreOutcome { delta[4]; points; han; fu; limit; yaku[]; }`，房间与协议只消费这个，不消费具体规则。

### 2.4 立直 / 振听 / 宝牌（全新状态）

- **立直**：需要新动作 `riichi`（打牌窗口的一个 option）+ 1000 点供托 + 门清校验 + 立直后手牌锁定（只允许摸切，暗杠受限）+ 一发窗口（立直后一巡内、且中间无鸣牌）。
- **振听**：三种（自己牌河、见逃、同巡）。现行 `ClaimOptions` 没有任何「放过」记忆，`win` 是无条件给出的，所以必须给房间加 `furiten_[4]`，并在 `ClaimOptions`/`DiscardOptions` 调用处过滤 `win`。
- **宝牌**：指示牌 → 宝牌映射（数牌 +1、9→1；风牌东→南→西→北→东；三元白→发→中→白）。里宝只在立直和牌时揭示。
- 立直状态还要进**动作事件**广播（`riichi` 打出 1000 点、横置），客户端要能画立直棒。

### 2.5 场风 / 自风 / 本场 / 供托

- 现行只有 `round_index ∈ 0..3`（`room.h`），语义是「一场四局」，没有东/南圈。日麻需要 `round_wind`（东/南）与 `seat_wind`（每局随庄家轮转）。
- 本场（连庄计数）已有 `renchan_count_`，可复用，但**每本 300 点的支付**要新增。
- 供托是房间级池子，`GameSettlement` 需要新增字段，否则和牌者拿不到立直棒。

### 2.6 流局

现行 `ScoreDraw()` 返回全 0。日麻：听牌家各 1000（合计 3000，未听牌家摊付，听牌家数 0 或 4 时无人收付）+ 庄家听牌连庄 / 未听牌轮庄。庄家转移规则与现行「荒庄一律按庄家荒庄、连庄计数 +1」不同，需在日麻侧另写。

### 2.7 协议

- `action` 词表加 `riichi`（`protocol.h:147` `IsKnownActionName`）。
- `game_start` 加 `round_wind` / `seat_wind` / `points[4]` / `dora_indicators[]`。
- `game_end.win` 加 `fu`、`points`、`limit`（`none/mangan/haneman/baiman/sanbaiman/yakuman`）、`ura_dora[]`、`riichi`（是否立直和牌）；`han_breakdown` 的 `tsumo` 恒 0 的约定在日麻下由「门前清自摸和 1 番」取代。
- `create_room` 加 `mode`（`classic` 默认 / `riichi`），`room_joined` 回显。
- `match_end` 的排名口径从 `final_score`（零和得失分）改为「终局点数」。
- **版本**：以上属于不兼容变更，`PROTOCOL.md` 需升大版本（v1.6 → v2.0），并保留 `mode` 缺省 = 现行玩法的兼容路径。

### 2.8 客户端

- `render/lobby.py`：玩法选择（两选一卡片）。
- `game_state.py`：`MELD_TYPES` 不该混入 `riichi`（立直不是副露）；新增 `points[]`、`round_wind`、`seat_wind`、`dora`、`riichi_seats[]`；`check_hand_count` 与 `kans` 可复用。
- `render/table.py`：宝牌指示牌、点数条（25000 起）、场风自风、立直棒。
- `render/actionbar.py`：立直按钮（听牌且门清且点数 ≥1000 才亮）。
- `render/result.py`：`_yaku_detail`（`result.py:96`）的「合计 N 番（2^N 分）」文案在日麻下必须改成「符 × 番 → 点数」，并支持役满文案。

### 2.9 AI

现行 AI 只有向听 + 向听不劣化（`ChooseDiscard` / `ChooseClaimAnswer`）。日麻至少要补两件事，否则玩法展示不出来：

1. **立直判断**：听牌 + 门清 + 非振听 + 打点/巡目阈值。
2. **防守**：对手立直后打现物（至少现物，筋与壁可后置）。

不做的话，三个 AI 永远不立直，日麻最核心的「读牌博弈」在演示里为零。

---

## 3. 架构改造方案

### 3.1 第一步：把共享原语抽出去（不改行为）

```
server/tiles.{h,cc}         Tile/Face/Suit/Rank、Parse/Format/Sort、BuildWall
server/hand_shape.{h,cc}    Decompose、EnumerateDecompositions、IsWinningHand、Shanten、ClaimOptions
server/mahjong_rules.{h,cc} 变薄：只留现行玩法的役种 + ScoreHand，依赖上面两层
server/riichi_rules.{h,cc}  新建：日麻役种 + 符计算 + 点数表 + 立直/振听/宝牌判定
```

这一步是纯搬迁 + 转发，验收标准是**现有单测一行不改、全绿**（`mahjong_rules_test.cc` 110 KB 是这次的回归网）。

### 3.2 第二步：规则集分派

```cpp
enum class RuleSetId { kClassic, kRiichi };

struct WinContext {
  int winner_seat;
  int from_seat;              // < 0 ⇒ 自摸
  Tile winning_tile;
  bool winner_is_dealer;
  int round_wind;             // 日麻用；classic 忽略
  int seat_wind;
  bool is_rinshan;
  bool is_haitei;             // 海底/河底
  bool is_chankan;
  bool is_riichi;
  bool is_ippatsu;
  std::vector<Tile> dora_indicators;
  std::vector<Tile> ura_indicators;   // 仅立直和牌
};

struct ScoreOutcome {
  std::array<int, 4> delta {};  // 含供托/本场的四家净变化
  int han = 0;
  int fu = 0;                   // classic 恒 0
  Limit limit = Limit::kNone;
  int points = 0;               // 和牌点数（classic = 2^han）
  std::vector<Yaku> yaku;
};

// 两个实现，room 只依赖这个签名
ScoreOutcome ScoreClassicHand(const std::vector<Tile>&, const std::vector<Meld>&, const WinContext&);
ScoreOutcome ScoreRiichiHand(const std::vector<Tile>&, const std::vector<Meld>&, const WinContext&);
```

`room.cc` 里出现 `if (ruleset_ == kRiichi)` 的**唯一合法位置**是「选哪个函数」和「哪个 option 表」，其余分支都应该被 `ScoreOutcome` 与 `Option` 吸收。

### 3.3 RuleConfig（构造期注入，替代 `constexpr`）

| 字段 | classic | riichi | 影响 |
| --- | --- | --- | --- |
| `dead_wall_size` | 0 | 14 | 牌山切分、`remaining` |
| `deal_count` | 53（含庄家第 14 张） | 52 + 首摸 | 开局事件序列 |
| `kopies` / `face_count` | 4 / 34 | 同 | 不动 |
| `start_points` | 0 | 25000 | 结算基准 |
| `return_points` | — | 30000 | 排名口径 |
| `rounds` | 4（东南西北各一） | 4 或 8（东场/半庄） | 场次长度 |
| `has_riichi` / `has_dora` / `has_furiten` | false | true | 动作窗口 |
| `allow_chow` | true | true | 同 |
| `chankan` | 永不做 | 必做 | 抢杠 |
| `draw_penalty` | 0 | 3000 | 流局 |
| `score_fn` | `ScoreClassicHand` | `ScoreRiichiHand` | 分派点 |

> `kTileCount` / `kWallSizeAfterDeal` 现在是 `inline constexpr` 且被 `static_assert` 钉死（`mahjong_rules.h:33-37`）。日麻不需要改总张数，只需让**牌山切分**参数化，因此这两个常量可以保留，改的是房间侧的 `next_index_` 与 `remaining_` 语义。

### 3.4 房间新增状态

```
dead_wall_[14]          王牌（岭上 4 / 宝牌指示 5 / 里宝指示 5）
dora_indicators_        已翻开的指示牌（开局 1 张，每杠 +1）
points_[4]              点数（起点 25000）
kyoutaku_               供托（立直棒数 ×1000）
honba_                  本场数
round_wind_ / seat_wind_
riichi_declared_[4]     立直宣言
riichi_turn_[4]         一发窗口起点（巡目）
ippatsu_valid_[4]
furiten_[4]             振听（含见逃记忆）
```

新增 `Phase` 不需要——立直是打牌窗口的一个 option，不是独立阶段。

### 3.5 服务端改造清单（按函数，`room.cc` 1903 行）

结论：**状态机骨架零重写**——`Phase` 六态、`Pump` 的循环、`seq` 窗口、`AsyncGate` 原子性、AI pacing、`Deliver` 并行投递全部保留。改动是「加字段 + 加分支 + 换分派」，但没有一个函数可以完全不动。

| 位置 | 改什么 | 规模 |
| --- | --- | --- |
| `RoomActor` 构造 | 多一个 `RuleSetId` + config 入参（`lobby.cc:191` 的 `Spawn` 同步加参） | S |
| `StartMatch` / `StartRound` | 切 14 张王牌、翻宝牌指示牌、`points_[4]` 置 25000、`round_wind_`/`seat_wind_`、`honba_` 清零 | M |
| `Pump`（`kTurnStart`） | 荒庄判据从 `remaining_ <= 0` 改为「**可摸区**耗尽」——岭上补牌不算荒庄 | S |
| `DrawForCurrentSeat` | `remaining_ = kTileCount - next_index_`（`room.cc:920`）改成扣掉王牌；岭上补牌从王牌取，不走 `next_index_` | S |
| `OpenDiscardWindow` | option 表按 mode 分化：立直后只剩 `win` + 摸切 | M |
| `ApplyDiscard` | 立直宣言（打牌与宣言同一条消息，不能拆两次交互）；牌河落子同时维护振听记忆 | M |
| `OpenClaimWindow` / `ResolveClaimWindow` | 振听过滤 `win`；有人 `pass` 掉可和牌 ⇒ 记见逃振听 | M |
| `ApplySelfKan` | 每杠翻一张新宝牌指示牌；明杠/暗杠/加杠的岭上补牌统一走王牌 | M |
| `EndRoundWithWin` | `ScoreHand` → 按 mode 分派 `ScoreRiichiHand`；里宝只在立直时算；供托与本场的支付 | M |
| `EndRoundWithDraw` | `ScoreDraw()` 全 0 → 听牌罚符；听牌判定可复用 `BuildTenpaiFrame` 里的现有逻辑 | M |
| `AdvanceAfterRound` | 庄家听牌连庄 / 未听轮庄（现行是「荒庄一律连庄」）；本场累加；`match_totals_` 语义 | M |
| `ComputeFinalRanks` / `BuildMatchEnd` | 排名从「累计得失分」改为「终局点数」，同点按起家顺位 | S |
| `BuildGameStart` / `BuildStateSync` / `BuildGameEnd` | 加 `round_wind` / `seat_wind` / `points` / `dora_indicators` / `fu` / `limit` / `ura_dora` | M |
| `Settlement`（`room.h`） | 加 `fu` / `limit` / `points` / `riichi` / `ura_dora` / `kyoutaku` / `honba` | M |

**不要**给 `Settlement` 的 `multiplier / per_payer / total` 硬套日麻语义（日麻没有「倍率」和「每家固定支付」）。建议 `Settlement` 只保留 `delta` + 一个 `ScoreDisplay` 变体（classic 的倍数 / 日麻的符番），`BuildGameEnd` 按变体序列化，否则这个 builder 会退化成一堆 `if (mode)`。

`room.cc` 之外的服务端文件：

| 文件 | 改动 | 规模 |
| --- | --- | --- |
| `lobby.cc` | `CreateRoom` 解析 `mode` 并透传；`ListRooms` 回显 mode；`OnRoomFinished` 落库带 mode | 约 60 行 |
| `protocol.h` | `IsKnownActionName` 加 `riichi`；`create_room` 的 mode 字段校验 | 约 20 行 |
| `db.h` / `db_mysql.cc` | `match_results` 加 `mode` 列（含建表语句与迁移） | 约 30 行 |
| `main.cc` | 可选 `--mode` 作为服务端默认玩法 | 约 15 行 |
| 测试 | 建议新建 `riichi_room_test.cc`，不要把日麻用例塞进 126 KB 的 `mahjong_room_test.cc` | 新增 |

**四个最易翻车的点**：

1. `remaining_` 有两个消费者：客户端剩余牌数显示、`DiscardOptions` 的杠可用性判定。日麻要同时保证「可摸区剩 0 时不能再杠」与「岭上补牌不消耗可摸区」，这两个派生值写错任意一个，e2e 的剩余牌断言就会红。
2. 零和。`match_totals_`、`Settlement.totals`、`final_rank`、`match_results.final_score` 四处都建立在「四家之和 = 0」上；日麻是点数制 + 供托池，**字段名不能复用**，否则客户端与 DB 会同时持两套语义。
3. classic 逐字节不变。1903 行里几乎每段都挂着 `GAME_DESIGN §x.y` / `RULESET §x` 的冻结引用，加分支的正确写法是「classic 分支原语句不动，riichi 走新函数」，**不要**在原表达式里插 `mode == kRiichi ?`。
4. 天和。`TsumoWinningTile` 现在用「手里最大的牌」兜底，因为 classic 庄家第一打没有摸牌记录；日麻改回「庄家首摸」后这个兜底必须一起去掉，否则天和会判到错误的和牌张上（进而影响役种与符）。

---

## 4. 里程碑

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **M0** | 本文评审 + `RIICHI_RULESET.md` 冻结（役种表、符表、点数表逐条数值） | 数值表可被单测逐格对照 |
| **M1** | 抽 `tiles` / `hand_shape`，`mahjong_rules` 变薄 | 现有 `mahjong_rules_test` 全绿、`room` 行为零变化 |
| **M2** | `riichi_rules`：36 役 + 符计算 + 点数表 + 宝牌/立直/振听纯函数 | `riichi_rules_test` 覆盖 1 番 20 符 ~ 役满各档 |
| **M3** | 房间：王牌切分、立直、振听、流局罚符、供托/本场、场风自风 | `mahjong_room_test` 增加 riichi 用例；经典用例不变 |
| **M4** | 协议 v2 + 客户端（大厅选玩法、立直按钮、点数、结算） | `gateway_smoke` / `client_headless` 双 mode 通过 |
| **M5** | AI 立直判断 + 现物防守 | 统计：AI 立直率 > 0，被立直后放炮率可测 |
| **M6** | e2e（一局 + 一场）+ `--seed` 确定性回归 + 战绩新增 `mode` 列 | `--mode riichi --seed N` 两次跑出同一点数序列 |

M1 是纯重构，**必须独立提交**，不要和 M2 混在一个 PR 里——否则规则库出问题时无法判断是搬迁错还是新逻辑错。

---

## 5. 红线与风险

1. **不要给 `ScoreHand` 加分支**。它被 110 KB 单测按现行数值冻结，一旦内部出现 `if (riichi)`，回归网就废了。
2. **`remaining` 的派生口径不能破**。现行 `RULESET.md` §1.5 明确要求派生值、禁止自减计数器；日麻要在此基础上再减王牌数，写错会导致客户端剩余牌数漂移。
3. **零和假设失效**。供托/本场是第三方池，`GameSettlement.totals` 与 `db.h:101` 的「四行之和 = 0」注释都要改成「四家 + 池 = 0」。
4. **战绩混排**。`match_results` 必须加 `mode`，否则两套玩法的 `final_score` 会被排到一张榜上（一个是零和得失分，一个是 25000 起的点数，不可比）。
5. **冻结条款的适用范围**。「抢杠和永久不做」只能对 classic 生效，不能用「覆盖旧文档」的方式删除。
6. **客户端文案耦合**。`render/result.py:96` 的 `_yaku_detail` 把「合计 N 番（2^N 分）」写死，日麻下是错的；`docs_drift_test.py` 目前只查 `tsumo` 公式，不查这条，容易漏。

---

## 6. 待裁决（需要拍板后才能进 M1）

1. **玩法是独立 `mode` 还是替换现有玩法？** ✅ 已定：独立 `mode`，`create_room` 的 `"mode"` 字段，缺省 `classic`。
2. **东场还是半庄？** ✅ 已定：先做东场 4 局（复用现有 `round_index` 与连庄上限），半庄后置。
3. **是否做食断（喰い断）与赤宝牌？** ✅ 已定：食断**开**（`tanyao` 不区分门前/副露）；赤宝牌**做**
   （v1.18：`0m`/`0p`/`0s` = 5 的第 4 张，每张 +1 番；classic 不含赤牌语义）。
4. **是否做双倍役满**？ ✅ 已定：**做**（v1.18：国士十三面 / 四暗刻单骑 / 纯正九莲 / 大四喜 = 2 倍）。
5. **AI 防守做到什么程度？** 未做：当前 AI 只做立直宣言，没有现物/筋防守（M5）。
6. **战绩榜是否合并两玩法？** 未做：`match_results` 还没有 `mode` 列（3c/M4）。

---

## 7. 实施进度（3a + 3b 已完成）

### 7.1 落地清单

| 项 | 状态 | 位置 |
| --- | --- | --- |
| 日麻规则库（役种 / 符 / 点数表 / 宝牌 / 立直可用性 / 振听） | ✅ | `game/server/riichi_rules.{h,cc}`（新增） |
| 规则库单测（16 项，逐档对照标准点数表） | ✅ | `game/server/riichi_rules_test.cc` |
| 玩法分派（`GameMode` + `ParseGameMode`） | ✅ | `game/server/room.h`、`room.cc` |
| 王牌 14 张切分 / 可摸区 122 / `remaining` 派生 | ✅ | `RoomActor::StartRound`、`DrawReplacementTile` |
| 宝牌指示牌（开局 1 张，每杠 +1）/ 里宝（仅立直和牌） | ✅ | `FlipDoraIndicator`、`BuildRiichiContext` |
| 立直宣言（`action:8` + 打牌同一条消息） | ✅ | `HandleAction`、`DeclareRiichi` |
| 一发窗口（下一摸前有效，鸣牌即清） | ✅ | `ippatsu_` / `ippatsu_armed_` |
| 振听（自家牌河 + 见逃，荣和过滤） | ✅ | `IsFuriten`、`ClaimOptionsFor` |
| 持点 25000 / 供托 1000 / 本场 300 | ✅ | `points_`、`kyoutaku_`、`honba_` |
| 场终排名按持点 | ✅ | `ComputeFinalRanks`、`BuildMatchEnd` |
| 击飞终局（任一持点 < 0 即终局，0 点继续，箱下照算） | ✅ | `riichi::IsBusted`、`AdvanceAfterRound`（v1.19） |
| 场终剩棒归一位（荒庄/击飞收尾时，供托不蒸发） | ✅ | `riichi::AwardLeftoverKyoutaku`、`AdvanceAfterRound`（v1.20） |
| AI 立直（门清听牌即宣言） | ✅ | `PlayAutomatedDiscard` / `BotShouldRiichi` |
| 房间冒烟（模式 / 牌山 / 立直拒绝） | ✅ | `game/server/riichi_room_test.cc` |
| 端到端（真服务端 `mode=riichi` 打一局） | ✅ | `game/tests/riichi_e2e_test.py` |
| `create_room.mode` 解析与透传 | ✅ | `player_lobby_agent.cc`、`LobbyActor::CreateRoomWithMode` |

### 7.2 协议增量（classic 帧字节不变）

- `create_room` 请求：新增可选 `"mode"`（`classic` / `riichi`），未知值回 `BAD_REQUEST`。
- `game_start`（仅 riichi）：`points[4]`、`dora_indicators[]`、`round_wind`、`seat_wind`。
- `state_sync`（仅 riichi）：`points[4]`、`dora_indicators[]`。
- `action`：新增动作名 `riichi`，`tiles` 恰 1 个牌面（宣言后打出的那张）。
- `game_end`（仅 riichi）：`points_after[4]`；`win` 追加 `fu` / `points` / `limit` / `dora` /
  `ura_dora` / `riichi` / `dora_indicators` / `kyoutaku` / `honba`。
- **已写入正式协议**：`PROTOCOL.md` **v1.13** 新增 §12.8（听牌帧的 `yaku` / `no_yaku`，PB-2 日麻口径 1000 µs）；
  v1.12 新增第 12 章（玩法开关、`game_start`/`state_sync`/`game_end`/`action{riichi}`/`stats` 分桶、牌山 122/69 口径、无役不能和、抢杠对 classic 的适用范围），
  并附 v1.12 修订行；**零新增错误码**，classic 帧字节逐字节不变。

### 7.3 3c 与 M4（本轮已补）

- **流局听牌罚符**：`riichi::DrawPenalty`（3000 点平摊：1 家听 1000/家、2 家听 1500/家、
  3 家听 3000；全听/全不听无人收付），`game_end` 追加 `tenpai[4]`。
- **荒庄庄家判据**：日麻改为「庄家听牌才连庄」，未听牌则轮庄（本场随之清零）；classic 不变。
- **无役不能和**：无役（宝牌不算役）时窗口里不提供 `win`（`RoomActor::HasYakuForWin` 用完整上下文
  判，立直/一发/海底/杠上开花这类状态役也计入）；`ScoreHand` 另有第二道防线（无役返回 0 点）。
- **振听不能立直**（v1.17）：`CanDeclareRiichiNow` 用「打完这张之后的 13 张」判振听（舍て牌 / 见逃），
  此前只查「听牌 + 点数 + 牌山」，振听的人也能宣言立直——这是明确的规则违反，不是体验问题。
- **天和 / 地和**（v1.17）：第一巡自摸的役满，`WinContext.is_tenhou / is_chiihou` 由房间按
  「牌河是否为空 / 本家牌河是否为空 + 无副露」置位；此前缺失，庄家配牌即和只能拿到 1 番门清自摸。
- **国士无双之前根本和不了**（v1.17 修复）：共享的 `CanWinExact` / `WaitingTiles` 是番数制口径，
  只认标准型 + 七对子；`riichi::DetectYakuman` 虽然能判 `kokushi`，但**牌型门禁先把它拒了**。
  后果：手里已经是国士却拿不到 `win` 选项、听牌条为空、连立直都宣言不了、荒庄还会被算成未听牌。
  修法：日麻侧新增 `IsKokushiHand` / `IsRiichiWinningHand` / `RiichiWaitingTiles`（标准型 ∪ 国士），
  并把打牌窗口、鸣牌窗口、抢杠、听牌帧、立直资格、荒庄听牌、房间的和牌校验全部切过去。
  **共享层一行未动**——classic 没有这个役，不能「顺手」修在那里。
- **立直后的合法暗杠（v1.16）**：立直手只有在「暗杠不改变听牌」时才允许暗杠
  （`riichi::CanAnkanAfterRiichi`：14 张去掉刚摸牌 vs 暗杠后 10 张 + 暗杠组，两份 `WaitingTiles` 必须相同）。
  `kakan` 永不提供——立直手是门前清，结构上就没有碰组。此前一律拒绝，会误杀合法操作。
- **抢杠（v1.15）**：日麻加杠不再直接落地，先开 `window:3` 的窗口（只有 `win`/`pass`，
  且只发给真能和的家，附带被抢牌面 `tile`）；抢杠 1 番使「无役」手也能抢；和了则杠不成立
  （不补岭上牌、不翻宝牌），第 4 张从加杠者手牌转入和牌者手牌。国士抢暗杠仍不做。
- **半庄（v1.14）**：`create_room.length` = `tonpu`（缺省，东场 4 局）/ `hanchan`（半庄 8 局）；
  场风由局号推导（`round_index / 4`），**自风修正为「庄家自风 = 场风」**
  （南场庄家是南家，`(seat − dealer) % 4` 会整体差一圈）；日麻的连庄上限放宽到局数
  （classic 的 4 不变）。大厅在日麻下多一行「局数：东场 / 半庄」。
- **听牌提示不再谎报「能和」**（本轮修复）：`tenpai` 帧追加 `waits[].yaku` 与 `no_yaku`，
  由 `riichi::WinningWaits` 逐张按**荣和**口径判役。**`is_tenpai` 保持形式听牌语义**
  （流局罚符、立直资格都看它），只是提示分成了「形听」与「能和」两件事。
- **客户端**：
  - 大厅新增玩法选择（经典麻将 / 日本麻将），classic 仍发不带 `mode` 的 `create_room`（帧字节不变）；
  - 操作栏新增「立直」按钮（需先选中宣言牌，与打牌同一条 action）；
  - 牌桌 HUD 显示持点与宝牌指示牌；结算行改为「合计 N 番 M 符（X 点）」，并渲染满贯分档；
  - 立直可见：`action_event{action:8}` + `state_sync.seats[].riichi`，名牌右侧挂红色「立直」标记；
  - 结算播报（阶段 A）六段在日麻下改为「番种 → 合计番 → 符 → 满贯分档 → 点数 → 收付（点）」：
    字幕念「N 番 M 符，X 点」，S3 显示符、S4 显示满贯分档、S5 显示点数，与阶段 B 明细行/结论句
    同口径（单位「点」），不再出现「倍 / 庄家加一番 / 自摸不加番 / 封顶 6 番」；语音只复用
    役名与番值都一致的 classic 整句（其余只进字幕）。判据是 `win.fu` **键存在**（役满 `fu=0`
    也算日麻），流局用 `tenpai[4]` 判玩法并改报听牌罚符与庄家连庄/轮庄。**classic 文案逐字不变**；
  - **赤宝牌（v1.18）**：5 的第 4 张物理牌 = 赤牌，房间只在 `mode=riichi` 时把它序列化成 `0m`/`0p`/`0s`
  （classic 一律 `5m`，帧字节不变）。算分侧 `ScoreResult.aka` / `win.aka_dora` 每张 +1 番，
  合并成 `yaku[]` 的一条 `aka_dora`；**赤牌不是役**（无役照样和不了），**役满不加算**。
  客户端 `tiles.py` 认 `0X`（`is_red_five` / `tile_face` 归一到 5，排序排在同面最后），
  牌名显示「赤 5 万」。**外观不是另画一套**：`faces/0m|0p|0s.png` 由 `5m|5p|5s.png`
  按「墨浓度」把油墨换成红色派生（`game/tools/make_aka_tiles.py`，`--check` 防漂移），
  与其它 33 种牌同源同光照、纸纹与阴影都不变；只有贴图缺失时才回退到程序化染红。
  打牌提交走**物理牌精确匹配**：手里同时有 `5m` 与 `0m` 时打 `5m` 不会误打赤牌；鸣牌优先消耗普通 5。
- **双倍役满（v1.18）**：`kokushi_13`（国士十三面）/ `suuankou_tanki`（四暗刻单骑）/
  `chuuren_junsei`（纯正九莲）/ `daisuushii`（大四喜）记 2 倍，`yaku[].han = 26`，
  多役满**倍数相加**（大四喜 + 字一色 = 3 倍 = 闲家 96000）；`win.limit` 仍是 `yakuman`，
  客户端按 id 查表即可，不需要自己判倍数。
- 立直后锁手牌：只能摸切、不能鸣牌（服务端强制，客户端照常提交由服务端裁决）。
  - 日麻专用番种中文表（立直/一发/门前清自摸/役牌/宝牌/役满…），classic 文案逐字不变。

### 7.4 仍未做

1. **立直后的杠** ✅（v1.16 已完成）：`kakan` 仍不给（结构上不可能），「不改变听牌的暗杠」已放开。
2. **庄家首摸仍是发牌直发第 53 张**：等价于「已经摸好」，因此 `remaining` 初值是 69 而不是 70；
   天和判定沿用了 classic 的「14 张起手」，未走「首摸」事件。
3. **AI 立直无条件**：门清听牌 + 点数够就宣言，不看打点与巡目；没有现物/筋防守。
4. **半庄与场风轮转** ✅（v1.14 已完成，见 §7.3）。
5. **赤宝牌 / 双倍役满** ✅（v1.18 已完成，见 §7.3）。
6. **`match_results` 有 mode 列** ✅（本轮已完成）：`matches` / `match_results` 都落 `mode`，
   `stats` 按 `classic` / `riichi` 分桶返回，`recent[].mode` 标注每局玩法；老库启动时自动 `ALTER TABLE` 补列。
7. **`PROTOCOL.md` 已升到 v1.20** ✅（v1.12 = §12 日麻整章；v1.13 = §12.8 听牌提示口径；
   v1.14 = §12.9 半庄 / 场风 / 自风；v1.15 = §12.10 抢杠；v1.16 = §12.4 立直后暗杠；
   v1.17 = §12.4 振听不可立直 + 天和/地和 + 国士修复；v1.18 = §2.1 赤 5 牌码 + §12.5 `aka_dora`
   + §12.11 赤宝牌与双倍役满；v1.19 = §12.12 击飞终局；v1.20 = §12.13 场终剩棒归一位）。
8. **击飞终局** ✅（v1.19 已完成，见 §7.6）。
9. **场终剩棒归一位** ✅（v1.20 已完成，见 §7.7）。

### 7.5 测试入口

```
cmake --build build --target riichi_rules_test riichi_room_test -j8
./build/game/riichi_rules_test     # 42 项：符/番/点数/宝牌/赤宝牌/双倍役满/振听/立直/听牌罚符/无役/听牌有役/场风自风/抢杠/立直后暗杠/击飞/供托归一位
./build/game/riichi_room_test      # 11 项：牌山切分 / 持点 / classic 帧不变 / 立直拒绝 / 局数 / 赤牌只发日麻 / win 帧自洽 / 半庄打到南场 / 击飞即终局 / 场终剩棒归一位
ctest --test-dir build -R riichi   # 加上端到端 riichi_e2e_test（真服务端打一局）
./.venv/bin/python game/tests/client_headless_test.py      # 客户端组件（含大厅截图）
./.venv/bin/python game/tests/client_riichi_e2e_test.py    # 真客户端 + 真服务端：建日麻房→开局→立直→摸切
```

### 7.6 击飞（v1.19）

按雀魂口径落地：**任一持点 < 0 ⇒ 本场立即终局**，恰好 0 点继续，负点不截断。

- 判定点：`RoomActor::AdvanceAfterRound`，即每局**收付完成后**（和了支付 / 荒庄听牌罚符之后）。
  局中持点只有立直供托一次变化，而立直要求 ≥1000 点，所以到不了负点 —— 「局中击飞」不存在。
- 优先级：**高于连庄与局数**。击飞那一局**不推进**庄家 / 局号 / 本场数（`if (!busted)` 包住整段
  轮庄判定），`game_end.match_will_continue = false`，紧接 `match_end`。
- classic 不参与：classic 没有持点概念（内部 `points_` 恒 0），不加 `mode` 判据会把每一场都截断在第 1 局。
- 协议：**零新增字段**。`game_end.reason` 仍是被击飞那一局的真实结束方式（`win_*` / `draw_exhausted`），
  击飞是场次层的终止条件，不新增 `reason` 值 —— 老客户端不会撞上未知枚举，`match_end` 提前到达即是信号。
- **「终局损」是规则选定，不是缺陷**：第 4 名给第 2 名点炮时可能第 2 名就此失去争一的机会；
  反过来说，若点炮的是第 2 名自己，他可以用「见逃」（代价是振听）拒绝提前终局。领先者则多了一个
  「锁名次」的手段。雀魂 / 天凤同口径，理由是给对局一个**硬性时间边界**（日麻连庄不封顶，
  没有击飞一场可以打很久）+ 让第 4 名**风险自负**（不能把「继续赌大牌」的成本转嫁给全桌）。
  若将来要做「无击飞 / 打满规定局数」或是「建房开关 `bust_end`」，改动点是同一个 `match_over`
  表达式，新增字段向后兼容。
- 测试：`riichi_rules_test` 的 `RiichiBust.OnlyNegativePointsBust`（0 点不算 / -1 算，纯函数边界），
  `riichi_room_test` 的 `BustEndsMatchImmediately`（`SetPointsForTest` 把一家打到 -100000，
  半庄也只下发 1 份 `game_end` 就 `match_end`，`round_index` 不推进，`points_after` 保留负值）。

### 7.7 场终剩棒归一位（v1.20）

立直宣言的 1000 点**当时**就从持点里扣走了（`delta` 记 −1000），棒挂在 `kyoutaku_` 上等下一次和牌。
只要有人和牌，棒就被和牌者收走；**但如果本场以荒庄（末局流局）或击飞收尾，没人来收** ——
修正前这 1000 点就凭空蒸发了：四家持点之和不再是 100000，`points_after` 与玩家看到的收付对不上。

- 口径（雀魂）：**台面上的棒全部归一位**。一位 = 持点最高者，**同点取 seat 号小的**（东一局座次 /
  起家优先），与 `match_end.rankings` 的「分数降序、同分 seat 升序」**同一套排序**，
  所以「场供归了一位」这件事永远不会改变名次（本来就归第一名）。
- 落点：`riichi::AwardLeftoverKyoutaku(points, sticks)`（纯函数，返回 `{seat, sticks, points}`）
  + `RoomActor::AdvanceAfterRound` 里 `match_over` 之后、`final_rank` 之前一次性归账。
- 归账要动**四处**，否则各帧会互相打架：`points_`（终局持点）、`match_totals_`（本场累计得失分）、
  本局 `settlement.delta` / `settlement.totals`、以及 `settlement.points_after` 的重算。
  于是最后一局的 `game_end.results[].delta_score`、`points_after[]` 与 `match_end.rankings[].final_score`
  三者逐点一致，客户端不需要自己补账。
- 协议（v1.20）：新增**可选字段** `game_end.kyoutaku_award`（`{seat, sticks, points}`），
  非场终局 / 无棒恒 `null`；`win.kyoutaku`（本局收走的棒数）语义不变，两者**互斥**。
- 客户端：结算页在「流局 · 听牌 N 家」下面多一行「场供 2 根 → 太郎 +2000」（`result._kyoutaku_award_text`），
  只在有归属时出现；classic 与普通局**不多画任何东西**。
- 与「点数守恒」的关系：供托在宣言时离开玩家持点、在场终回到玩家持点，中间挂在房间上；
  现在「四家持点之和 = 100000」在整个对局里**恒成立**（除了宣言到结算之间那一段，棒在台面上）。
- 测试：`RiichiKyoutakuAward.LeftoverSticksGoToFirstPlace`（无棒 / 正常 / 同点取起家 / 有负点，
  纯函数边界）、`LeftoverKyoutakuGoesToFirstPlace`（种子 20260101 的东场末局荒庄，`SetKyoutakuForTest`
  在第 4 局塞 2 根 ⇒ `kyoutaku_award = {seat, 2, 2000}`，四家持点之和 = 100000 + 2000（注入不扣点），
  收棒的是持点最高那家，`match_end` 的一位与 `points_after` 一致，房间供托清零）。
  客户端侧 `client_headless_test` 增两条断言 + 产物 `artifacts/client_result_kyoutaku_award.png`。
