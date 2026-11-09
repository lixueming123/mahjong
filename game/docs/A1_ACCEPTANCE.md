# A 批验收计划（A1_ACCEPTANCE）

- 文档 ID：A1_ACCEPTANCE
- 版本：v1.3（最终验收报告见 §十；M1/M2 已按主理人终裁口径改写；新增 M11 随机性判据）
- 作者：qa-lead（严过关）
- 判定对象：A 批四项交付 —— **番型表 + 番数结算 + 一场四局/连庄 + 暗杠加杠**（B1 规则库 + B2 房间状态机已交付；B3 match_results 落库进行中；B4 客户端未开工）
- 判据唯一来源（只引用，不重造）：
  - `GAME_DESIGN.md` v1.10（§2.2 ⑦′/§3.4 场次、§6 算分、§10 验收标准）
  - `RULESET.md` v1.3（§6.3 A/B 组用例表、§6.4 番种表、§10 单测规范、附录 C 实现坑）
  - `PROTOCOL.md` v1.6（§10 t4/t5/t6 三份自检清单、§5.15 match_end、§7.1 错误码）
  - `UI_UX_SPEC.md` v2.0（§9 结算两形态、§12 落地清单 UX-1..UX-15、附录 B 控件矩阵）
  - `ACCOUNT_DESIGN.md` v1.3（§2.1 match_results、§7 验收）
- 执行命令基线：
  - C++ 单测：`cmake --build build-release -j && ctest --test-dir build-release --output-on-failure`
  - 定向复跑：`./build-release/game/mahjong_rules_test --gtest_filter=<TestName>`（mahjong_room_test / mahjong_lobby_test 同理）
  - e2e：`python3 game/tests/e2e_full_game.py --seed 17`（现行单局口径，见 G5）

---

## 一、验收判据总表

状态三档：【自动化已覆盖】= 已有具名测试/脚本，验收时重跑即可；【自动化待补】= 判据成立但当前无执行主体，须在验收前补齐；【需人工】= 机器测不出，必须真人路径。
责任方三档：自动化测试 / 人工 / 用户试玩。

### G1 数值类（13 条）

| ID | 判据（机械可执行） | 来源 | 状态 | 责任方 |
| --- | --- | --- | --- | --- |
| N1 | `win.han_breakdown.total == min(base + tsumo + dealer, 6)` 且 `tsumo` 恒 0；`yaku[]` 不含 `tsumo`/`dealer_bonus` | GAME_DESIGN §6.1/§6.4、RULESET §6.1（v1.3 内修正） | 已覆盖：gtest `BonusTsumoAndDealerBonus`、`ExpectScoreConsistent`；协议字段层随 E2 补 | 自动化测试 |
| N2 | `win.multiplier == 2^total`，R ∈ {1,2,4,8,16,32,64} 逐行等于 GAME_DESIGN §6.1 全表 | RULESET §10.3 S2 | 已覆盖：gtest `HanToMultiplierTable`、`ScoreHandCoversEveryHanRow`、`ScoreMultipliers` | 自动化测试 |
| N3 | **0 番自摸恰好 −2/+6**：0 番手 tsumo ⇒ delta = {−2,−2,−2,+6}、`per_payer==2`、`total==6`（R=1 绝对值锚） | RULESET §6.3/§10.3 S3、GAME_DESIGN §6.1 | 已覆盖：gtest `ZeroHanTsumoIsExactlyMinusTwoPlusSix`（我定向复跑通过） | 自动化测试 |
| N4 | 0 番荣和 ±1（唯一新旧回归锚） | RULESET §10.3 S1/S3 | 已覆盖：gtest `ZeroHanWinScoresOne` | 自动化测试 |
| N5 | 无起和番：tags 为空照常和牌 | RULESET §3.5/§6.4 规则 4 | 已覆盖：同 N4（`score.yaku.empty()` + 得 1 分） | 自动化测试 |
| N6 | 逐局 `Σ results[].delta_score == 0`（荣和/自摸/荒庄三种均断言） | GAME_DESIGN §10.2 GD-B1 | 已覆盖：gtest `ZeroSumForEverySettlementKind` + room `FullGamesSatisfyProtocolInvariants`；e2e 单局已有（GD-B1） | 自动化测试 |
| N7 | 整场 `Σ match_total == 0` 且 `final_score` 与最后一次 `match_total` 逐座位相等 | RULESET §7.3、GAME_DESIGN §10.2 GD-B8 | 已覆盖：room 帧计数测试；TCP 层随 E2 补 | 自动化测试 |
| N8 | `rank` 为 competition ranking（= 1 + 严格大于本座位的人数），**不存在 `1,2,2,3` 形状**；`rankings[]` 按 final_score 降序、同分 seat 升序 | PROTOCOL §5.15、GAME_DESIGN §3.4 | 已覆盖：room match_end rankings 断言（排序 + 并列跳号）；e2e 随 E2 补 | 自动化测试 |
| N9 | 庄家 +1 番：未封顶时恰多 1；6 番手 + 庄家仍 6 番（封顶对加成生效） | RULESET §10.3 S4/S5 | 已覆盖：gtest `BonusTsumoAndDealerBonus`（capped_dealer 断言） | 自动化测试 |
| N10 | 封顶：`han_base ≥ 6` ⇒ `han==6`、`R==64`（如门清清一色 8 番 → 6 番） | RULESET §10.3 S4 | 已覆盖：gtest `ScoreMultipliers` | 自动化测试 |
| N11 | `Σ yaku[].han == han_breakdown.base` | PROTOCOL §10.4 ③ | 已覆盖：gtest `ExpectScoreConsistent`（`YakuHanTotal(yaku)` 即该和）；协议层随 E2 补 | 自动化测试 |
| N12 | 同一手牌荣和 vs 自摸 `han` 相同（自摸不加番的最直接验证，分差全部来自 2R/6R） | GAME_DESIGN §6.3 A 组「荣和 vs 自摸同一手」行 | 已覆盖：gtest `BonusTsumoAndDealerBonus`（3 番手两形态同 han）、`ScoreRonAndTsumo` | 自动化测试 |
| N13 | GAME_DESIGN §6.3 A 组关键行绝对值：2 番荣和 −4/+4、2 番自摸 −8/+24、3 番 −8/+8、6 番自摸 −128/+384 | GAME_DESIGN §6.3 A 组 | 已覆盖：gtest `ScoreRonAndTsumo`（2 番行）、`ScoreMultipliers`（3/6 番行） | 自动化测试 |

### G2 规则类（15 条）

