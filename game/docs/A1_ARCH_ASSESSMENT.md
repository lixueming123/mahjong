# A1 架构影响评估 —— A 批（番型表 + 番数结算 + 多局场次/连庄 + 暗杠加杠）

- 作者：柯桥良（技术架构总监）
- 批次：P1（与创意总监的设计案并行）
- 状态：**评估结论**，不含玩法规则、不含代码
- 评估对象：A 批四项改动对现有服务端 / 客户端 / 协议 / 存储 / 测试的架构影响
- 本轮**不涉及引擎选型**：技术栈已定（C++ 服务端 + ex_actor actor 框架；Python + pygame 客户端），故无引擎加权对比表

---

## 0. 阅读清单与前提假设

### 0.1 实际阅读过的文件（结论只指向这些）

| 类别 | 文件 | 规模 |
| --- | --- | --- |
| 服务端规则 | `game/server/mahjong_rules.{h,cc}` | 235 / 856 |
| 服务端房间 | `game/server/room.{h,cc}` | 403 / 1369 |
| 服务端大厅 | `game/server/lobby.{h,cc}` | 124 / 526 |
| 服务端协议 | `game/server/protocol.{h,cc}`、`player_agent.h`、`player_actor.cc` | 180 / 60 / 96+ / — |
| 服务端存储 | `game/server/db.h`、`db_mysql.cc`、`db_stub.cc` | 224 / 462 / 65 |
| 框架约束 | `game/server/async_gate.h`、`timer.h` | 149 / 55 |
| 客户端 | `game/client/game_state.py`、`app.py`、`render/actionbar.py`、`render/result.py` | 462 / 1436 / 243 / 271 |
| 文档 | `PROTOCOL.md`(906)、`RULESET.md`(552)、`GAME_DESIGN.md`(922)、`ACCOUNT_DESIGN.md`(229) | — |
| 测试 | `mahjong_rules_test.cc`(1079)、`mahjong_room_test.cc`(1581)、`lobby_test.cc`(363)、`game/tests/*.py`(4584) | — |

### 0.2 前提假设（假设变了结论要重估）

| # | 假设 | 依据 | 若假设不成立的影响 |
| --- | --- | --- | --- |
| A1 | 客户端与服务端**同仓库同步发版**；"向后兼容"只需保证"旧客户端不崩、能退化运行"，不承诺旧客户端能完整玩满一场 | `GAME_DESIGN.md:74-76`（单机可运行样例定位）、`game/start_client.sh` 与服务端同仓 | 若将来要跨版本共存，须引入线上升级协商（当前帧内**无任何 version 字段**，见 §2.4） |
| A2 | 场次固定 4 局 + 连庄不占局数 ⇒ **理论上无上界**，因此架构上必须引入硬上限（具体数值待裁决） | 用户 P0.5 原文；`RULESET.md:376-377` 现有"一局必然终止"证明只覆盖单局 | 无上限则房间存活时间与资源回收失去上界（见 §8.4 单点风险 S1） |
| A3 | 番种表、番数、庄家轮转/连庄判定、杠后是否设抢杠窗口，**内容由创意总监定**；本评估只给"承载结构"与"影响面" | 本任务书纪律 | 番种数量/是否含役满会影响 `ScoreResult` 是否需要 `limit`(封顶) 分支 |
| A4 | 平台合规 / 上架 / 打包管线不在本轮 | 任务书 | — |
| A5 | 不改 ex_actor 框架本身（不改 mailbox / 调度 / Send 语义） | `PROTOCOL.md:48-55`、`async_gate.h:1-30` | — |
| A6 | 本里程碑仍"单房间、1 真人 + 3 AI"，不做多真人同房与观战 | `GAME_DESIGN.md:16`、`PROTOCOL.md:44-46` | — |

### 0.3 对任务书两处事实的订正（以代码为准）

| 任务书表述 | 实际情况 | 依据 |
| --- | --- | --- |
| "现有 11 条错误码" | 当前是 **14 条**（v1.3 由 11 增补 `AUTH_FAILED`/`USERNAME_TAKEN`/`ALREADY_ONLINE`） | `protocol.h:75-77` 注释与 `:78-91` 定义；`PROTOCOL.md:671`、`:905` |
| "`STALE_SEQ` 不作为错误码" | 正确，且本轮也不新增错误码 | `PROTOCOL.md:690` |

另：`protocol.h:67-69` 的注释仍在描述 `player_actor.cc` 里的 `kFrameRoutes`，该表已迁到 `player_agent.h:73-91`（编译期完整性断言在 `player_actor.cc:143-144`）。属文档漂移，顺手修即可，**不在本轮必需项**。

---

## 1. 结论总表

| # | 议题 | 结论 | 关键依据 | 风险 |
| --- | --- | --- | --- | --- |
| 1 | 协议变更面 | **可纯增补**（不改现有帧必填字段、不新增错误码），但**必须文档先行**：先修订 `PROTOCOL.md` v1.6 + `RULESET.md` v1.3，否则实现即违反 `protocol.h:3-5` 的"不得私自发明"契约。新增 1 个 S→C 帧 `match_end`；`action` 动作词表 +2（`ankan`/`kakan`）；**不新增窗口类型** | `protocol.h:3-5`、`:70-73`、`:145-147`；`PROTOCOL.md:90-100`；`player_agent.h:73-91` | **中** |
| 2 | 房间生命周期重构 | **必须改，且是本批最大工作量**。现有 `room.cc` 全篇假设"一局即终局"：`Phase::kFinished` 是终态、`StartGame` 只跑一次、`NotifyFinished` 一局一次、lobby 收到通知就 arm 30s 回收。共 **15 个改造点**（§3） | `room.h:257-263`、`room.cc:577-607`、`:1002-1073`、`lobby.cc:210-229`、`lobby.cc:469-500` | **高** |
| 3 | ex_actor 原子性与 seq | **`seq` 作废方案够用，不需扩展机制**；但 seq 必须**跨局单调、不重置**（现有 `StartGame` 里的 `seq_ = 0` 必须删），否则违反 `PROTOCOL.md:632` 且直接撞 `mahjong_room_test.cc:1193-1201` 的严格递增与 `game_end` 唯一性断言。暗杠/加杠**不新增窗口类型**，作为 `discard` 窗口的附加 `options`；补牌**不破坏** 83/84/21 三个派生数（每局重建牌山 + 补牌同样 `remaining--`） | `room.cc:570-575`、`:590`、`:675`、`:966-984`；`PROTOCOL.md:620-636`、`:651-659`；`RULESET.md:92-101`、`:370-378` | **中**（机制低，落地有 3 个易错点） |
| 4 | 算分改造落点 | `ScoreResult` 扩字段（保留 `multiplier`/`per_payer`/`total` 兼容）、`ScoreWin` 加 `ScoreContext` 入参、`WinTags` 合并进 `DetectYaku`（**消除"两处判定同一件事"的漂移风险**）。纯函数、整数、确定性可保 | `mahjong_rules.h:215-227`；`mahjong_rules.cc:799-832`、`:401-409`；`RULESET.md:331-355`、`:383-390` | **中** |
| 5 | 战绩存储 | 现有 `matches` 表够存"每局"，不够存"场次总分/最高番"。`ACCOUNT_DESIGN.md` **无 SCHEMA_VERSION / 无迁移框架**（只有 `CREATE TABLE IF NOT EXISTS`）⇒ 推荐**加表不改表**（新增 `match_summaries`），零迁移、零回归 | `db_mysql.cc:219-243`；`ACCOUNT_DESIGN.md:31-63`；`db.h:84-98`、`:130-131` | **低** |
| 6 | 确定性与可复现 | 可行，且**已有可直接复用的派生范式**：`DeriveRoomSeed`（双射 Mix）→ 新增 `DeriveRoundSeed(match_seed, round_index)`，round 0 取恒等以保全既有 e2e 可复现断言。但确定性契约要写明边界：**只在 `--ai-delay-ms 0` + 脚本化客户端下成立**（`deadline_ms` 取墙钟，帧字节本身不可复现） | `room.h:71-92`；`lobby.cc:132-137`；`room.cc:705`；`mahjong_rules.h:89` | **低** |
| 7 | 实现顺序与工作量 | 6 批，B0 文档先行 → B1 规则库 ∥ B4a 客户端协议层 → B2 房间场次 → B3 大厅/存储 → B4b 客户端界面 → B5 e2e。**单点风险 = B2 房间生命周期重构** | §8 | — |

**一句话结论**：本批改动**没有一项会撞穿 ex_actor 的硬约束**（不需要取消/撤回/抢占，不需要新的原子性机制），真正的成本集中在**房间状态机从"单局"变成"场次"**；协议与存储都可以用纯增补方式过关；番数与暗杠的真正风险在**测试面**（69 个 C++ 单测〔规则 37 / 房间 26 / 大厅 6〕+ 5 个 Python 集成测试里，硬编码了 `M ∈ {1,4,16}`、"每局恰好一个 `game_end`"与"一局一次回调"），不在运行时机制。

---

## 2. 协议变更面

### 2.1 结论

**可以向后兼容地增补，不需要线上版本协商**（帧里根本没有 version 字段）。但三件事必须同时做到，否则"旧客户端不崩"不成立：

1. **不新增 `window` 取值**（`action_request.window`）。旧客户端 `game_state.py:305` 的白名单是 `(discard, claim)`，其他值直接丢帧 → 该窗口永远无人应答，每窗口干等 15s 超时（不崩，但完全不可玩）。
2. **暗杠/加杠不得复用 `action:4`**。旧客户端 `game_state.py:354-363` 对 `pong/chow/kong` 会**去 `from_seat` 的牌河删最后一张**；如果暗杠复用 `kong` 且不带 `from_seat`，旧客户端会打一条 WARN 然后什么都不做（尚可），一旦误带 `from_seat` 就会删错牌河。用新动作名 `ankan`/`kakan` 则旧客户端既不渲染按钮也不动牌河，只 `clear_pending()`，**退化安全**。
3. **副露类型继续用 `"kong"`**。旧客户端 `game_state.py:36`/`:55` 的 `MELD_TYPES` 只认 `pong/chow/kong`，未知类型会**整组丢弃** → 手牌张数自检 `len(hand)+3*melds ∈ {13,14}` 全部报 WARN。暗杠/加杠的"暗"用**附加字段**（如 `"concealed": true`）表达，旧客户端读不到就忽略（`PROTOCOL.md:97` 明令未知字段必须忽略）。

### 2.2 受影响帧逐条

| 帧 | 方向 | 变更 | 兼容性 | 依据 |
| --- | --- | --- | --- | --- |
| `game_start` | S→C | 增 `round_index`(0..3)、`round_count`(=4)、`totals[4]`（累计分）；`dealer` **值域变化**（不再恒 0，`PROTOCOL.md:436` 现写"本里程碑恒为 0"） | ✅ 纯增字段；旧客户端只读 `hand/seat/dealer`，且 `_apply_game_start` 会整体重置本地状态（`game_state.py:221-258`）→ **旧客户端收到第二局的 `game_start` 会自然切回牌桌**（这是本方案最省事的兼容点） | `room.cc:501-503`、`:593-599` |
| `state_sync` | S→C | **建议不加任何字段** | ✅ | 热路径：每次摸牌广播 4 份（`room.cc:447-466`、`PROTOCOL.md:568-577`）。场次进度用低频帧承载，避免每帧变大 |
| `action_request` | S→C | `window` **不加新值**；`options` 增加 `{"action":5,"tiles":["5m"]}` / `{"action":6,"tiles":["5m"]}`（`tiles` 用单张面码，与 `kong` 同口径） | ⚠️ 旧客户端：按钮不渲染、超时按默认打牌 → **可继续玩，只是不能杠**（可接受退化） | `room.cc:81-90`、`:701-705`；`mahjong_rules.cc:429-436`；`actionbar.py:68-84` |
| `action_event` | S→C | `action` 值域 `{pong,chow,kong,win}` → `+{ankan,kakan}`；**两者 MUST NOT 携带 `from_seat`**（同自摸口径） | ⚠️ 旧客户端：走 `clear_pending()` 分支，不动牌河；随后 `state_sync` 全量覆盖 `hand`/`melds` 纠偏（`room.cc:985` 保证副露后必发 sync） | `room.cc:92-104`、`:954-962`；`PROTOCOL.md:489-501`；`game_state.py:346-369` |
| `game_end` | S→C | **语义保持"局终"不变**（`reason` 词表不动：`win_ron`/`win_tsumo`/`draw_exhausted`）；增 `round_index`、`match_over`(bool)；`win` 内增 `han`(int) 与 `yaku[]`（`{id,han}` 数组），`win.tags` **保留但改填 yaku id 列表** | ✅ 纯增字段；旧客户端 `result.py:29` 对未知 tag 回落到 `str(t)`，只影响文案 | `room.cc:505-553`；`result.py:21-29`；`PROTOCOL.md:541-556` |
| **`match_end`（新）** | S→C | 新帧：`type`、`rounds_played`、`renchan`（连庄次数）、`totals[4]`、`ranking[4]`、`best_win{seat,han,yaku[]}` | ✅ 旧客户端忽略未知帧（`PROTOCOL.md:98`、`game_state.py:213-216`）→ 停在最后一局结算页，正是想要的退化 | 设计参考：`GAME_DESIGN.md:662` 已预留"协议新增 `match_state`" |
| `action`（C→S） | C→S | `action` 值域 +2（`ankan`/`kakan`），`tiles` 语义同 `kong`（`[T]`） | ✅ **不新增客户端帧类型**，`protocol.h:70-73` 与 `player_agent.h:73-91` 的帧表**不用动** | `protocol.h:145-147` |

### 2.3 错误码

**零新增、零改语义**：`PROTOCOL.md:671` 的 14 条保持不变。

| 场景 | 复用错误码 | 判定点 |
| --- | --- | --- |
| 暗杠/加杠不在本窗口 `options` 内 | `ILLEGAL_ACTION` | 沿用 `room.cc:1311-1315` 的"按窗口提供的 options 判定"口径 |
| 窗口不是打牌窗口 / 不轮到该座位 | `NO_PENDING_ACTION` / `NOT_YOUR_TURN` | `room.cc:1224-1227`、`:1303-1306` |
| `tiles` 形状不对 | `BAD_REQUEST` | `room.cc:1245-1261` |
| 杠张不在手里 | `UNKNOWN_TILE` | `room.cc:1233-1236` |