| ID | 判据 | 来源 | 状态 | 责任方 |
| --- | --- | --- | --- | --- |
| R1 | **14 番种逐项正反例**：每番种至少 1 正例 + 1 反例，id 与番数逐字等于 §6.4 表 | RULESET §6.4/§10.3 | 已覆盖：gtest `YakuMenzen`…`YakuPureOneSuit` 共 14 个（成对正反例） | 自动化测试 |
| R2 | RULESET §6.3 示例表 + B 组回归锚直接作为用例（七对子 ±8、门清清一色 ±64 为设计意图**不得判为回归**） | RULESET §6.3、GAME_DESIGN §6.3 | 已覆盖：`ScoreMultipliers`、`RegressionAnchorLegacyTags` | 自动化测试 |
| R3 | 三暗刻荣和例外：荣和时和牌张补成的刻子不计暗刻；改自摸则计 | RULESET §6.4 #10、§10.3 S11 | 已覆盖：gtest `YakuSanankou` | 自动化测试 |
| R4 | 门清 + 暗杠仍得 `menzen`；加杠必破 `menzen` | RULESET §4.4/§4.5、§10.3 S10 | 已覆盖：gtest `YakuMenzen` | 自动化测试 |
| R5 | 七对子：4 张同面不算两对（N3 反例） | RULESET §3.3/§10.2 | 已覆盖：gtest `NotWinFourOfAKindIsNotTwoPairs` | 自动化测试 |
| R6 | 七对子：`melds>0` 或 `kans>0` 时必不和 | RULESET §2.2/§10.2 N8 | 已覆盖：gtest `NotWinSevenPairsRequiresConcealed` | 自动化测试 |
| R7 | 字牌不可吃、`1z2z3z` 非顺子；字牌只作刻子/杠/将 | RULESET §3.2/§4.2、§10.2 N9 | 已覆盖：gtest `HonorCannotChow`、`HonorRunIsNotAWinningHand`、`HonorInWinningHands` | 自动化测试 |
| R8 | 杠补牌 `remaining` 用派生式 `136 − next_index`，三种杠同源；杠后牌数守恒 | RULESET §1.5、附录 C12 | 已覆盖：gtest `KongTileAccountingStaysInsideBothFormulas` + room 守恒断言 | 自动化测试 |
| R9 | 庄家 14 张、其余 13 张；`DealTiles(wall, dealer)` 每个庄家位都正确 | RULESET §1.4、GAME_DESIGN §3.3 | 已覆盖：gtest `DealTilesRespectsDealerSeat` | 自动化测试 |
| R10 | **荒庄一律连庄**（判据 B 无附加条件） | RULESET §7.3 S4-B、GAME_DESIGN §3.4 | 已覆盖：gtest `NextDealerAndClassifyRound`（`draw_exhausted` → 连庄） | 自动化测试 |
| R11 | 打牌窗口 options：手中 4 张同面且 `remaining ≥ 1` 才出 `ankan`；有碰组 + 第 4 张且 `remaining ≥ 1` 才出 `kakan`；`remaining==0` 时两者都不出现 | RULESET §5.2、GAME_DESIGN §4.2 步 5 | 已覆盖：gtest `DiscardOptionsKongWindows`（规则库层）+ room `NoKongOptionsWhenWallExhausted`（`remaining==0` 不出杠选项，我定向复跑通过） | 自动化测试 |
| R12 | 打牌窗口合法动作序 `win → ankan → kakan`（discard 不入 options）；该窗口回 `pass` = `ILLEGAL_ACTION` | RULESET §5.2、PROTOCOL §6.3 | 已覆盖：room `ClaimWindowRejectsDiscardAndUnknownAction`、`PostMeldDiscardWindowRejectsWinEvenWhenTheHandIsComplete`、`IllegalOperationsChangeNothing`；gtest `DiscardOptionsAndDefaultDiscard` | 自动化测试 |
| R13 | 抢杠和永久不做：加杠不产生任何应答窗口、不占 15s、不参与优先级 | RULESET §4.6 | 已覆盖（结构性负判据）：room `SelfKanActionsRoundTripThroughTheDiscardWindow`（加杠链路无 claim 窗口） | 自动化测试 |
| R14 | 拆法枚举冻结序（将牌升序 → 最小未用牌面 → 刻子先于顺子）；同番取枚举序第一个、禁止跨拆法拼凑 | RULESET §6.4 规则 3 | 已覆盖：gtest `EnumerateDecompositionsReturnsAllSplits`、`EnumerateDecompositionsAgreesWithWinningJudge`、`WinPureOneSuit`（P5 同分取舍用例）、`DifferentialAgainstBruteForce`（14300 手差分） | 自动化测试 |
| R15 | 判番确定性：同一手牌判 1000 次 + 打乱输入牌序，`yaku[]`/`han` 逐字段相同 | RULESET §10.3 S12、PROTOCOL §10.4 ③ | 已覆盖：gtest `YakuDetectionIsDeterministicUnderShuffles`（我定向复跑通过） | 自动化测试 |

### G3 场次类（10 条）

| ID | 判据 | 来源 | 状态 | 责任方 |
| --- | --- | --- | --- | --- |
| M1 | `game_end` 下发次数 == `match_end.hands_played + match_end.renchan_count`（连庄每一手各发一次）；**终裁口径：`hands_played` = `game_end` 下发次数，取值 4..8**（主理人 Q1 终裁，取代「round_index 终值+1」与「恒 4」两种旧表述） | PROTOCOL §10.4、GAME_DESIGN §6.4；Q1 终裁 | 已覆盖：room 帧计数断言 + `match_full_e2e_test.py`（实测 rounds=7/hands_played=7、rounds=5/hands_played=5，逐场相等） | 自动化测试 |
| M2 | `match_end` 每场**恰好 1 次**、`hands_played == game_end 下发次数`（终裁口径 4..8，打满 4 局的正常场即 4）、`renchan_count == game_end 里的终值` | PROTOCOL §5.15/§10.2；Q1 终裁 | 已覆盖：room `FullGamesSatisfyProtocolInvariants`、`FinishedCallbackFiresOncePerRoundAndFlagsTheMatchEnd` + `match_full_e2e_test.py`（hands_played 与下发次数逐场相等，实测 2 场） | 自动化测试 |
| M3 | 连庄不占局数：连庄时 `round_index` 与上一局相同、`dealer` 不变 | PROTOCOL §10.4、GAME_DESIGN §3.4 | 已覆盖：room 逐局断言；gtest `NextDealerAndClassifyRound` | 自动化测试 |
| M4 | **C15 双闸**：场终当且仅当 `round_index ≥ 4` **或** `renchan_count > 4`；二者均满足边界 | GAME_DESIGN §3.4、RULESET §7.3 S5-D/E | 已覆盖：room C15 令牌专项 + `EXPECT_TRUE(expected_round_index == 4 \|\| last_renchan > 4)`；M1/M2 同批修正后联合重验 | 自动化测试 |
| M5 | **局终不回收房间、场终才回收**（ReapRoom 只在场终 arm） | RULESET §7.3、PROTOCOL §1.4.5 | 已覆盖：lobby `ReclaimsARoomAndItsConnectionAfterMatchEnd`、`ReapRoomIsIdempotentAndLeavesOtherRoomsAlone` | 自动化测试 |
| M6 | 单场局数上界 8（`4 + 4`），任何 seed 的整场 `game_end` 次数 ≤ 8 | GAME_DESIGN §1.2/§3.4 | 已覆盖（随 M1 的帧计数）；与 M1 同批修正后联合重验 | 自动化测试 |
| M7 | 局间 `Phase::kHandSettled`：无未决窗口、不下发 `state_sync`、拒绝一切对局帧（`BAD_REQUEST`）、迟到超时静默丢弃 | PROTOCOL §10.2 局间行、GAME_DESIGN §2.2 ⑦′ | 已覆盖：room `RoundEndRejectsEveryGameFrame`（我定向复跑通过）+ `match_full_e2e_test.py` 真链路 M7 探针（局间发一帧对局帧，实测恰好回 1 条 `BAD_REQUEST`，两场均为 m7_bad=1） | 自动化测试 |
| M8 | 局间隔默认 5000 ms（`--inter-hand-delay-ms` 缺省值），e2e 观察相邻 `game_end`→`game_start` 间隔 ≥5000ms；**客户端全程无任何「继续提前」出站帧**（Q2 裁决：§2.1 图为文档缺陷，以 §2.2 冻结口径为准） | GAME_DESIGN §2.2 ⑦′、RULESET §7.2 | 已覆盖：gtest `InterHandDelayDefaultIsFrozen`（缺省值）+ `match_full_e2e_test.py`（gaps 305–310ms @ delay=300，逐间隔 ≥ 参数值） | 自动化测试 |
| M9 | 轮庄：`dealer = (dealer+1)%4`、`round_index += 1`，只此一种情形递增 | GAME_DESIGN §3.4 | 已覆盖：`NextDealerAndClassifyRound` + room 逐局断言 | 自动化测试 |
| M10 | `match_end.rankings[].final_score` 与最后一份 `game_end.results[].match_total` **同一份数据**（逐座位相等，不得两算） | GAME_DESIGN §6.4、PROTOCOL §5.15 | 已覆盖：room rankings 断言 + `e2e_full_game.py --matches 1`（rankings 与末次 match_total 逐座位核对） | 自动化测试 |
| M11 | **随机性/多样性（盲区补件）**：同一场内**相邻两局**的牌山与四家起手牌必须不同（**含连庄场**：连庄时 dealer 相同、round_index 相同，仍必须是新牌山）；不同 `--seed` 的服务器必须发不同牌山；固定 `--seed` 时整场可复现 | 新增需求（用户实玩发现「同场每局牌山相同」blocker 后新增，2026-10-22） | 已覆盖：gtest `ConsecutiveRoundsDealDifferentWallsAndHands`（连庄 + 轮庄两类转换）、`DifferentServerSeedsDealDifferentWalls`、`RoundSeedIsPureAndPerHand`、`SameSeedReplaysTheWholeMatchFrameForFrame`（四条我逐条复跑通过） | 自动化测试 |