> `STALE_SEQ` 仍**不**作为错误码（`PROTOCOL.md:690`）：新窗口的迟到回复继续静默丢弃。

### 2.4 为什么"不需要 bump 协议版本"

线协议里**没有版本字段**：`PROTOCOL.md` 附录 A（`:862-884`）的全部帧字段表中无 `version`；`protocol.h` 也无版本常量。因此"协议版本"只存在于**文档修订记录**（`PROTOCOL.md:897-906`）。兼容性完全由两条冻结规则承担：

- `PROTOCOL.md:97`：未知**字段** → 服务端 MUST 忽略；
- `PROTOCOL.md:98`：未知**服务端帧类型** → 客户端 SHOULD 忽略并记日志（`game_state.py:213-216` 已实现）。

⇒ 本批的正确做法是**加修订行 + 加字段/加帧**（照 v1.3 / v1.5 的先例），而不是造一个线上升级机制。

### 2.5 但有两处"必须走文档评审"的硬约束

1. `protocol.h:3-5`：「Every value in this header is copied from the frozen spec… Implementations MUST NOT invent message types, fields or error codes (§0.1)」+ `PROTOCOL.md:20` 同义。
   ⇒ **B0（文档修订）必须先行**，否则 B1/B2 的实现一开始就违约。
2. **H1 需修订**：`PROTOCOL.md:26` 冻结「打牌窗口的合法回复只有 `discard{tile,seq}` 或 `action{win,seq}`；`action{pass}` 属 `ILLEGAL_ACTION`」。暗杠/加杠使打牌窗口出现第三种合法回复，H1 必须明确扩写为「`discard` / `action{win}` / `action{ankan}` / `action{kakan}`」，并同步 `room.cc:1264-1301` 的判定矩阵与 `PROTOCOL.md:638-647` 的判定表。

---

## 3. 房间生命周期重构 —— 改造点清单

> 现状一句话：`RoomActor` 是"一局的状态机"，`LobbyActor` 是"房间的注册表 + 回收器"；`GAME_DESIGN.md:836-837` 与 `PROTOCOL.md:44-46` 都明写"一房间一局、结算后销毁"。改成 4 局场次 = **在 RoomActor 内部加一层"场次"，而不是新增 MatchActor**（理由见 §3.2）。

### 3.1 改造点（C1–C15）

| # | 改造点 | 位置（文件:行号） | 具体动作 | 风险 |
| --- | --- | --- | --- | --- |
| **C1** | 场次状态注入 | `room.h:356-400`（状态区） | 新增 `round_index_`、`round_count_`、`renchan_count_`、`match_totals_[4]`、`match_over_`。**累计分只由 RoomActor 持有**——lobby 声明"从不持有对局状态"（`lobby.h:5`），必须守住 | 中 |
| **C2** | 拆 `StartGame` | `room.cc:577-607` | 拆为 `StartMatch()`（一次：校验座位、`match_totals_` 清零、`round_index_ = 0`）与 `StartRound()`（今天的全部内容：`BuildWall`/`DealTiles`/发 `game_start`/开庄家打牌窗口） | 中 |
| **C3** | **`StartRound` 必须补重置** | `room.cc:577-607` vs `room.h:361-363` | 现有 `StartGame` 只重置 `hands_`/`wall_`/`next_index_`/`remaining_`/`dealer_`/`current_seat_`/`seq_`，**没有重置 `melds_` 与 `discards_`**。单局时看不出来（只跑一次），多局时第 2 局会继承上一局的副露与牌河 → 牌数守恒断言会立刻炸。这是**当前就存在的多局 bug**，改造必修 | **高** |
| **C4** | 阶段枚举扩展 | `room.h:257-263`、`room.cc:609-669`（`Pump` 的 switch） | 现 `kIdle/kTurnStart/kDiscardWindow/kClaimWindow/kFinished`。改为：`kFinished` 语义收窄为**局终**，新增 `kRoundEnd`（局间等待推进）与 `kMatchEnd`（场终）；`Pump` 的 switch 必须处理新分支，且**默认落在"等消息"而非继续循环** | **高** |
| **C5** | 局间推进**不可**在同一次 `Pump` 里循环 | `room.cc:610`（`kMaxPumpSteps = 20000`，`room.cc:166`） | 一次消息处理内只允许推进 1 局；进入 `kRoundEnd` 后必须 `co_return`，由一条定时消息（C6）触发下一局。否则一次 `Pump` 打完整场，既撞步数上限又让 actor 长时间不让出调度 | 中 |
| **C6** | 局间推进令牌 | `room.h:104-107`、`room.cc:1089-1152` | 复用 `timeout_scheduler_`，但**必须用独立 token 位**：现有 `kAiPacingTokenBit = 1ull<<63`（`room.h:104`）。局间"没有开窗"⇒ `seq_` 不变 ⇒ 旧窗口的迟到 timeout 与局间推进令牌**无法靠 seq 区分**，必须新增 `kRoundAdvanceTokenBit = 1ull<<62`（或把 `round_index_` 编进 token 低 8 位）。**这是本批最隐蔽的易错点** | **高** |
| **C7** | `game_end` 语义拆：局终 vs 场终 | `room.cc:1002-1073`（`EndWithWin`/`EndWithDraw`）、`room.h:271-285`（`Settlement`） | `EndWithWin/EndWithDraw` 改名为 `EndRoundWithWin/EndRoundWithDraw`：算分 → 累加 `match_totals_` → 广播局终 `game_end` → 判定是否 `FinishMatch`（局数满且无连庄）→ 是则广播 `match_end` 并进 `kMatchEnd`，否则进 `kRoundEnd` 并 arm C6 令牌 | **高** |
| **C8** | `Settlement` 结构扩展 | `room.h:272-285` | 加 `round_index`、`match_over`、`totals[4]`、`dealer_next`、`yaku[]`/`han`。**这是内部结构，不是线格式**，所以可以自由扩 | 低 |
| **C9** | 庄家推进（含发牌口径） | `room.cc:588`（`dealer_ = 0`）、`mahjong_rules.cc:234-235`、`RULESET.md:85-90` | ①`dealer_` 由纯函数 `NextDealer(dealer, round_result)` 推进（规则库，可单测）；②**`DealTiles` 硬编码 `hands[0]` 为庄家 14 张**（`mahjong_rules.cc:234-235`，注释与 `RULESET.md:87` 都写"庄家=seat 0"）⇒ 必须改为 `DealTiles(wall, dealer)` 并同步修订 `RULESET.md:85-90`。**漏了这条会出现"庄家牌给了 0 号位、庄家标记却是别人"的静默错位** | **高** |
| **C10** | 回调语义拆：每局战绩 vs 场终回收 | `room.cc:271-297`（`NotifyFinished`）、`lobby.cc:210-229` | 现状：一局一次回调 → ①逐座位 `RecordMatch` ②arm 30s `ReapRoom`。改为：**每局都回调**（继续逐座位记战绩），但 `GameSettlement` 增 `match_over` 字段，lobby 只在 `match_over == true` 时 arm `ReapRoom`。**提前 arm = 中途毁房；漏 arm = 房间永久泄漏** | **高** |
| **C11** | 托管在局间 | `room.h:119`、`room.cc:1177-1197` | 局间没有窗口 ⇒ 无 `action_request`、无 `OpenWindowTimer`（`room.cc:1079-1087`）。`disconnected` 标记**跨局保留**（它表达的是"这个座位由服务端代打"，与局无关）。`OnPlayerDisconnected` 在 `kRoundEnd` 只置标记 + `Pump()`（新 `Pump` 在 `kRoundEnd` 直接 `co_return`） | 低 |
| **C12** | 断线重连在局间 | `lobby.cc:410-434`、`GAME_DESIGN.md:644` | **结论：不支持，沿用现有口径**。断线时 lobby 已 `players_.erase`（`lobby.cc:420`），重连是**新 `player_id`**（游客）或撞 `ALREADY_ONLINE`（账号），没有 `player_id → room` 的持久映射，无法回到原房间。跨局托管继续跑是够用的。真要支持 = 独立里程碑（需房间快照持久化，`PROTOCOL.md:44-46` 明确不做） | 低（明确不做即为零） |
| **C13** | 内省 API 语义 | `room.h:220-222` | `game_finished()` 现在等价于 `phase_ == kFinished`；测试夹具用它判停（`mahjong_room_test.cc:1152`）。需拆成 `round_finished()` 与新增 `match_finished()`；`HandleGameFrame` 的早退条件（`room.cc:1201`）改为"场终才早退"，局间仍要能把迟到帧静默丢掉 | 中 |
| **C14** | 局间可否换人/加入 | `room.cc:251-269`（`TakeSeat`）、`lobby.cc:292-297`（`JoinRoom` 拒已开局） | **结论：不开放**。`game_started_` 一旦为真 `TakeSeat` 即拒（`room.cc:257`），lobby 也已把 `room_started_` 置真（`lobby.cc:377`）。局间换人会给"累计分归属"引入额外状态，收益远小于成本 | 低 |
| **C15** | 场次硬上限 | 新增（`room.h` 常量 + `C7` 判定里） | `max_rounds = 4` + `max_renchan`（建议 4，待裁决）双闸；两者任一触发即 `FinishMatch`。**没有这条，A2 假设下房间存活时间与 `discards_` 内存增长失去上界**（`room.h:363`）。同时保留 `kMaxPumpSteps` 作为兜底 | 中 |