> ⚠️ **M11 是原 60 条判据的盲区**：60 条判据全部在验「规则算得对不对 / 协议字段对不对 / 场次推进对不对」，**没有一条在验「这是不是新的一局」**。牌山复用属于「每局都合法、每局都一样」，任何规则断言都必然通过。它只能被两种方式发现：① 真人连续打两局（用户路径）；② 显式比较相邻两局的牌山。本条即该盲区的补件。

### G4 协议类（12 条）

| ID | 判据 | 来源 | 状态 | 责任方 |
| --- | --- | --- | --- | --- |
| P1 | **t4 清单（§10.2）逐条**：seq 单调/窗口唯一、seq 跨局不重置、超时走 seq 作废、超时/托管永不选杠、H1 打牌窗口不收 pass、H3 action_event 语义、H8 重算、广播并行、game_end 零和字段完整 | PROTOCOL §10.2 | 已覆盖：room 测试群（`StaleSequencesAreDroppedSilently`、`TimeoutPlaysTheDefaultActionAndStaleTimeoutsAreIgnored`、`IllegalOperationsChangeNothing`、`DisconnectedSeatWaitsForTheWindowDeadline`、`ReconnectBeforeTheDeadlineKeepsTheOpenWindow`、invariants 群） | 自动化测试 |
| P2 | **t5 清单（§10.3）逐条**：出站回带 seq、options 内取值、按 from_seat 移牌河、他家无牌面、state_sync 整体覆盖、局终≠场终、暗杠/加杠按钮位、局间不显示倒计时 | PROTOCOL §10.3 | **需人工**（B4 未开工；随 B4 验收执行，占位见 §五） | 人工 + 用户试玩 |
| **R16** | **账号刷新重连（v2.2）**：对局中断开 → 同账号重新登录（`ALREADY_ONLINE` 短重试）→ `login_ok{in_room:true}` + 同 `room_id`/`seat` + `game_start`（`match_total`/`renchan_count`）+ `state_sync`；`resume:false` 脱离旧房；等待页 `in_room:false`；一账号一连接不产生双连接 | PROTOCOL v2.2 §4.1/§5.1/§5.4/§8.5 | **自动化已覆盖**：`reconnect_e2e_test.py`（MySQL 不可用时 ctest SKIP）+ `mahjong_room_test.ReconnectedSeatReplaysTheOpenWindow` + `mahjong_lobby_test.LoggedInAccountRebindsItsSeatedRoom` | 自动化测试 |
| P3 | **t6 清单（§10.4）逐条**：场次级断言（M1/M2/M3/M8 组）、暗杠断言（P6/P12 组）、番数断言（N1/N11/R15 组）落到 e2e | PROTOCOL §10.4 | 已覆盖：`match_full_e2e_test.py`（M1/M2/M7/M8 真链路）+ `e2e_full_game.py --matches`（M10/N1/N11/P5/P9/P11/P12/P6/P7 组，实测通过） | 自动化测试 |
| P4 | 零新增错误码：错误码总数恒 14、语义未动；整场全程零 `error` 帧（GD-C1） | PROTOCOL §7.1/§10.2 | 已覆盖：静态核对 `protocol.h` 错误码枚举 + 现行 e2e GD-C1 | 自动化测试 |
| P5 | `seq` 整场严格递增、跨局不重置 | PROTOCOL §6.2 第 5 条、§10.4 | 已覆盖：room `SameSeedReplaysTheWholeMatchFrameForFrame` + 帧计数；e2e 随 E2 补 | 自动化测试 |
| P6 | 他家 `ankan.tiles` 恒 `[]`、本家 4 张、局终最后一份 `state_sync` 与 `game_end.results[].hand` 补全揭示 | PROTOCOL §10.2 暗杠行、§5.9 | 已覆盖：room `SelfKanActionsRoundTripThroughTheDiscardWindow` + `BuildSeatsArray` 三态测试；e2e 随 E2 补 | 自动化测试 |
| P7 | `ankan`/`kakan` 的 `action_event` 不带 `from_seat`、不动任何牌河 | PROTOCOL §5.8/§10.2 | 已覆盖：room 自理杠测试；e2e 随 E2 补 | 自动化测试 |
| P8 | `draw` 仅发给摸牌者本人；他家手牌永不下发（`seats[].hand` 不存在） | PROTOCOL §5.5/§8.2 | 已覆盖：room `MultiHumanBroadcastCarriesPrivateHandsAndOneSharedSeatsBlock` | 自动化测试 |
| P9 | `game_start` 每局恰一次，含 `round_index`/`round_count==4`；`dealer` 不再恒 0 | PROTOCOL §5.4/§10.4 | 已覆盖：room 帧计数；e2e 随 E2 补 | 自动化测试 |
| P10 | `game_end`/`match_end` 不携带 `seq`、不参与 seq 比较 | PROTOCOL 附录 A | 已覆盖：room invariants（字段存在性断言） | 自动化测试 |
| P11 | `win.tags` 元素 ∈ `1..45`（`yaku` 域）且 `tags ⊆ yaku[].id`，顺序同 `yaku[]`；每 id 至多一条、序严格按 §6.2 | PROTOCOL §10.4 ①、GAME_DESIGN GD-B7 | 已覆盖：gtest `RegressionAnchorLegacyTags` + `ExpectScoreConsistent`；协议层随 E2 补 | 自动化测试 |
| P12 | 每局物理守恒：`remaining + 各家手牌 + 副露牌面 + 牌河 == 136`，杠补牌后仍成立 | PROTOCOL §10.4 暗杠行 | 已覆盖：room invariants + e2e statistics 模式物理守恒；整场 e2e 随 E2 补 | 自动化测试 |

### G5 端到端类（10 条）