### 3.2 为什么是"RoomActor 内加场次"，不是"新增 MatchActor"

`GAME_DESIGN.md:662` 的建议是「新增 `MatchActor` 包裹多个 `RoomActor`」。我评估后**不推荐**，理由：

| 维度 | 方案 A：RoomActor 内加场次（推荐） | 方案 B：新建 MatchActor 包 RoomActor |
| --- | --- | --- |
| 座位/连接所有权 | 不变（`RoomSeat.endpoint` 已在 room 内，`room.h:115-123`） | 需在两 actor 间同步座位表，或让 MatchActor 持有端点 → **连接所有权分裂成两处** |
| `seq` 单调性 | 天然（同一 actor 内单调） | 每局一个新 RoomActor ⇒ `seq` 要么重启（违反 `PROTOCOL.md:632`），要么由 MatchActor 注入起始值（引入跨 actor 的 seq 协商） |
| 超时/托管 | 沿用现有 `timeout_scheduler_` 与 `seq` 作废 | 回收 RoomActor 时其 mailbox 中未处理的 timeout 消息指向已销毁 actor ⇒ 需新的生命周期协议 |
| 改造面 | 改 1 个 1369 行文件的内部结构 | 新建 actor + 改 lobby + 两套 gate + 跨 actor 结算协议 |
| 结论 | ✅ **一人/小团队的可维护性最优** | ❌ 过度工程，且把 ex_actor 的"Send 不可取消"风险放大 |

> 前提：场次逻辑真的只是"同一批座位、同一批连接、连续的 4 局"。若将来要"多人多房匹配 + 换座 + 观战"，MatchActor 才值得。

---

## 4. ex_actor 原子性、`seq` 与派生不变量

### 4.1 原子性：不受影响

- 全部入口只有一个：`RoomActor::Dispatch`（`room.h:197-201`），第一句 `co_await gate_.Acquire()`（`room.cc:222`），所有 handler 都是 private（`async_gate.h:26-30` 明确要求"entry 是唯一入口"）。
- 新增阶段（`kRoundEnd`/`kMatchEnd`）与新增消息（C6 的推进令牌）都走同一个 `Dispatch` + `std::visit`（`room.cc:221-245`），`RoomEnvelope` 是 `std::variant`（`room.h:186-188`）⇒ **加一个 alternative 会让 `overloaded` visitor 编译失败**，这是好的失败（强制补齐）。
- 结论：**不需要任何新的锁、不需要扩展 `AsyncGate`**。

### 4.2 `seq` 作废方案：够用，但有两个硬条件

| 条件 | 理由 | 依据 |
| --- | --- | --- |
| **seq 跨局单调、不重置** | `PROTOCOL.md:632` 冻结"初始 1、每个窗口过渡自增一次、单调递增、永不回退"；且 `mahjong_room_test.cc:1196-1201` 断言同一局内 `action_request.seq` 严格递增（若夹具改跑整场，就成了跨局断言）。现有 `room.cc:590` 的 `seq_ = 0` **必须删** | `room.cc:590`、`:570-575` |
| **窗口类型枚举不动** | `seq` 的语义是"窗口标识"，与新窗口类型无关。只要"一次窗口过渡 = 一次 `Transition()`"，机制就不变。新增窗口类型反而要同步改：`PROTOCOL.md:620-628` 表、`game_state.py:305` 白名单、`room.cc:1089-1152` 三处 phase 分支 | `room.cc:570-575` |

> 若采用"暗杠/加杠 = 打牌窗口的附加 options"（本评估推荐），`seq` 语义**完全不变**：玩家暗杠后仍然是最多一个未决窗口，`Transition()` 照旧推进一次（`room.cc:717`、`:951`）。

### 4.3 "每次打牌对应一次摸牌"——这个不变量在代码里**不存在**，别按它设计

- 真实不变量是**牌数守恒**：`remaining_ = kTileCount - next_index_`（`room.cc:675`、`:976`），即 `remaining` 是 `next_index_` 的纯函数；杠补牌同样取 `wall_[next_index_]` 并 `remaining--`（`room.cc:966-984`）。
- 明杠已经打破了"摸打配对"（杠补牌是一次额外摸牌 + 一次额外打牌），只是**净张数不变**，所以 84/21 两个派生数仍然成立。
- **暗杠/加杠不需要新机制**：与明杠同路径补牌即可。但必须补一条前置校验：`remaining >= 1` 才提供 `ankan`/`kakan` 选项——现有 `CanKong(hand, discarded, remaining)` 已带该参数（`mahjong_rules.h:116`、`RULESET.md:98-100`），新函数要照抄这个口径，否则会出现"杠了但没有牌可补"的死角（明杠的这条在 `room.cc:968-972` 已经处理成"直接荒庄"，暗杠要一致）。

### 4.4 84 / 21 / 83 是否要改

| 数值 | 结论 | 依据 |
| --- | --- | --- |
| `kWallSizeAfterDeal = 83` | **不变**（每局重建牌山 + 重新发牌） | `mahjong_rules.h:32`、`RULESET.md:85-90` |
| 84 次打牌（83 摸 + 庄家第一打） | **不变**：补牌也消耗 `remaining`，总摸牌次数上界仍是 83 | `RULESET.md:92-101`、`:370-378`；`PROTOCOL.md:64` |
| 单座牌河上限 ⌈84/4⌉ = 21 | **不变**（总打牌次数上界不变） | 同上 |
| 这些数的**适用范围** | **要改文档措辞**：从"一局"→"每局"；场次总量 = 4 局（+连庄）⇒ 单房间牌河条目总数上界 4×84 | `PROTOCOL.md:64`、`RULESET.md:376-377` |

> ⚠️ 唯一会推翻上表的是**"杠补牌改从王牌/岭上牌取"**（真实日麻做法）。本里程碑 `RULESET.md:95` 冻结"从**同一牌山**取下一张"，若创意总监要改，则 83/84/21 三个数全部要重算。列为待确认（§9 Q3）。

### 4.5 客户端侧的原子性无关项（但要一起改）

客户端 `_note_seq` 只认"严格更大"的 seq（`game_state.py:431-445`），且 `game_start` 会重置 `last_seq = None`（`game_state.py:238`）⇒ 只要 seq 跨局单调，客户端不会误关窗。

---

## 5. 算分改造落点

### 5.1 现状

```cpp
struct ScoreResult {                       // mahjong_rules.h:215-220
  std::array<int,4> delta{}; int multiplier = 0; int per_payer = 0; int total = 0;
};
int  MultiplierForTags(const std::vector<std::string>&);                    // cc:799-805
ScoreResult ScoreWin(int winner_seat, int from_seat,
                     const std::vector<Tile>& concealed_with_winning_tile,
                     const std::vector<Meld>& melds);                        // h:224-226 / cc:807-832
std::vector<std::string> WinTags(concealed, melds);                          // h:139-140 / cc:401-409
```

已知结构性缺陷（本批必须顺手修）：**同一件事判了两遍**——`WinTags` 判一次标签（`cc:401-409`），`MultiplierForTags` 再按标签字符串查一次倍率（`cc:799-805`）。番种表一旦扩到十几个，两处必然漂移。

### 5.2 建议的目标形状（架构，不含番种内容）

```cpp
struct Yaku { protocol::YakuId id; int han; };       // v2.1：id 为线枚举，与 win_type/tags 同一词汇表
struct ScoreContext {                                 // 显式入参，杜绝全局状态（RULESET §8）
  bool tsumo = false;
  int  winner_seat = 0;
  int  dealer_seat = 0;      // 庄家加成/庄家支付口径需要
  // 预留：round_wind / seat_wind，本轮不用（无场风/自风玩法）
};
struct ScoreResult {
  std::array<int,4> delta{};
  std::vector<Yaku> yaku{};    // 顺序 = 冻结的判定顺序（确定性）
  int han = 0;                 // Σ yaku.han
  int base_points = 0;         // 由 han 与番数表决定的支付基数
  // ↓ 兼容字段：保留名字，改填充值（旧客户端、e2e 断言、DB 都不必立刻改形状）
  int multiplier = 0; int per_payer = 0; int total = 0;
};
std::vector<Yaku> DetectYaku(concealed, melds, const ScoreContext&);   // 取代 WinTags
ScoreResult ScoreWin(int winner_seat, int from_seat, concealed, melds, const ScoreContext&);
```

### 5.3 必须守住的四条约束

| 约束 | 具体要求 | 依据 |
| --- | --- | --- |
| **纯函数** | 不读时钟、不用随机、无全局可变状态；`ScoreContext` 只按值传 | `mahjong_rules.h:7`、`RULESET.md:383-390` |
| **确定性** | 番种判定顺序 = 一张固定表（`constexpr` 数组）的顺序；同分并列时取表内靠前者；**全整数，禁止浮点** | `RULESET.md:351`（"全部为整数运算，无舍入"） |
| **零和** | `Σ delta == 0` 由构造保证，并在函数内 `assert`；现有实现靠"两家加减"隐式成立，扩到番数后极易破 | `RULESET.md:343-351`；断言已存在：`mahjong_room_test.cc:1439`、`room_full_game_test.py:346` |
| **可单测** | 判定与支付分开：`DetectYaku` 可单独断言"这手牌判出哪些番、多少番"；`ScoreWin` 只做支付算术 | `RULESET.md:439-494`（单测规范） |

### 5.4 签名推进的两种走法（工作量差一倍）

| 走法 | 做法 | 影响 | 建议 |
| --- | --- | --- | --- |
| **A：保留旧签名 + 新重载** | 旧 `ScoreWin(winner, from, concealed, melds)` 转发到新函数并填 `ScoreContext{tsumo: from<0}` | 现有 3 个算分单测（`mahjong_rules_test.cc:288`、`:318`、`:570`）**可以不动** | 若番数结算与旧 `M∈{1,4,16}` 数值相容（例如把"平和=1番"映射回 M=1）则选 A，回归面最小 |
| **B：直接改签名** | 删旧重载 | 3 个单测 + 房间测试的 `multiplier` 断言 + `room_full_game_test.py:361-363` 全要改 | 若番数表与旧 `M` 语义不相容（几乎必然），则只能选 B |

### 5.5 测试影响清单（这是本批第二大的实际工作量）