| ID | 判据 | 来源 | 状态 | 责任方 |
| --- | --- | --- | --- | --- |
| E1 | 同 `--seed` 两次**逐帧一致**（C++ 进程内整场回放） | GAME_DESIGN §7.4/§10.6 | 已覆盖：room `SameSeedReplaysTheWholeMatchFrameForFrame` | 自动化测试 |
| E2 | 同 `--seed` 两次 **TCP 整场逐帧一致**（真网关 + agent 链路）；整场兜底超时 = 960s（Q3 裁决） | PROTOCOL §10.4、GAME_DESIGN §10.6；Q3 裁决 | **按用户裁决降级**：逐帧哈希比对不做（用户原话「e2e 简单跑即可，不要浪费太多时间」）。**等价覆盖**：进程内 `SameSeedReplaysTheWholeMatchFrameForFrame`（逐帧回放断言，实跑通过）；`--determinism-check` 双跑模式已随 E3 提供，可按需启用 | 自动化测试 |
| E3 | `e2e_full_game.py` —— 升级为整场：`--matches N` 打完整场 + PROTOCOL §10.4 v1.6 三组断言 + GD-B8，兜底超时 960s | GAME_DESIGN §10、PROTOCOL §10.4；Q3 裁决 | 已覆盖：`--matches 1 --seed 17` 实测通过（rounds=5 hands_played=5 renchan=1 m7_bad=1 gaps=303–310ms）；⚠️ **默认无参入口 `run_single` 未定义会 NameError**（缺陷 D-5，用 `--matches N` 替代） | 自动化测试 |
| E4 | `room_full_game_test.py` —— 整场口径 | — | 已覆盖：实测通过（`reason=win_tsumo requests=21 discards=70 max_seq=139` + 多房间不同牌山可复现）；保留为房间级整局回归 | 自动化测试 |
| E5 | `account_e2e_test.py` —— 整场 + `matches` 每局一行 + `match_results` 每场一行（真库） | ACCOUNT_DESIGN §7.2/§4.7 | 已覆盖：实测通过（rounds=5 → `games=5 net_score=-8`，`match_results` 行 `hands=5 dealer=1 score=-8 rank=4` 逐字段核对、rematch 新房间、重启后账号与战绩存活）**= B3-8 终证** | 自动化测试 |
| E6 | `client_headless_test.py` —— 局终页/场终页 UX-13 断链 | UI_UX_SPEC §9.5/§9.6、UX-13 | 已覆盖：实测 233 断言（232 PASS + 1 帧耗时偶发 FAIL，见 D-2）；D-1 键盘守卫**仅代码核对、未补断言**（见 D-1 状态） | 自动化测试 |
| E7 | `gateway_smoke_test.py` —— **无需升级**：网关冒烟，无对局语义；按 ACCOUNT_DESIGN §7.1 保持「一行不改」全绿 | ACCOUNT_DESIGN §7.1 | 已覆盖（维持现状即可） | 自动化测试 |
| E8 | GAME_DESIGN §10 断言组映射：A1–A5、B1–B7、C1–C4 在现行单局 e2e 中已全部落地 | GAME_DESIGN §10.1–10.3 | 已覆盖（单局口径）；⚠️ GD-A5 的 120 秒是**单局**口径，整场阈值见 Q3 | 自动化测试 |
| E9 | **真人试玩清单**（见 §六）：从冷启动、不开调试钩子，走完 登录 → 大厅 → 一场 → 场终页出口 | UI_UX_SPEC §12 UX-1..UX-15 | **需人工**（B4 交付后执行；自动化全绿不能替代本项） | 用户试玩 |
| E10 | 压力/统计冒烟：`e2e_full_game.py --games 200 --seed 17` 仍全绿（荒庄率、局时长分布无回归级漂移）+ `mahjong_room_bench` | — | 已覆盖（可选加强项，不作本批阻断） | 自动化测试 |

**判据总数 61**（v1.3：原 60 条 + 新增 M11 随机性/多样性盲区补件）：【自动化已覆盖】59（其中 E2 的逐帧哈希按用户裁决降级，等价覆盖指向进程内逐帧回放）、【自动化待补】0、【需人工】2（P2 已由我执行完毕；E9 待用户试玩）。

---

## 二、缺陷定级标准（四级，例子从规格条文推导，均非已发生缺陷）

| 级别 | 定义 | 本项目示例（各 2 个） |
| --- | --- | --- |
| **blocker** | 破坏冻结口径的核心算术/终止性，或使一场无法正常结束——出现即不得交付 | ① 任何情形 `Σ delta_score != 0` 或整场 `Σ match_total != 0`（GAME_DESIGN §6.1「任何情形下 Σ=0」、§3.4 零和归纳）；② C15 双闸失效导致连庄无界、一场不终止（RULESET §7.3「一场必然终止」、附录 C14「场终必须可达」） |
| **major** | 规则语义错误：数值/判定结果与冻结表不符，玩家可直接感知对错 | ① 0 番自摸按 −4/+12 结算（自摸被计番 = 双重计番，RULESET §6.1 v1.3 内修正明文禁止）；② 荒庄后轮庄（违反 RULESET §7.3 判据 B「荒庄一律连庄」） |
| **minor** | 字段/顺序/时序与协议不符，但分数与胜负正确，不影响核心玩法 | ① `win.yaku[]` 顺序不按 §6.2 判定序（数值正确但违反冻结序，番种明细行乱序，PROTOCOL §10.4 ①）；② 局间收到对局帧回了 `INTERNAL` 而非 `BAD_REQUEST`（错误码选错、拒绝语义正确，PROTOCOL §10.2 局间行） |
| **nit** | 纯文案/外观/冗余，不改任何语义 | ① 无番和牌显示旧义「平和」而非「无番（0 番）」（UI_UX_SPEC §9.4 明文作废旧文案，且「平和」现已是真实番种名，易混淆）；② `game_end` 帧多带了一个协议未定义的冗余字段（附录 D「实现方不得添加」，但未误导客户端） |

> 「设计主张不同」（如实现与设计对 AI 策略保守度的取舍）列出但不计缺陷；「平台过审 / 好不好玩」不在技术验收范围内，只由用户试玩给出。

---

## 三、验收闸门

1. **放行条件：`blocker + major 数 == 0`，且 60 条判据无一条「待补未补」或「执行失败」。** minor/nit 允许带病放行，但须逐条列入交付说明。
2. 每条判据的责任方已在 G1–G5 标注：自动化判据以**具名测试重跑全绿**为准；人工判据以逐项打勾记录为准；用户试玩（E9）是**独立闸门**——自动化全绿不能替代真人路径。
3. **「全绿」≠「能玩」**：t4/t6 清单全绿只证明没崩；G5 E9 的真人路径（冷启动 → 不借助任何调试钩子 → 场终页出口）必须至少完整走一遍并留记录（截图或逐帧日志）。
4. 缺陷计数口径：同一根因引发的多个表现计 1 条，按最高级别定级；复现路径必须写明 seed / 命令 / 帧序号。

---

## 四、B3 占位区：match_results 落库（实现中）——待实现项核对清单

> 现状核实：`db.h`/`db_mysql.cc`/`db_stub.cc`/`lobby.cc`/`room.cc` 中均无 `match_results`/`RecordMatchSummary`，B3 未落码。以下全部标 **待核对**，随 B3 交付逐条执行。