| 测试 | 位置 | 需要做什么 |
| --- | --- | --- |
| `ScoreRonAndTsumo` | `mahjong_rules_test.cc:288` | 断言 M 值 → 改为 `han`/`base_points` + 保留 `delta` 零和断言 |
| `ScoreMultipliers` | `:318` | 同上；`M ∈ {1,4,16}` 的枚举断言要整体重写 |
| `HonorSevenPairsAndScoring` | `:570` | 同上（含字牌口径下的番种判定） |
| `HonorIsNotPureOneSuit` / `WinPureOneSuit` | `:591` / `:205` | 判定语义不变，但断言从 tag 字符串改成 `Yaku.id` |
| **新增** | — | `DetectYaku` 顺序冻结、同手牌多次调用结果一致、零和属性测试（随机 40 手）、`NextDealer` 全表（含连庄）、`DeriveRoundSeed` 纯函数 + round 0 恒等、`CanAnkan/CanKakan` 边界（`remaining==0`、已有 pong、4 张同面） |
| `GameStartOpensTheDealerDiscardWindow` | `mahjong_room_test.cc:921` | 断言 `dealer == 0` 且庄家 14 张 → 需加"第 2 局 dealer == 1"用例 |
| `FullGamesSatisfyProtocolInvariants` | `:1145` | **改动最大**：循环体从"一局"变"一场"；`ASSERT_EQ(ends.size(), 1u)`（`:1200-1201`，"每局恰好一个 `game_end`"）要改成"每场恰好 `round_count` 个"; 牌数守恒（`:1177-1190`）必须把暗杠 4 张算进 `FlattenMelds`（已按 meld 全量算，天然覆盖）；seq 严格递增（`:1193-1198`）自动变成跨局断言，正好守住 C4 |
| `FinishedCallbackFiresExactlyOncePerGame` | `:1412` | 改名 + 断言"每局一次、场终一次、`match_over` 只有最后一次为真" |
| `PacedGamesAlwaysTerminateAndSettleCorrectly` | `:886` | 场次化后必须仍然终止（依赖 C15 硬上限） |
| `PostMeldDiscardWindowRejectsWinEvenWhenTheHandIsComplete` | `:1347` | 新的打牌窗口动作矩阵（H1 扩写）会改变这个用例的前提 |
| `ClaimWindowRejectsDiscardAndUnknownAction` | `:1282` | `IsKnownActionName`（`protocol.h:145-147`）扩词表后，用例里的"未知动作"必须换成一个**仍然未知**的名字 |
| `RoomLevelErrorsCountTowardTheQuota` | `:1451` | 若新增错误分支，配额归属要复核 |
| `ReclaimsARoomAndItsConnectionAfterGameEnd` | `lobby_test.cc:108` | **语义变为"场终才回收"**，必须重写为多局 |
| `room_full_game_test.py` | `:346`、`:361-363`、`:220-230` | Σ delta 零和不变；`multiplier in (1,4,16)` 与 `total == 6*M` 要按新番数口径重写；牌数守恒（`:220-230`）加上暗杠后仍是 136，**不用改** |
| `account_e2e_test.py` | `:377`、`:417` | `games == 1` → `games == round_count`（若保留"每局一行"口径） |
| `client_headless_test.py` / `e2e_full_game.py` | — | 整场帧序列、结算页/场终页的界面迁移断言 |
| `discard_policy_evidence.py` | — | 只依赖打牌策略，**预计零改动**（需跑一遍确认） |

---

## 6. 战绩存储

### 6.1 现状够不够

| 需要的字段 | 现状 | 结论 |
| --- | --- | --- |
| 每局每座位的 `delta_score` / `win` / `reason` / `room_id` / `ended_at` | `matches` 表全有（`db_mysql.cc:232-242`） | ✅ **够** |
| 场次总分 | 无 | ❌ 可由 `SUM(delta_score) GROUP BY room_id` 推出来，但"房间"与"场次"是多局后才等价的，硬算会与"每局一行"耦合 |
| 最高番（best_win.han / yaku 列表） | 无 | ❌ 需要新列或新表 |
| 连庄次数 / 最终名次 | 无 | ❌ 需要新列或新表 |

### 6.2 关键发现：**没有 SCHEMA_VERSION，也没有迁移框架**

- `ACCOUNT_DESIGN.md:31-63` 只规定"启动时 `CREATE DATABASE IF NOT EXISTS` + `CREATE TABLE IF NOT EXISTS` 自建"；`db_mysql.cc:219-249` 的 `kSchema[]` 就是两条 `CREATE TABLE IF NOT EXISTS`，**没有任何版本表、没有 `ALTER TABLE`、没有迁移脚本**。
- 含义：给 `matches` 加列**不会**自动发生在已存在的库上（`IF NOT EXISTS` 对已存在的表是 no-op），必须写 `ALTER TABLE`，而 MySQL 8.0 **不支持** `ADD COLUMN IF NOT EXISTS` ⇒ 要自己查 `information_schema` 判重。

### 6.3 建议：**加表不改表**

| 方案 | 做法 | 迁移成本 | 回归风险 | 建议 |
| --- | --- | --- | --- | --- |
| **A：新增 `match_summaries` 表** | 沿用 `CREATE TABLE IF NOT EXISTS` 手法加第 3 条 DDL：`account_id, room_id, rounds_played, renchan, total_score, best_han, best_yaku(VARCHAR→或 JSON), rank, ended_at`。`matches` 保持"每局一行"语义不变 | **零迁移**（新库老库都自建） | 零（`StatsJob` 的两个查询 `db_mysql.cc:388-391`、`:412-415` 不动） | ✅ **推荐** |
| B：给 `matches` 加列 | 写 `ALTER TABLE` + `information_schema` 判重 | 中（要写幂等迁移，且没有版本表可挂） | 中（`INSERT`/`SELECT` 两处 SQL 都要改，`db_stub.cc` 也要同步） | 除非要在同一行上同时存局与场 |
| C：把场次数据塞进 `reason` 字符串 | — | 零 | 高 | ❌ 破坏 `SUM(reason='draw_exhausted')` 的 `draws` 统计（`db_mysql.cc:390`） |

### 6.4 连带要改的三处（加表方案的完整影响面）

| # | 位置 | 改动 |
| --- | --- | --- |
| 1 | `db.h:130-131`（`kActorMethods` 元组）+ `DbService`（`db.h:172-222`） | 新增方法 `RecordMatchSummary` 并加进反射元组 + 门面路由（`db.h:211` 的 `PickByAccount` 复用） |
| 2 | `db_mysql.cc` | ①`kSchema[]` 加第 3 条 DDL；②新增 `SummaryJob`；③`StatsJob` **不动** |
| 3 | `db_stub.cc:59-61` | 新增空实现（否则 `-DMAHJONG_WITH_MYSQL=OFF` 构建失败） |

> `stats` 帧（`protocol.h:168-178`、`protocol.cc:39-58`）**不必改**：场次汇总走 `match_end` 帧，与战绩查询解耦；若将来要在战绩页显示最高番，再给 `StatsSummary` 加可选字段（同样是纯增补）。

### 6.5 写入时机与幂等

沿用现有 fire-and-forget 口径（`ACCOUNT_DESIGN.md:81`、`:185-192`）：场终通知里发一次 `RecordMatchSummary`，失败只记日志；**不做去重键**（依赖"场终恰好一次"的保证，与现有 `NotifyFinished` 的 exactly-once 同源：`room.cc:271-297`，测试 `mahjong_room_test.cc:1412` 已覆盖"局终恰好一次"，需补"场终恰好一次"）。

---

## 7. 确定性与可复现

### 7.1 现状的三个确定性来源

| 来源 | 位置 | 是否跨局可用 |
| --- | --- | --- |
| 牌山：`BuildWall(seed)` = `mt19937(seed)` + 降序 Fisher–Yates，纯函数 | `mahjong_rules.cc:207-223`、`mahjong_rules.h:89` | ✅ 换 seed 即可换局 |
| 房间 seed 派生：`DeriveRoomSeed(base_seed, room_index)`（奇数乘法 + SplitMix64 finalizer，**room 0 为恒等**） | `room.h:71-92`、`lobby.cc:132-137` | ✅ **现成的范式** |
| AI 决策：同步、纯函数、只用 `(hand, melds, drawn, options)` | `room.cc:312-360`、`:826-868`、`mahjong_rules.h:192-210` | ✅ 与局无关 |

### 7.2 跨局怎么做

```
round_seed(round) = DeriveRoundSeed(match_seed, round)         // 与 DeriveRoomSeed 同款，round 0 恒等
match_seed        = DeriveRoomSeed(server_seed, room_index)     // 现状不变
```

- `round 0` 取恒等 ⇒ **既有 `--seed` 复现性完全保留**（现有测试 `RoomsOfOneServerDealDifferentHandsAndStayReproducible`（`mahjong_room_test.cc:686`）与 `RoomWallSeedsArePureAndPerRoom`（`:658`）继续绿）。
- 双射性沿用 `room.h:78-83` 的论证（odd multiplier + finalizer 都是 64 位双射 ⇒ 不同 round 不会撞同一副牌）。
- 实现位置：`RoomActor::StartRound`（C2）内 `wall_ = BuildWall(DeriveRoundSeed(seed_, round_index_))`；`room.h` 里加一个 inline 纯函数，**不进规则库**（它是房间层概念，规则库保持"只懂牌"）。

### 7.3 确定性契约的边界（必须写进文档，否则验收会扯皮）

| 边界 | 事实 | 依据 |
| --- | --- | --- |
| `--seed` 可复现的是**牌与决策序列**，不是**帧字节** | `deadline_ms = clock::EpochMs() + timeout`，墙钟进入帧 | `room.cc:705`、`:808` |
| AI 节奏化会削弱端到端可复现 | `--ai-delay-ms > 0` 时决策以定时消息到达，人类/脚本帧的到达顺序会影响结果 | `room.cc:299-310`、`room.h:94-98` |
| 全确定性只在 `--ai-delay-ms 0` + 脚本化客户端下成立 | 现有测试 `AiDelayZeroKeepsTheSynchronousPath`（`mahjong_room_test.cc:773`）已覆盖该路径 | 同上 |
| 场次可复现的前置 | 每局牌山由 `(match_seed, round_index)` 决定；庄家序列由 `NextDealer` 决定；两者都是纯函数 ⇒ 整场可复现 | §7.2 |
| 建议验收方式 | `e2e_full_game.py --seed S` 连跑两次，比较**帧类型+关键字段（去掉 deadline_ms）**的序列哈希；现有 `e2e_full_game.py` 已有 7 处 `game_end` 断言可扩展 | — |

---

## 8. 实现顺序、依赖与工作量

### 8.1 批次划分（B0–B5）

| 批次 | 内容 | 依赖 | 相对工作量 | 可并行 |
| --- | --- | --- | --- | --- |
| **B0** | **文档先行**：`PROTOCOL.md` v1.6（§2.2 帧表、§4.6 动作词表、§5.x 增字段、新 §5.15 `match_end`、§6.1 窗口表补"打牌窗口的附加 options"、§0.2 修订 H1、§0.6 84/21 改"每局"、附录 A/C）；`RULESET.md` v1.3（§1.1/§1.4 庄家参数化、§1.5 暗杠补牌、§2.1 连庄、§3 番种判定、§4.3/4.4 暗杠加杠、§5 打牌窗口 options、§6 番数结算、§7 场次终止证明）；`GAME_DESIGN.md` v1.10（§2.2 局制、§9 预留项转正、附录 C）；`ACCOUNT_DESIGN.md` v1.3（场次汇总口径） | 创意总监设计案（番种/局制/抢杠） | 中 | ❌ 必须最先 |
| **B1** | 规则库：`DetectYaku` + `ScoreResult` 扩展 + `CanAnkan`/`CanKakan` + `NextDealer` + `DealTiles(wall, dealer)` + `DiscardOptions` 扩 kong/ankan/kakan | B0 | **大** | ✅ 与 B4a 并行 |
| **B2** | 房间状态机：C1–C9、C13、C15（含 C3 的 `melds_`/`discards_` 重置、C6 推进令牌） | B1（要用新算分与新 options） | **大** | ❌ 串行瓶颈 |
| **B3** | 大厅/存储：C10（回调语义拆）、§6.3 新增 `match_summaries` 表 + `RecordMatchSummary`（db.h/db_mysql.cc/db_stub.cc 三处） | B2（要 `match_over` 字段） | 小 | ✅ 可与 B2 后半并行 |
| **B4a** | 客户端协议层：`game_state.py` 的动作词表/窗口选项、`match_end` 处理、`game_start` 多局重置验证、`MELD_TYPES` 附加字段 | B0（帧形状定了就能写） | 中 | ✅ 与 B1 并行 |
| **B4b** | 客户端界面：番数/番种展示（`render/result.py`）、场次 HUD（局数/庄家/累计分）、暗杠/加杠按钮（`render/actionbar.py:61-101` 加两个 kind）、**局终结算页 vs 场终结算页**两个屏幕状态 | B4a | 中 | ❌ |
| **B5** | 验收：`room_full_game_test.py`（`:346`/`:361-363`）、`account_e2e_test.py`（`:377`/`:417`）、`client_headless_test.py`、`e2e_full_game.py`、`lobby_test.cc:108` 重写；新增"整场确定性"脚本 | B2 + B3 + B4 | 中 | ❌ |

### 8.2 依赖图

```
创意总监设计案 ──► B0 文档 ──┬─► B1 规则库 ──► B2 房间 ──┬─► B3 大厅/存储 ──┐
                            │                              │                    ├─► B5 验收
                            └─► B4a 客户端协议层 ──► B4b 界面 ─────────────────┘
```

关键路径：**B0 → B1 → B2 → B5**。B3 与 B4 都可以塞进 B2 的空档并行。

### 8.3 必须先做 / 可以后做 / 明确不做

| 分类 | 内容 |
| --- | --- |
| **现在就要做**（本批不可省） | B0 全套；C3（`melds_`/`discards_` 重置，**不做就是确定的 bug**）；C6（推进令牌隔离）；C9（`DealTiles` 庄家参数化）；C10（`match_over` 门控回收）；C15（场次硬上限）；§5.3 四条约束与零和断言 |
| **可以后做**（本批不做也能发布） | `match_summaries` 表（可先用 `SUM(delta_score)` 在 `stats` 侧临时算）；客户端场次 HUD 的动画/过渡；`best_win` 展示；`state_sync` 里加场次字段 |
| **明确不做**（本轮范围外） | 断线重连 + 房间快照持久化（C12）；局间换人/加入（C14）；观战；立直/宝牌/振听/符数；抢杠（除非创意总监明确要，见 §9 Q2）；多真人同房；移动端；平台合规/上架 |

### 8.4 最大的单点风险