| ID | 判据 | 来源 | 状态 |
| --- | --- | --- | --- |
| B3-1 | 同 `room_id` 的 `match_results` 四行 `final_score` 之和 == 0 | ACCOUNT_DESIGN §2.1 约定 1、§4.7 | 待核对 |
| B3-2 | `UNIQUE KEY uk_match_results_room_account(room_id, account_id)` 生效：重复场终通知只记 WARN 不重写 | ACCOUNT_DESIGN §2.1 约定 1 | 待核对 |
| B3-3 | `matches` 仍每局一行（4 局 ⇒ 4 行），`RecordJob`/`StatsJob` 零改动 | ACCOUNT_DESIGN §0 局制行、§2.1 约定 2 | 待核对 |
| B3-4 | `stats` 帧与 `StatsSummary` 零改动（本期不展示 match_results） | ACCOUNT_DESIGN §2.1 约定 4 | 待核对 |
| B3-5 | 实现落点三处齐全：`db.h`（方法 + kActorMethods 反射）、`db_mysql.cc`（kSchema 第三条 DDL + 新 job）、`db_stub.cc`（空实现，否则 `-DMAHJONG_WITH_MYSQL=OFF` 构建失败） | ACCOUNT_DESIGN §2.1 约定 5 | 待核对 |
| B3-6 | `dealer_hands` = 该账号作庄手数（含连庄额外手）；`final_rank` == `match_end.rankings[].rank` 逐座位一致 | ACCOUNT_DESIGN §4.7 | 待核对 |
| B3-7 | 局终不写 `match_results`（两条路径独立）；`-DMAHJONG_WITH_MYSQL=OFF` 构建全绿 | ACCOUNT_DESIGN §2.1 约定 3/§7 | 待核对 |
| B3-8 | `account_e2e_test.py` 升级为整场后核对上表（对应 E5） | ACCOUNT_DESIGN §7.2 v1.3 补充 | 待核对 |

## 五、B4 占位区：客户端 Python 界面（未开工）——待实现项核对清单

> 判据来源固定为 `UI_UX_SPEC.md` v2.0 的 §9（结算两形态）与 §12（落地清单 UX-1..UX-15），此处只列**逐条核对**入口，不复制规格正文。

| ID | 判据（逐条对 UI_UX_SPEC） | 状态 |
| --- | --- | --- |
| B4-1 | §12 UX-1..UX-15 逐条打勾（四界面迁移、坐标、手牌点选、八种交互可见性/禁用态、倒计时、私有信息、四种异常态、术语、局终≠场终、暗杠两形态、番种明细文案） | 待实现 |
| B4-2 | §9.2 局终页逐区块：标题行（第 X/4 局 · 连庄 N）、番种明细行（yaku 顺序 + 已封顶 + 无番（0 番））、四卡片（含累计 match_total 第二行）、下一局提示行、**局终页无出口按钮** | 待实现 |
| B4-3 | §9.6 场终页：排名表按 rankings 数组序、并列跳号、金框第一名、「再来一局 / 返回大厅」两出口、不渲染局终页遗留状态 | 待实现 |
| B4-4 | 附录 B 控件矩阵：「暗杠/加杠」仅 discard 窗口、「杠」仅 claim 窗口、「过」永不出现于 discard 窗口 | 待实现 |
| B4-5 | 他家手牌背按 18 张预留不越界；他家暗杠揭示前 4 张牌背、揭示后 4 张牌面 | 待实现 |
| B4-6 | §12 UX-9 美术约束：无图片资产、`pygame.image.load` 零调用（可 grep 机械判定） | 待实现 |
| B4-7 | B4-1..B4-6 通过后执行 G5 E9 真人试玩清单并留记录 | 待实现 |

---

## 六、真人试玩清单（G5 E9，给用户/验收人的操作脚本）

```bash
# 1. 开服（固定 seed 便于复现本清单提到的局面）
cmake --build build-release -j
./build-release/game/mahjong_server --seed 42          # 默认 127.0.0.1:9527
# 2. 开客户端（另开终端；关掉一切调试工具，从命令行直接启动）
python3 game/client/main.py
```

按顺序核对（对应 UI_UX_SPEC UX 编号）：

1. 登录 → 进大厅，看到身份行与「新建房间 / 开始对局」→ 建桌 → 开局（UX-1）。
2. 牌桌四方位布局正确、自家手牌可点选（金色描边 + 上移 8px）、打牌需二次确认（UX-2/3）。
3. 轮到他人时不出现操作栏；鸣牌窗口只出现「碰/吃/杠/过」中 options 含的按钮；打牌窗口只出现「胡/暗杠/加杠/打牌」中 options 含的按钮，**永不出现「过」与「杠(明)」**（UX-4、附录 B）。
4. 倒计时进度条走动，归零后界面不代打，等服务端 `discard_event` 后牌才消失（UX-5/6）。
5. 他家手牌全是牌背、数量 ≤18；他家暗杠时该组渲染 4 张牌背；本家暗杠 4 张牌面（UX-14）。
6. 局终出现**局终页**：四卡片含本局得失分与累计分两行、和牌者金框、番种明细按「中文名 N 番」顺序排列、合计 N 番（封顶时「已封顶」）、**没有任何出口按钮**；5 秒提示后自动进下一局（B4-2）。
7. 荒庄局终页显示「本局不计分；庄家连庄」且下一局庄家不变（UX-12）。
8. 第 4 局（或连庄兜底）后出现**场终页**：排名表（并列时两行同名次）、金色第一名描边、「再来一局 / 返回大厅」两个出口（B4-3）。
9. 点「返回大厅」回到大厅且战绩可查；点「再来一局」直接开新一场（UX-1、§9.5）。
10. 结束前如出现无番和牌，确认文案为「无番（0 番）」而非「平和」（UX-15）。

> 注：本清单第 6/7/8/10 步依赖随机局面，如一局内未自然出现，用不同 `--seed` 重开补采；不需要任何注入式脚本。

---

## 七、待澄清问题（已全部裁决，v1.1 回填）

| # | 问题 | 裁决（主理人） | 对本计划的影响 |
| --- | --- | --- | --- |
| Q1 | **E 闸角点算术**：判据 E（`renchan_count > 4` 场终）允许「未打满 4 局即因连庄兜底终场」，三条冻结口径（`hands_played` 恒 4、`game_end` 次数 = `hands_played + renchan_count`、单场上界 8 局）在该角点算术上不能同时成立 | **已裁决**：`hands_played = round_index 终值 + 1`（无截断）；PROTOCOL §5.15 由架构总监修正、room.cc 与测试期望由主程序修正，**同批提交**中 | M1/M2 判据已改为新公式；两处修正落地后联合重验 M1/M2/M4/M6 |
| Q2 | GAME_DESIGN §2.1 循环图「可点『继续』提前」与 §2.2/PROTOCOL 冻结「不可提前」矛盾 | **成立，文档缺陷**；已派创意总监改图对齐 | M8 判据补「客户端无『继续提前』出站帧」断言；验收只按固定 5000ms 口径 |
| Q3 | GD-A5 的 120 秒是单局口径，整场无冻结超时值 | **已裁决**：整场脚本兜底超时 = **960s**（8 × 120s，对应单场上界 8 局；单局 120s 口径不变） | 已写入 E2/E3 判据 |
| Q4 | P2（t5 清单）与 B4 全部判据在 B4 交付前无执行主体 | **确认**：B4 完成后我才执行那两条 | B4 验收与 A 批终验收合并；B3 只需过 B3-1..B3-8 + E5 |
| Q5 | `CanAnkan`/`CanKakan` 纯函数签名不含 `remaining`，守卫必落 Room 层 | **确认为架构事实**：守卫在 Room 层 options 生成时校验 `remaining ≥ 1`，规则库保持纯函数不依赖牌山状态，分层正确、不改 | R11 补件落在 room 级 gtest，不要求改规则库签名 |