> **S1（最高）：C4/C5/C6/C7 —— 房间状态机从"单局"变"场次"。**
>
> 为什么是它：
> 1. `Phase` + `Pump` 是**全状态机中枢**（`room.cc:609-669`），`kFinished` 现在是**终态**（`room.h:262`、`room.cc:613`），"终态可回到活态"是整个文件里唯一的结构性假设被推翻；
> 2. `seq`（`room.cc:570-575`）、超时消息（`:1089-1152`）、AI 节奏消息（`:299-310`）三套异步机制全部建立在"窗口唯一且 seq 唯一"之上，局间新增第四种定时消息（C6）是最容易写错的地方 —— **一旦推进令牌与旧窗口 timeout 混淆，表现是"某一局偶发卡死 15s 后按默认动作继续"**，靠单测很难稳定复现；
> 3. 它同时牵动 C3（重置遗漏）、C9（庄家错位）、C10（回收时机）、C13（内省语义）四个次级改造点，任一漏掉都是**静默错误**而非编译错误（C3 漏了牌数守恒测试会抓到；C9 漏了只是"庄家标记错"，测试抓不到）。
>
> 缓解措施（必须一起做）：
> - 用 `mahjong_room_test.cc:1145` 的 `FullGamesSatisfyProtocolInvariants` 改成"跑整场"，并把三件事升级为**整场级断言**：牌数守恒每帧成立、`seq` 跨局严格递增、每局 `game_start` 恰好一次；
> - 加一条"整场确定性"e2e（`--seed S` 跑两次比序列哈希，见 §7.3）；
> - C6 的令牌位**一次性设计到位**（新增独立 bit，不要复用 `kAiPacingTokenBit`，也不要用"seq 不变"这种隐式判据）；
> - C15 的硬上限当**开发期断言**用，先设小（如 `max_rounds=2`）跑通再加到 4。
>
> **S2（次高）：B1 的番数结算 × 零和守恒 × 全链路断言。** `M ∈ {1,4,16}` 被 C++ 单测（3 处）、房间测试、2 个 Python 测试、`game_end` 字段、DB 数值共同锚定；改番数口径是"一处改、五处断"。缓解：先做 `DetectYaku`（可独立单测）再改支付算术；`ScoreResult` 保留 `multiplier`/`per_payer`/`total` 字段名，把"改形状"降级为"改填充值"。
>
> **S3（第三）：暗杠/加杠的可见性口径未定。** 暗杠 4 张是否对他人可见，直接决定 `seats[].melds[].tiles` 要不要打码（`PROTOCOL.md:711-724` 的私有信息矩阵没有这一行）。若要打码，`state_sync` 的 `melds` 形状与"和牌者扁平手牌长度 = 14 + 杠数"（`PROTOCOL.md:546`）都要重新定义，成本会翻倍。

---

## 9. 待确认（需要创意总监/主理人裁决，我不代决）

> **裁决状态（主理人，B0 定稿时）**：Q1 `max_renchan = 4`（单场上界 8 局；`RENCHAN_MAX` 仍需用户点头）/ Q2 **不做抢杠且永久不做** / Q3 **不改**（同一牌山，83·84·21 不动）/ Q4 **对他家不可见**（他家 `tiles` 恒 `[]`，局终揭示）/ Q5 **不进** `state_sync` / Q6 **不做役满，封顶 6 番**。下表保留原始提问作为决策记录。

| # | 问题 | 为什么架构上必须现在定 | 归属 |
| --- | --- | --- | --- |
| Q1 | **连庄的硬上限**：连庄不占局数 ⇒ 理论上无上界。是否设 `max_renchan`（建议 4）或"总分封顶即结束"？ | 决定 C15 是否存在，进而决定房间存活时间与"一局必然终止"的证明能否升级为"一场必然终止"（`RULESET.md:376`） | 创意总监 + 主理人 |
| Q2 | **加杠是否设抢杠窗口**：若要，加杠就从"一次自理动作"变成"一个需要他家应答的窗口"，`seq`/超时/托管/AI 鸣牌门槛全部要多一条路径 | 工作量差 2–3 倍；且 `RULESET.md:268-277` 现明确"不实现抢杠" | 创意总监 |
| Q3 | **杠补牌是否改从王牌/岭上牌取**：若改，83/84/21 三个冻结数全部重算（`RULESET.md:92-101`、`PROTOCOL.md:64`） | 影响协议文档与 3 处测试断言 | 创意总监 |
| Q4 | **暗杠是否对他人可见**（S3） | 决定 `melds[].tiles` 是否要打码，以及"扁平手牌 14+杠数"是否还成立 | 创意总监 + 协议 |
| Q5 | **场次总分是否进 `state_sync`**：我建议不进（热路径，`room.cc:447-466`）；若 UI 要求"牌桌上常显累计分"，则要在每次摸牌的 4 份广播里加 4 个整数 | 每帧成本 ×4 局；`RoomPerf.ConcurrentRooms`（`lobby_test.cc:291`）会受影响 | 创意总监（UI 需求）+ 我（成本） |
| Q6 | **番种表是否含役满/封顶**：若含，`ScoreResult` 需要 `limit` 分支，支付公式从"线性"变"分段" | 决定 §5.2 的 `base_points` 是否要变成 `{base, cap}` | 创意总监 |

---

## 10. 本评估明确不覆盖

1. 番种表内容、番值、连庄判定规则、庄家轮转规则的具体语义（创意总监的设计案）。
2. 任何代码与测试的实际修改（P1.5 起）。
3. 性能预算表的完整定义（帧率/Draw Call/包体与本批无关；本批只有"消息量 ×4、房间寿命 ×4、DB 行数 ×4"三个容量影响，已在 §8.4 与 Q5 记录）。
4. 多平台打包、Steam / 主机 / 移动端合规与发布管线（不在本轮）。
5. 断线重连与房间状态持久化（C12：明确不做）。

---

## 11. B0 定稿回写（v1.6 定稿时新发现的两条实现须知）

> 本节是 B0（`PROTOCOL.md` v1.6 / `ACCOUNT_DESIGN.md` v1.3 定稿）之后对本文的补充，写给 B2/B3 的派工对象。

| # | 新发现 | 影响 | 处置 |
| --- | --- | --- | --- |
| **N1** | **暗杠隐藏 ⇒ `seats[]` 不再"与收件人无关"**。v1.6 规定 `seats[].melds[].ankan.tiles` 对他家恒为 `[]`、对本家带 4 张。而现有 `BuildSeatsArray()` 被刻意做成**收件人无关**并在广播时只渲染一次、拼进 4 份帧（`room.h:344-348` 注释、`room.cc:447-466` 的 `seats_json` 复用）。**该优化在暗杠存在时直接失效** | B2 的性能与代码结构 | 实现取"两版缓存"而不是"每座位重建"：按「本家视角 / 他家视角」各渲染一份 `seats_json`（2 次而非 4 次）；无任何暗杠时退化为 1 份（保持今天的成本）。**不得**为了省事而对所有人下发真实牌面——那是私有信息泄露（§8.1） |
| **N2** | **`match_end.rankings[]` 的并列跳号是协议契约**（`rank` = 1 + 严格大于本座位 `final_score` 的人数），不是"客户端自己排" | B2 必须算；B4 必须直接展示 | 服务端算好 `rank`；客户端**不得**重算。同时 `final_score` 必须与最后一次 `game_end.results[].match_total` 逐座位相等（同一份数据，一次算） |
| **N3** | `game_start` 增了 `round_index` / `round_count`（超出派工单原始的"只给 `game_end` 加字段"清单） | 客户端要靠它显示"第 N / 4 局"，且 `game_start` 是低频帧（每局一次），不违反 Q5 的热路径约束 | 若主理人认为应收窄，删这两个字段即可，其余不受影响；我按"需要"保留了 |
| **N4** | `matches` 表零改动 + 新增 `match_results`（`ACCOUNT_DESIGN.md` v1.3）⇒ B3 的落地是**三处**而非一处：`db.h`（方法 + `kActorMethods` 反射元组 + `DbService` 门面）、`db_mysql.cc`（`kSchema[]` 第三表 + 新 job）、`db_stub.cc`（空实现，否则 `-DMAHJONG_WITH_MYSQL=OFF` 构建失败） | B3 | 三处缺一不可，且 `StatsJob`/`RecordJob` 不动（零回归） |