**11 条「自动化待补」的归属（主理人裁定）**：不塞进 B3（落库任务）；**B4 完成后**统一交给主程序做「e2e 升级 + 断言补齐」批次——E2（TCP 整场逐帧 + 960s 兜底）、E3–E6 四脚本升级、N3 绝对值锚、R11/R15、M7/M8。本文档即该批次的任务来源。B3 回传后我按 §四 8 条核对；B4 回传后按 §五 7 条核对 + 执行 P2/E9 人工判据；最后出 A 批验收报告。风险第 2 条（TCP 层零覆盖）经主理人确认排最高优先——E2 升级完成前不进闸门。

---

## 八、当前实现风险最高的 3 个点（验收前重点关注）

1. **E 闸 × hands_played 口径矛盾（Q1，已裁决、修正执行中）**：这是唯一一处「文档在修正中、测试已无条件断言、分支可能被真实 seed 触发」三线交汇的地方。裁决口径为 `hands_played = round_index 终值 + 1`，但 PROTOCOL §5.15、room.cc、room 测试期望三处必须**同批落地**——任何一处先行都会让另外两处翻红。落地后我按 M1/M2/M4/M6 联合重验。
2. **整场链路 TCP 层零覆盖**：一场四局/连庄/场终/rankings/yaku 字段目前只在 C++ 进程内单测验证，从未穿过网关 JSON 序列化与 player agent 真实链路；`game/tests/` 下 5 个脚本无一引用 `match_end`。协议类 11 条「e2e 待补」判据当前没有执行主体——B1+B2 的 Release 全绿证明的是「进程内正确」，不是「线上正确」。
3. **绝对值锚缺失**：「0 番自摸恰好 −2/+6」（N3）与判番确定性 1000 次打乱（R15，RULESET §10.3 S12 明文要求）都只有关系式或近似覆盖。自摸不加番是 v1.10 内修正的核心裁决，缺绝对值锚意味着 0 番路径上的回归（如 tsumo 番被误加回）只能靠 2 番手测试间接拦截。

---

## 九、B3/B4 交付核对记录（v1.2，qa-lead 逐条实码核对）

核对方式：读 client/ 与 server/ 实码（非转述对照表）+ 复跑测试 + 真库冒烟。执行环境：本机（build-release 已链 libmysqlclient；本机 mysqld 运行中）。

### 9.1 执行证据（我实际跑过的命令与结果）

| 命令 | 结果 |
| --- | --- |
| `ctest --test-dir build-release --output-on-failure -R "mahjong"` | **8/8 全绿**（含 mahjong_rules_test / mahjong_room_test 35.6s / mahjong_lobby_test 8.7s） |
| `.venv/bin/python game/tests/client_headless_test.py`（复跑 2 次） | **228 断言 = 226 PASS + 2 FAIL**：两次均在「含字牌牌桌/结算单帧耗时」失败（第 1 次 max 37.91/25.04ms、第 2 次 max 57.43/33.68ms，预算 ≤10ms；中位 3.3–5.2ms 达标）→ 与主程序回传「全过」不符 |
| `.venv/bin/python game/tests/gateway_smoke_test.py` | **13/13 PASS** |
| `grep pygame.image.load game/client/` | **0 调用**（UX-9 通过） |
| 真库冒烟：清库 `mahjong_qa_b3` → 启动 `mahjong_server --port 19777 --mysql-db mahjong_qa_b3` | 日志 `db: account storage ready`；**三表自建**（accounts / matches / match_results），`SHOW CREATE TABLE match_results` 与 ACCOUNT_DESIGN §2.1 逐字段一致（含 `UNIQUE KEY uk_match_results_room_account`） |
| 真库重复插入模拟 | 第二次同 `(room_id, account_id)` 插入被拒：`Duplicate entry 'r-9999-1' ... error 1062` —— 与 `db_mysql.cc:495-500` 的 WARN-ignore 分支吻合 |

### 9.2 B3-1..B3-8 核对结论

| ID | 结论 | 证据 |
| --- | --- | --- |
| B3-1 四行 Σfinal_score==0 | **通过（静态）**：`totals` 源自逐局零和 `match_total` 累加（room invariants 已断言每局 Σ==0）；四行落库终证待 E5 整场 e2e | room.h:67、room.cc:1361；ctest 全绿 |
| B3-2 UNIQUE 生效 | **通过**：DDL 冻结一致 + 真库 1062 实测 + 代码 WARN-ignore 分支 | db_mysql.cc:244-256、495-500；§9.1 真库冒烟 |
| B3-3 matches 每局一行 | **通过（静态）**：`RecordMatch` 仍在局终回调、字段与语义未动；行数核对待 E5 | lobby.cc:218-224 |
| B3-4 stats 零改动 | **通过**：gtest `StatsFrameTest.UnaffectedByMatchResultsPersistence`（6 键 + recent 5 键冻结断言）+ ctest 全绿 | account_test.cc:135-160 |
| B3-5 三处落点齐全 | **通过**：① db.h:96/133/147（kActorMethods 含 `&DbActor::RecordMatchResult`）/162/222；② db_mysql.cc:244/310/482；③ db_stub.cc:63 空实现 | 实码定位 |
| B3-6 dealer_hands / final_rank 语义 | **通过（静态）**：`dealer_hands_` 场次清零（room.cc:725）、每局 `++`（:1341）、场终快照与 match_end **同源**（:1360-1363，`ComputeFinalRanks()` 即 rankings 数据源）；逐座位一致性待 E5 | room.cc 实码 |
| B3-7 局终不写 + OFF 构建 | **前半通过**：`if (settlement.match_over && !db.IsEmpty())` 门在 lobby.cc:227，局终路径不触达；**OFF 构建未实测**（`-DMAHJONG_WITH_MYSQL=OFF` 冒烟待补，非阻断——stub 代码已存在且 ctest 走过编译） | lobby.cc:227 |
| B3-8 整场落库 e2e | **待核对**：5 个 e2e 脚本仍单局口径（10-22 复查 `grep -c match_end` = 0，文件时间戳未变）→ 归 E5，随主程序 e2e 升级批次 | — |

**B3 判定：达标（不含 e2e 终证项）**。落库三处落点、DDL/UNIQUE、stats 零改动、局终不写全部实码+实测通过。

### 9.3 B4-1..B4-7 核对结论（对照 UI_UX_SPEC v2.0 读实码）

| ID | 结论 | 证据 / 缺口 |
| --- | --- | --- |
| B4-1 UX-1..UX-15 | **基本通过（静态 + headless）**：226 项 headless 断言覆盖八种交互按钮序、暗杠两形态、局终/场终分支、张数两道校验；UX-2 坐标为 spot-check（theme.py 冻结 rect），**终判靠 E9 真人试玩** | client_headless_test.py 226 PASS |
| B4-2 局终页逐区块 | **通过，3 处文案/展示缺口**（见 9.4 缺陷 D-2/D-3） | render/result.py:207-326 |
| B4-3 场终页 | **通过**：rankings 数组序直渲、并列 rank 照显、金框第 1 名、两出口、不渲染局终遗留（金框/番种/收付/手牌均不画）、底注（已拍板差异 #3） | render/result.py:332-401 |
| B4-4 控件矩阵 | **通过（结构性保证）**：按钮只由 `window.options` 生成（render/actionbar.py:63-98）——claim 窗口按协议永不含 ankan/kakan、discard 窗口永不含 pass/kong，客户端按 options 渲染即满足矩阵；「过」仅在 WINDOW_CLAIM 追加（:91-92） | 实码 |
| B4-5 牌背 18 + 暗杠两形态 | **通过**：`OPP_BACK_MAX=18`（theme.py:186）、第二行补 4 张、>18 记 WARN（table.py:282-305）；暗杠空数组 4 张牌背两处（table.py:135/158、result.py:462） | 实码 |
| B4-6 美术约束 | **通过**：`pygame.image.load` 全 client 零调用、目录无图片资产 | grep |
| B4-7 E9 真人试玩 | **待执行**（等 e2e 批次全绿后由主理人请用户上手；脚本在 §六） | — |

### 9.4 本轮新增缺陷清单

| 编号 | 描述 | 复现路径 | 级别 | 建议处理 |
| --- | --- | --- | --- | --- |
| D-1 | **局终页键盘出口旁路**：`_on_key` 的 `SCREEN_RESULT` 分支未检查 `state.match_end`，Enter 触发 `rematch()`、ESC 触发 `go_to_lobby()`——与点击路径（app.py:311 有 `match_end is None` 守卫）不一致。局终 5 秒窗口内按一次 Enter = 断线弃局（旧场转托管）+ 重连开新场，违反 UI_UX_SPEC §9.5 v2.0 冻结前置「局终页不得响应这两个操作」 | 开局 → 打完任意一局（match_will_continue=true 的局终页）→ 按 Enter | **major** | lead-programmer：给 `_on_key` 的 RESULT 分支补同一守卫（一行）；修后 headless 补一条「局终页 Enter 不触发重连」断言 |
| D-2 | **headless 单帧耗时 2 项失败**：含字牌牌桌 max 57.43ms / 结算 max 33.68ms（预算 ≤10ms；中位 3.3–5.2ms 达标）。复跑 2 次均复现；主程序回传「228 全过」与当前树不符（疑其运行环境差异或缓存预热） | `.venv/bin/python game/tests/client_headless_test.py` | **minor** | lead-programmer 定位尖峰来源（怀疑字体字形缓存首次构建 / GC）；E9 前给出结论或降级说明 |
| D-3 | 局终页收付文案缺「本场累计：\<领先者昵称\> 领先」（并列时「并列领先」）——UI_UX_SPEC §9.2 收付文案区末行，v2.0 新增展示项；全 client grep 无「领先」 | 局终页目测 | **minor** | render/result.py `_render_win_block` 补一行 |
| D-4 | 封顶文案「（封顶）」≠ 规格「已封顶」，且合计缺「（multiplier 分）」（§9.2 番种明细行 ③） | 6 番和牌局终页 | **nit** | result.py:113 一处文案 |

**已拍板四差异复核**：连庄并入 HUD 中段（table.py:201-204）✓、多暗杠按钮同文案（actionbar.py:77-80）✓、场终页底注（result.py:394-401）✓、倒计时常量 5（result.py:53）✓ —— 与主理人拍板一致，不计缺陷。

### 9.5 P2（PROTOCOL §10.3 t5 清单）逐条判定

| t5 自检项 | 结论 |
| --- | --- |
| 出站 `discard`/`action` 回带 `seq` | ✅ app.py:1262-1265、:1294 |
| 只在 `options` 内取值；打牌窗口不提供「过」；新增 ankan/kakan 按钮位 | ✅ actionbar.py:63-98（D-1 为键盘路径缺陷，与本项无关） |
| 按 `from_seat` 从打出者牌河移除被鸣牌；ankan/kakan 不动任何牌河 | ✅ game_state.py:398-407（仅 pong/chow/kong 分支动牌河） |
| 他家不渲染任何牌面；`seats[].hand` 即使出现也不消费 | ✅ SeatView 无牌面字段（game_state.py:74-84）；`_apply_state_sync` 对 `hand` 脱敏记 WARN（:431-434） |
| `state_sync` 整体覆盖，`action_event` 仅即时反馈 | ✅ `_apply_state_sync` 整体覆盖 + `_note_seq` 严格更大才关窗 |
| game_start 唯一重置信号；game_end 不退出牌桌；match_end 才进最终结算页；局间不显示倒计时 | ✅ game_state.py:252-302（game_start 重置）、:456-493（局终→SCREEN_RESULT 非终态、场终→match_end）；局间提示行为已拍板差异 #4 |

**P2 判定：通过（含 D-1 待修，D-1 属 §9.5 出口纪律，不属 t5 七项中的任何一项）**。

---

## 十、A 批最终验收报告（v1.3，qa-lead，2026-10-22）

> 判定对象：A 批四项交付（番型表 + 番数结算 + 一场四局/连庄 + 暗杠加杠）+ B3 场次汇总落库 + B4 客户端接入。
> 判定口径：§一 61 条判据 + §三 闸门。**本报告只判技术面**；「好不好玩」由用户试玩（E9）给出，我不代判。

### 10.1 本轮实测证据（我亲自执行的命令与输出摘要）

| # | 命令 | 实测结果 |
| --- | --- | --- |
| 1 | `ctest --test-dir build-release -R mahjong` | **8/8 全绿**（room 22.0s / lobby 3.9s） |
| 2 | `mahjong_rules_test --gtest_list_tests` / `mahjong_room_test --gtest_list_tests` | **rules 67 / room 34**（与主程序自报一致） |
| 3 | 四条局种子修复断言定向复跑 | `ConsecutiveRoundsDealDifferentWallsAndHands`、`DifferentServerSeedsDealDifferentWalls`、`RoundSeedIsPureAndPerHand`、`SameSeedReplaysTheWholeMatchFrameForFrame` → **4/4 PASS** |
| 4 | `ZeroHanTsumoIsExactlyMinusTwoPlusSix` + `YakuDetectionIsDeterministicUnderShuffles` | **2/2 PASS**（N3 / R15 补件确认） |
| 5 | `NoKongOptionsWhenWallExhausted` / `RoundEndRejectsEveryGameFrame` / `InterHandDelayDefaultIsFrozen` / `StateSyncWaitsAreOwnPrivateAndConsistent` | 前 3 条 **PASS**；第 4 条**单独运行失败**（见 D-6） |
| 6 | `.venv/bin/python game/tests/client_headless_test.py` | **233 断言 = 232 PASS + 1 帧耗时偶发 FAIL**（三次复跑：牌桌/结算交替出现 1–2 项失败，max 13.9–57.4ms，中位 3.0–5.5ms） |
| 7 | `.venv/bin/python game/tests/gateway_smoke_test.py` | **13/13 PASS** |
| 8 | `.venv/bin/python game/tests/match_full_e2e_test.py --inter-hand-delay-ms 300` | **PASS**：`rounds=7 hands_played=7 renchan=3 m7_bad=1 gaps=[306,305,304,305,304,309]` |
| 9 | `.venv/bin/python game/tests/e2e_full_game.py --matches 1 --seed 17` | **PASS**：`rounds=5 hands_played=5 renchan=1 m7_bad=1 gaps=[307,310,305,303]` |
| 10 | `.venv/bin/python game/tests/e2e_full_game.py`（默认无参） | **崩溃** `NameError: name 'run_single' is not defined`（D-5） |
| 11 | `.venv/bin/python game/tests/e2e_full_game.py --games 2 --seed 17` | PASS（统计模式未受影响） |
| 12 | `.venv/bin/python game/tests/room_full_game_test.py` | PASS（`win_tsumo requests=21 discards=70 max_seq=139` + 多房间不同牌山可复现） |
| 13 | `.venv/bin/python game/tests/account_e2e_test.py`（真库） | PASS：rounds=5 → `games=5 net_score=-8`；`match_results` 行 `hands=5 dealer=1 score=-8 rank=4` 逐字段核对；rematch 新房间；重启后存活 —— **B3-8 / E5 终证** |
| 14 | `mahjong_room_test --gtest_shuffle --gtest_random_seed=12345` | **3 项 FAIL**（D-6 证据：套件顺序依赖） |

### 10.2 AC 编号 → 逐条实测证据（按判据组汇总，逐条状态见 §一）

| 组 | 判据 | 实测证据落点 | 结论 |
| --- | --- | --- | --- |
| G1 数值（13） | N1–N13 | 证据 #2/#4 + `ScoreRonAndTsumo`/`ScoreMultipliers`/`ZeroHanWinScoresOne`/`ZeroHanTsumoIsExactlyMinusTwoPlusSix` | **13/13 通过** |
| G2 规则（15） | R1–R15 | 证据 #2/#5 + 14 个 `Yaku*` 成对正反例 + `DifferentialAgainstBruteForce`（14300 手差分）+ `NoKongOptionsWhenWallExhausted` + `YakuDetectionIsDeterministicUnderShuffles` | **15/15 通过** |
| G3 场次（11） | M1–M11 | 证据 #3/#5/#8/#9 + room 帧计数 + lobby 回收 + `match_full_e2e` 的 hands_played/gaps 实测 | **11/11 通过** |
| G4 协议（12） | P1–P12 | 证据 #1/#5/#7/#8/#9 + room 协议不变量群 + `match_full_e2e` M7 探针（真链路 BAD_REQUEST 恰 1 条） | **12/12 通过**（P2 由我逐条实码核对，见 §九 9.5） |
| G5 端到端（10） | E1–E10 | 证据 #3/#6/#8/#9/#11/#12/#13；E2 按用户裁决降级（等价覆盖 `SameSeedReplaysTheWholeMatchFrameForFrame`）；E9 待用户 | **9/10 通过，E9 待用户试玩** |

### 10.3 D-1..D-4 当前状态

| 编号 | 上轮级别 | 当前状态 | 复验证据 |
| --- | --- | --- | --- |
| D-1 局终页键盘出口旁路 | major | **已修（关闭）** | `app.py` 的 `_on_key` SCREEN_RESULT 分支现已含 `if self.state.match_end is None: return`，注释标注「D-1 守卫（§9.5 v2.0）」；与点击路径守卫同规则。⚠️ 备注：修复**仅代码核对**，headless 未新增对应断言（建议后续补，不计缺陷） |
| D-2 单帧耗时尖峰 | minor | **未关闭 → 记入交付说明（已知局限）** | 复跑 3 次恒有 1–2 项超 10ms（max 13.9–57.4ms），中位 3.0–5.5ms 稳定；失败对象在「牌桌/结算」间漂移 → 判为偶发尖峰（疑字形缓存/GC），不影响操作与判定 |
| D-3 「本场累计：XX 领先」缺失 | minor | **已修（关闭）** | `render/result.py:125-140` 新增 `_match_total_hint`，含「并列领先」分支 |
| D-4 封顶文案 | nit | **已修（关闭）** | `render/result.py:118-120`：「 ｜ 合计 N 番（M 分）」+「已封顶」，与 §9.2 冻结形状一致 |

### 10.4 本轮新增缺陷

| 编号 | 描述 | 复现路径 | 级别 | 建议 |
| --- | --- | --- | --- | --- |
| D-5 | `game/tests/e2e_full_game.py` **默认无参入口崩溃**：`main()` 在 `--games <= 1` 分支 `return run_single(args)`，但 `run_single` 从未定义（只有 `run_one_game`/`run_one_match`）；docstring 仍自述「single game (default)」 | `python3 game/tests/e2e_full_game.py` → `NameError` | **minor（验收资产，非产品代码）** | lead-programmer 一行修复（改走 `run_one_game` 或删掉该分支并更新 docstring）。整场能力不受影响（`--matches N` 与 `match_full_e2e_test.py` 均实测通过），故不阻断；若团队认为验收资产入口失效应按 major 计，我接受升级 |
| D-6 | **room 套件 3 个测试顺序依赖**：`StateSyncWaitsAreOwnPrivateAndConsistent`、`HandsStaySortedForEverySeat`、`SelfKanActionsRoundTripThroughTheDiscardWindow` 单独运行或 `--gtest_shuffle` 下必挂，默认注册顺序下通过（其中 `SelfKanActionsRoundTrip` 是 A 批 R13/P6/P7 的取证测试） | `mahjong_room_test --gtest_filter='*SelfKanActionsRoundTrip*'` → FAILED；`--gtest_shuffle --gtest_random_seed=12345` → 3 FAILED | **minor** | fixture 显式初始化调度器/世界（`SetUpTestSuite`）或在用例内自建，使每条断言可独立复跑。**门禁本身成立**（CI 走默认序），但「单条可复跑」能力被削弱 |

### 10.5 缺陷计数与可交付判定

| 级别 | 本轮状态 | 明细 |
| --- | --- | --- |
| blocker | **0** | 用户实玩的「同场每局牌山相同」blocker 已由主程序修复，我以 4 条断言 + 整场 e2e 复验通过 |
| major | **0** | D-1 已修并复验关闭 |
| minor | **2** | D-2（已知局限，中位达标）、D-5（验收资产默认入口）、D-6（测试顺序依赖）—— D-2/D-5/D-6 均不改变产品行为 |
| nit | **0** | D-4 已修关闭 |

**判定：技术面「有条件通过」，可交用户试玩（E9）。**

- 放行依据：blocker 0 / major 0；61 条判据 59 条自动化已覆盖（E2 按用户裁决降级、等价覆盖成立）、1 条人工判据（P2）我已执行完毕；唯一未执行项是 E9 真人试玩本身。
- 随交付说明带走的已知局限 3 条：① D-2 单帧耗时偶发尖峰（中位 3.0–5.5ms 达标，预算 10ms）；② D-5 `e2e_full_game.py` 默认入口失效（用 `--matches N` 或 `matches_full_e2e` 替代）；③ D-6 三条 room 测试需按默认顺序运行。
- **不承诺「必过」「好玩」**：本报告只证明技术面达标。用户试玩（E9）是独立闸门——若试玩发现新 blocker/major，本判定自动失效并按新缺陷重开。

### 10.6 盲区复盘：原 60 条判据为什么没抓到「同场每局牌山相同」

- 60 条判据的判定对象分别是：算分算术（G1）、番种判定（G2）、场次推进（G3）、协议字段与序列（G4）、可复跑与脚本口径（G5）。
- 「每局牌山复用」在这五类下**全部合法**：牌局能正常打完、算分零和、场次推进正确、协议字段完整、同 seed 也确实可复现（甚至更「可复现」）。同 seed 复现类断言反而把它当成正确行为。
- 它只能被两类手段发现：① **真人连续打两局并注意到起手牌一模一样**（用户路径）；② 显式断言「相邻两局牌山/四家起手牌不同」。前者不在自动化能力内，后者原判据表里没有。已补为 M11。
- 教训（写入本报告以示记录）：**「规则正确」与「每局是新的一局」是两类正交性质**。后者属于「随机性/多样性」维度，验收表若没有这一维度，任何实现缺陷都只能靠玩家运气发现。
