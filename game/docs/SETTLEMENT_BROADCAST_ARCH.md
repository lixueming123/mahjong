# 结算播报（字幕 + 语音）技术架构裁定书

> **版本** v1.0 · **作者** tech-architect（柯桥良） · **状态** 待主程序实现 / 待主理人确认 2 项未决
> **上游需求**：用户已拍板「播报 = 字幕 + 语音」「推导 = 分区静态 + 逐条高亮」「播放播报时暂停下一局倒计时」
> **本文性质**：**只做裁定与规格，不含实现代码、不改任何源码**。文中所有源码行号均为裁定当日实测。

---

## 0. 结论摘要

| # | 议题 | 裁定 | 一句话 |
|---|---|---|---|
| A | 番数口径自相矛盾 | **实现权威，改文档** | 线上 `han = min(Σ yaku.han + 庄家, 6)`，**自摸不计番**，`han_breakdown.tsumo` **恒 0**；错的是 `PROTOCOL.md` §5.10 的三处且必须改 |
| B | 播报期间暂停下一局 | **方案 A：服务端延长窗口 + `game_end` 携带 `interlude_ms`** | 客户端只做「显示暂停」，**时间永不暂停**；服务端窗口是唯一时钟，客户端无法延长 ⇒ 不存在房间卡死 |
| C | 播报状态机放哪 | **新建 `client/broadcast.py`** | `result.py` 只画不写（守住 `UI_UX_SPEC §11`）；`app.py` 持实例并喂 `UiView` |
| D | 与既有冻结条款的冲突 | **3 条，必须显式登记** | `PROTOCOL §5.10` 公式、`UI_UX_SPEC §9.2` 「N 由 5 递减」、`FEEL_SPEC §5.2` 第 3 条「禁用项：不得出现人声」 |

> ⚠️ **先纠正一处编号**：任务里写「§5.15 `game_end` / §5.16 `match_end`」，与文档现状不符。实测：**`game_end` = §5.10**（`PROTOCOL.md:580`）、**`match_end` = §5.15**（`PROTOCOL.md:714`）、**`tenpai` = §5.16**（`PROTOCOL.md:743`）。本文件全部按实测编号写；若按 §5.15 去改 `game_end`，会**改到 `match_end` 章节**。

---

## 1. 前提假设（假设变了，结论要重估）

| # | 假设 | 若不成立的影响 |
|---|---|---|
| P1 | 玩家构成是 **1 真人 + 3 AI**（`player_agent.h` 里的 bot 座位由服务端驱动，AI 没有客户端进程） | 若改为 4 真人联机，方案 A 的「跳过空等 ≤5 s」会 ×4 人次，方案 B 才可能变得划算（见 §3.2） |
| P2 | 客户端与服务端在**同一台机器**或局域网（`start_server.sh` / `start_client.sh` 本地启动，RTT < 50 ms） | 若跨公网（RTT > 300 ms），`BC_TAIL_RESERVE_MS = 500` 不够，竞态兜底会真的被触发（见 §3.6 第 1 行） |
| P3 | 语音资产是**离线固定短语**，本项目不引入联网 TTS（`FEEL_SPEC §5.3 D4`：零收集、纯本地、不得引入需联网校验的素材） | 若允许联网 TTS，可动态念数字，但 P3 一破就等于推翻 `FEEL_SPEC D4`，需主理人重新裁决 |
| P4 | 不存在需要兼容的**旧客户端分发现场**（本地启动，客户端与服务端同仓同版本） | 若存在旧客户端，§3.8 的上线顺序必须改为「同时上线」 |
| P5 | 用户接受「**显示暂停**」而非「**时间暂停**」 | 若用户坚持时间真的暂停，只剩方案 B，且要解冻 `UI_UX_SPEC §9.5` 的「局间不得发帧」前置说明（见 §3.2） |

---

## 2. 议题 A 裁定书：番数口径自相矛盾

### 2.1 事实核对（源码 + 测试，四处独立证据）

**证据 1 — 算分实现**：`server/mahjong_rules.cc:1371-1377`

```cpp
// han = 番种合计 + 庄家(1)，受**同一**封顶（B1.1 裁决：自摸不是番种，不计入合计；
// "自摸 ×2" 已并入下方支付公式的 2R/6R）
int han = YakuHanTotal(result.yaku) + result.dealer_bonus;
if (han > kHanCap) han = kHanCap;      // kHanCap = 6（mahjong_rules.cc:1344）
result.han = han;
```

⇒ **`han` 与 `ctx.Tsumo()` 无关**。自摸只影响 `per_payer` / `total`（`mahjong_rules.cc:1379-1395`），不影响 `han`。

**证据 2 — 线协议序列化**：`server/room.cc:741-751`

```cpp
// 番数四段式（B1.1 裁决）：base = Σ yaku.han；tsumo 恒 0（自摸不是番种，
// "自摸 ×2" 已并入支付公式的 2R/6R）；dealer = 庄家加成；total = 封顶后合计。
breakdown.Set("base",   json::Value::Int(base));                    // 745-746
breakdown.Set("tsumo",  json::Value::Int(0));                       // 747 ← 硬编码 0
breakdown.Set("dealer", json::Value::Int(settlement.dealer_bonus)); // 748
breakdown.Set("total",  json::Value::Int(settlement.han));          // 749
```

⇒ **线上 `tsumo` 字段永远是 `0`**，不是 `0/1`。

**证据 3 — C++ 单测**：`server/mahjong_room_test.cc:1513`

```cpp
EXPECT_EQ(static_cast<int>(IntOf(*breakdown, "tsumo")), 0) << "B1.1：自摸不是番种，tsumo 恒 0";
...
EXPECT_EQ(total_han, std::min(base + dealer_bonus, 6));   // 1517
```

**证据 4 — Python e2e**：`tests/room_full_game_test.py:421-427`

```python
check(win["han_breakdown"]["tsumo"] == 0, "han_breakdown.tsumo must be 0 (自摸不是番种, B1.1)")
expected_total = min(base + win["han_breakdown"]["dealer"], 6)
```

**证据 5 — 规则源头文献**：`docs/RULESET.md:377 / 386 / 435`

- `:377` `han_base = Σ 成立番种的番数   // 14 番种，见 §6.4（自摸不是番种）`
- `:386` 「**自摸不加番（v1.3 内修正）**…若再把「自摸」计为番种，0 番自摸会变成每家 −4 / +12…构成双重计番。协议 `han_breakdown.tsumo` **保留但恒 0**。」
- `:435` 「**自摸不是加成、也不是番种**（×2 由 §6.2 支付公式承载；`han_breakdown.tsumo` 恒 0）」

**反面证据（唯一一处写错的）**：`docs/PROTOCOL.md`

| 行 | 原文 | 错在哪 |
|---|---|---|
| 623 | `total` = `min(base + tsumo + dealer, 6)`；`tsumo` = 自摸加成（0/1） | 公式错 + 值域错（实际恒 0，不存在 1） |
| 633 | 「客户端**必须**把"番种明细 + 自摸 + 庄家 = 合计"渲染成一段可加和的式子」 | 指示客户端渲染一个恒 0 的加项，制造「自摸 +0 番」的误读 |
| 653 | 要点②「`han_breakdown.total = min(base+tsumo+dealer, 6)`」 | 同 623 |
| 1097 | 自检清单「`han_breakdown.total == min(base+tsumo+dealer, 6)`」 | 同 623，且被写进「验收自检」 |
| 1315 | 帧表 `han_breakdown{base,tsumo,dealer,total}` | 形状对，未提语义，可保留 |

> 连带污染：`docs/UI_UX_SPEC.md:914`（线框图「自摸 +0 · 庄家 +1」）、`:941`（§9.2 明细行「追加「自摸 +`tsumo` · 庄家 +`dealer`」）、`:1000`（§9.4 展示项）三处都跟着写了「自摸 +N」，与 `client/render/result.py:85` 的实现注释「**自摸 +tsumo 恒 0，恒不显示**」直接冲突。

### 2.2 裁定

1. **权威归属：`RULESET.md §6` + `mahjong_rules.cc` 为实现权威；`PROTOCOL.md` §5.10 在此处是错的，改文档，不动实现。**
   依据：`RULESET.md` §0 明确「番值与判定条件以 `RULESET.md` 番种表为唯一权威」（`PROTOCOL.md:632` 自己也这么写：「**每个 id 的番值与判定条件以 `RULESET.md` 番种表为唯一权威**，本文不重复其数值，避免两处漂移」）。即协议自己声明了它不权威，却在 §5.10 越权写了一个错的公式 —— 这正是它担心的「两处漂移」，只是这次漂的是它自己。
2. **唯一权威口径（写进文档的一句话）**：
   ```
   han_base   = Σ yaku[].han                     // 只含 14 个番种
   dealer     = winner_is_dealer ? 1 : 0         // 身份加成，不是番种
   total      = min(han_base + dealer, 6)        // 封顶 6 番，庄家加成受同一封顶
   R          = 2^total
   自摸       = 不参与番数；只活在支付公式（per_payer = 2R，total = 6R）
   tsumo 字段 = 恒 0，兼容占位，不参与任何计算、不参与任何渲染
   ```
3. **`tsumo` 字段的处置：保留字段名，重写语义，禁止渲染。**
   - **不删**：`PROTOCOL.md:582` 冻结「字段只增不改」，删字段是破坏性变更；且旧客户端可能读它。
   - **不改名**：改名同样是破坏性变更。若主理人坚持要一个「读起来不会误解」的名字，唯一安全路径是 **v-next 新增** `total_derivation{base, dealer, capped, total}`，**原 `han_breakdown` 一字不动**并行一个版本，之后才能弃用。**在 v1.7 内改名一律不批**（理由：改名收益是「文档可读性」，代价是「全部客户端 + 全部测试 + 全部线抓包重跑」，不成比例）。
   - **文档措辞（取代 `PROTOCOL.md:623` 的 `tsumo` 行）**：
     > `tsumo` | int | **恒 `0` 的兼容占位字段（历史遗留）**。自摸**不是番种**，其 ×2 已内建于支付公式（`RULESET.md` §6.2）。此字段**不参与** `total` 计算、**不得**参与任何客户端渲染。保留它仅为形状向后兼容；读到非 0 值即视为**服务端缺陷**，客户端按 0 处理并记 WARN。
   - 这就是「纯展示用途该如何改名以免误读」的答案：**它连展示用途都没有**，正确处置是把它的文档身份从「加成项」降级为「兼容占位」，并在协议里明确写「不得渲染」。任何客户端渲染出「自摸 +0」都算**文档缺陷导致的实现缺陷**。

### 2.3 改动清单（文件 + 章节号 + 符号 + 精确改法）

#### 必须改（阻塞，与播报特性同批）

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
| A-1 | `docs/PROTOCOL.md` | §5.10 字段表 `han_breakdown` 行（`:623`） | `total` 公式改 `min(base + dealer, 6)`；`tsumo` 行整句换成 §2.2 第 3 条给出的措辞 |
| A-2 | `docs/PROTOCOL.md` | `:633` 的 `tsumo` 与庄家说明条 | 删「自摸（自摸 +1）…」；改为「`tsumo` 恒 0、是兼容占位；庄家加成是身份加成不是牌型；客户端渲染可加和式子时**只有** `yaku[] + dealer = total` 三项，**不得**出现自摸项」 |
| A-3 | `docs/PROTOCOL.md` | `:653` 要点② | `han_breakdown.total = min(base+dealer, 6)` |
| A-4 | `docs/PROTOCOL.md` | `:1097` 自检清单② | 同上；并追加一句「`tsumo` 必须为 0」 |
| A-5 | `docs/UI_UX_SPEC.md` | §9.1 线框图 `:914` | 明细行示例「自摸 +0 · 庄家 +1」→「庄家 +1」 |
| A-6 | `docs/UI_UX_SPEC.md` | §9.2 `:941` 番种明细行 | 删「追加「自摸 +`tsumo` · 庄家 +`dealer`」」中的自摸半句；改为「追加「庄家 +`dealer`」（为 0 不显示）」 |
| A-7 | `docs/UI_UX_SPEC.md` | §9.4 `:1000` 展示项行 | `win.han_breakdown{tsumo,dealer,total}` → `{base,dealer,total}`，并注明「`tsumo` 恒 0 不渲染」 |
| A-8 | `docs/UI_UX_SPEC.md` | §9.2 明细行末追加一句 | 「**明细行禁止出现「自摸 +N」字样**（自摸不是番种，`RULESET.md` §6.1；出现即判实现缺陷）」 |

#### 建议改（不阻塞，随文档整理批次）

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
| A-9 | `server/mahjong_rules.h` | `:376` `int han;` 注释 | 已正确（「**不含自摸**」），**不动**。但把 `:363-370` 的公式块补一行显式说明「`han_breakdown.tsumo` 恒 0，仅为形状兼容」——让「读头文件的人」不必再跳到 `room.cc` |
| A-10 | `docs/GAME_DESIGN.md` / `GAME_DESIGN_V2_DRAFT.md` | 含 `han_breakdown` 的段落 | 同步自查，避免第三处漂移（本次未逐行核，**列入主程序自查项**） |

#### 明确不改

| 文件 | 理由 |
|---|---|
| `server/mahjong_rules.cc` / `.h` | 实现与 `RULESET.md` 完全一致，无缺陷 |
| `server/room.cc:741-751` | 同上；注释也写对了 |
| `server/mahjong_room_test.cc:1513,1517` | 断言正确，是证据不是缺陷 |
| `tests/room_full_game_test.py:421-427` | 同上 |
| `client/render/result.py:81-121` `_yaku_detail` | 行为正确（只渲染 dealer、不渲染 tsumo）。**但它有另一个独立缺陷，见 §2.5** |
| `docs/RULESET.md` | 已是权威且正确 |

### 2.4 需要补的测试断言

现有三处（C++ 单测 `:1513/:1517`、Python e2e `:421-427`、`mahjong_rules_test.cc:1720-1753` 的 `BonusTsumoAndDealerBonus`）**已充分证明自摸不计番**，不需要重补。真正缺的是两类**新**断言：

**① 边界交叉用例（真空白，且是最锐利的判别器）** —— `server/mahjong_rules_test.cc` 新增：

```cpp
// 0 番 × 庄家 × 自摸：三个口径的交叉点。若把自摸算进番数，R 会翻倍 ⇒ 本断言必挂。
TEST(MahjongRules, ZeroHanDealerTsumoProvesTsumoIsNotHan) {
  const std::vector<Meld> pong = {Pong({"9s", "9s", "9s"})};
  std::vector<Tile> zero_han = H({"1m","2m","3m","4p","5p","6p","7p","8p","9p","2m","2m"});
  const ScoreResult s = ScoreHand(zero_han, pong, *ParseTile("2m"),
                                  Ctx(0, -1, /*dealer=*/true));
  EXPECT_TRUE(s.yaku.empty());
  EXPECT_EQ(s.dealer_bonus, 1);
  EXPECT_EQ(s.han, 1);            // 0 + 庄家 1；若自摸计番会是 2
  EXPECT_EQ(s.multiplier, 2);     // 2^1；若自摸计番会是 4
  EXPECT_EQ(s.per_payer, 4);      // 2R；若自摸计番会是 8
  EXPECT_EQ(s.total, 12);         // 6R；若自摸计番会是 24
  EXPECT_EQ(s.delta, (std::array<int, 4>{12, -4, -4, -4}));
  EXPECT_EQ(Sum(s.delta), 0);
}
```

> 为什么这条最锐利：`ZeroHanTsumoIsExactlyMinusTwoPlusSix`（`:1760`）是非庄家 0 番自摸（R=1），`BonusTsumoAndDealerBonus`（`:1720`）是 3 番起点。**「0 番 × 庄家 × 自摸」这个交叉点两侧都没覆盖**，而它恰好是「自摸算不算番」会让 `R` 差一倍的那个点。

**② 文档漂移探测器（本项目风格的「最便宜的探测器」，建议采纳）** —— 新增 `tests/docs_drift_test.py`：

```python
# PROTOCOL §5.10 曾把 han_breakdown.total 写成 min(base+tsumo+dealer, 6)，
# 与 RULESET §6.1 / mahjong_rules.cc:1373 矛盾。这类「文档 vs 实现」漂移无法被
# 任何单测抓住（两边各自自洽），只能用字符串断言把它钉死。
def test_protocol_never_reintroduces_tsumo_in_han_formula():
    text = open("docs/PROTOCOL.md", encoding="utf-8").read()
    assert "min(base + tsumo + dealer, 6)" not in text
    assert "min(base+tsumo+dealer, 6)" not in text
    assert "min(base + dealer, 6)" in text
```

### 2.5 顺带发现：`_yaku_detail` 把「字段缺失」误报成「无番」（独立缺陷，建议同批修）

`client/render/result.py:89-93`：

```python
if not isinstance(win_diff, dict):
    return "本局不计分"
yaku = [...]
if not yaku:
    return "无番（0 番）"
```

问题：**「服务端没发 `yaku[]`」（旧/异构服务端）与「真的 0 番和牌」被渲染成同一句话**。前者是「未知」，后者是「确定 0 番」，UI 上宣称「无番（0 番）」是**事实错误**。

改法（不引入新分支也能修）：`yaku` 为空时再看 `win_diff.get("han_breakdown")` / `win_diff.get("han")`：
- 有 `han_breakdown` 且 `base == 0` ⇒ 真「无番（0 番）」（正确）
- `yaku` 键**不存在**且 `han_breakdown` 也**不存在** ⇒ 「明细不可用（数据缺失）」，且**不播报**

这与 §4.4 的四级降级是同一件事，裁定为：**由本方案的 `broadcast.lines_for()` 统一处理，`_yaku_detail` 不再自己判**（见 §4.2），避免两套降级逻辑。

### 2.6 兼容性结论

| 对象 | 是否破坏 | 说明 |
|---|---|---|
| 旧客户端 × 新服务端 | **不破坏** | 本次不动任何字段值与形状，只是把文档写对 |
| 新客户端 × 旧服务端 | **不破坏** | 旧服务端本来就发 `tsumo: 0`（`room.cc:747` 一直是这个值） |
| C++ 单测 / Python e2e | **不破坏** | 断言本就是对的 |
| 现有结算页渲染 | **不破坏** | `result.py:85` 的实现已经按「不渲染自摸」写 |
| **旧文档读者** | **破坏（这是目的）** | `PROTOCOL.md` §5.10 的公式会被推翻，属于**修 bug**，不是版本升级 |

**结论：议题 A 是纯文档修复，零运行时风险，零版本号变更。** 唯一要求是主程序在实现播报时**不得**按旧文档去渲染「自摸 +N」——否则会把一个文档 bug 变成界面 bug。

---

## 3. 议题 B 裁定书：播报期间的下一局时序

### 3.1 现状时序（读代码后的 4 个关键事实）

**事实 1 — 服务端窗口是一个固定的一次性定时器，客户端无从影响**
`server/room.cc:1454-1468`：`game_end` 广播完 ⇒ `phase_ = kRoundEnd` ⇒ `Transition()`（`seq_++`）⇒ `timeout_scheduler_(inter_hand_delay_ms_, MakeRoundAdvanceToken(seq_))`。
`server/room.cc:1550-1557`：令牌到点且 `phase_ == kRoundEnd` ⇒ `StartRound()`。
参数：`room.h:128` `kDefaultInterHandDelayMs = 5000`，`room.h:129` `kMaxInterHandDelayMs = 60000`。

**事实 2 — 局间发任何对局帧都会吃 `BAD_REQUEST`（并且计配额）**
`server/room.cc:1653-1656`：
```cpp
if (phase_ == Phase::kRoundEnd) {
  co_await ReplyError(seat, protocol::kCodeBadRequest,
                      "the round has ended; wait for the next game_start");
  co_return;
}
```
`UI_UX_SPEC.md:1012-1013` 已把这条冻结成 UX 前置说明：「客户端也不得在局终页响应这两个操作（**发帧会撞上服务端局间状态的 `BAD_REQUEST`**）」。
⇒ **这是方案 B（客户端 ready 回报）的死刑判据**：它要求客户端在局间发帧，必须先在这一行**之前**插白名单，否则每次回报都计 `RecordBadFrame`（`player_actor.cc:75-79`）并可能关连接。

**事实 3 — `game_start` 会硬重置结算页状态**
`client/game_state.py:290-295`：
```python
self.result = None
self.result_at = None          # ← 倒计时基准没了
self.match_end = None
self.win_seat = None
self.game_started = True
self.screen = SCREEN_TABLE     # ← 直接跳走
```
⇒ **客户端纯本地延长播报 = 一定被 `game_start` 打断**。这是裁定方案 A 的最强理由：客户端不能凭空造时间。

**事实 4 — 场终没有延迟，最后一局的播报会被 `match_end` 秒杀**
`server/room.cc:1470` `co_await FinishMatch();` —— `match_over` 为真时**没有任何 sleep**，`match_end` 紧接着 `game_end` 下发。而 `match_end` ⇒ 场终页（`game_state.py:544-555`）。
⇒ **不处理的话，一场最后一局的番数推导玩家一秒都看不到**（而最后一局通常最想复盘）。这必须和议题 B 一起解决。

**事实 5 — 客户端倒计时的 5 s 是硬编码常量，与服务端参数**只是「口头同源」**
`client/render/result.py:53` `NEXT_ROUND_SECONDS = 5.0`，`:209-213` `_seconds_to_next_round()` 用它算剩余。
**这是一个既有 bug**：`./start_server.sh -- --inter-hand-delay-ms 8000` 启动时，客户端会显示 5→0 然后卡在「0 秒」等 3 秒。播报特性会把「卡在 0 秒」放大 2 倍，所以这条**必须**顺手修掉。

### 3.2 方案对比

| 维度 | **A：服务端延长窗口 + 帧内携带 `interlude_ms`**（推荐） | A0：纯客户端压缩，服务端 0 改动 | B：客户端 ready 回报，服务端等齐 | C：新增 `round_hold` / `round_resume` 帧 | D：`sleep` / 阻塞主循环 |
|---|---|---|---|---|---|
| 改动面 | 服务端：`room.h:128` 默认值 5000→11000、`game_end` +1 字段、`FinishMatch` 加延迟；客户端：+1 模块 + `result.py` 读字段 | 客户端仅 +1 模块 | `protocol.h:71` +1 帧类型 + `player_agent.h` 绑定 + `player_actor.cc:144` 的 `static_assert` 会对不上 + `room.cc:1653` 插白名单 + 「单令牌推进」改「等齐 or 超时」双条件 | 2 个 S→C 帧；但**仍需 B 的回执**才有意义，否则只是 A 换了层皮 | 0 |
| 「暂停」的语义 | **显示暂停**（时间不暂停） | 不暂停（只是塞得进去） | 时间暂停 | 时间暂停 | 时间暂停（假象） |
| 1 真人 + 3 AI | 天然成立（AI 无客户端，无需参与） | 成立 | 只等真人 ⇒ 语义退化成 A | 同 B | 成立 |
| 播报中客户端断线 | 服务端自走，客户端 `abort`，对局不受影响 | 同 | 回执永不到 ⇒ 必等超时 ⇒ **退化成 A** | 同 B | 客户端卡死，服务端等超时 |
| 玩家跳过后的空等 | ≤ 5 s（原阅读窗） | 0 | 0 | 0 | 0 |
| `game_start` 早到竞态 | 预算内不会；**无条件兜底为丢弃**（§3.6） | 不存在 | 不存在 | 取决于实现，但同样需要兜底 | **一定发生** |
| 违反既有冻结条款 | `UI_UX_SPEC §9.2`「N 由 5 递减」需改（**本来就要改**，见事实 5） | 无 | **`UI_UX_SPEC §9.5` 前置说明「局间不得发帧」** | 无 | `FEEL_SPEC` 主循环约束 / `UI_UX_SPEC §1.2` |
| 新增失败模式 | 0（字段可缺省） | 0 | 回执丢失、回执伪造、回执乱序、`seq` 与局间无窗的冲突 | 帧积压、双状态机 | 心跳超时、窗口无响应 |
| 测试影响 | `--inter-hand-delay-ms 0/300` 场景**自动降级为静态页**；gtest 默认值断言要改 1 行 | 无 | **所有测试客户端都要回 ack**（`room_full_game_test.py` / `match_full_e2e_test.py` / e2e 全改） | 同 B | — |
| 预估工时（主程序单人） | ~1 人日 | ~0.5 人日 | ~2.5 人日 | ~2 人日 | — |
| **裁定** | ✅ **采纳** | ❌ 备选（主理人若否掉服务端改动则用它） | ❌ **不做** | ❌ **不做** | ❌ **禁止** |

### 3.3 裁定与理由

**采纳方案 A。核心裁定一句话：客户端只做「显示暂停」，服务端窗口是唯一时钟。**

不选 A0 的理由（诚实版）：A0 能省一天工时且零协议改动，但 `interlude_ms` 缺省时预算是 `5000 − 500 = 4500 ms`。虽然字幕是**全量静态显示**（不靠高亮推进来阅读，见 §4.3），4500 ms 里高亮只有 3.6 s、语音必须压到 ≤ 4500 ms，**语音几乎必然被截断**（`BC_VOICE_BUDGET_MS = 6000` 的用意就是「语音永不截断」）。用户已拍板「播报 = 字幕 + 语音」，让语音在正常路径下就被截断，等于交付一个打折的特性。**A0 保留为回退方案**，主理人若否掉服务端改动，就用 A0 + 把 `BC_VOICE_BUDGET_MS` 降到 4000（语音资产准入线同步降到 3500 ms/条）。

不选 B 的三条硬理由：
1. **它要求解冻一条已被冻结的 UX 条款**（`UI_UX_SPEC §9.5` 前置说明：「局间发帧会撞 `BAD_REQUEST`」）。为一个「省 5 秒空等」的收益去解冻一条防错条款，收益/风险不成比例。
2. **它的失败路径会静默退化成 A**：回执丢一次就得等超时，而超时值必须 ≥ 播报预算 ⇒ 最坏情况与 A 完全一样，最好情况才省 5 s。**下限相同、上限多一点、复杂度翻倍。**
3. **它给 3 个 AI 座位定义不出语义**：AI 无客户端。「等齐」只能解释成「等所有真人」，那 1 真人 + 3 AI 就等于 A；4 真人时又变成「一个人挂机拖住全场」。**同一套协议要覆盖两种玩家构成，是过度工程。**

不选 C 的理由：`round_hold/round_resume` 若不携带 `interlude_ms` 之外的信息，就是 A 加了两个帧；若要携带「现在还剩多少 hold」，那也是字段不是帧。**信息量没有增加，帧类型却增加了 2 个**（每个都要过 `kClientFrameTypes` + 绑定 + 文档表 + 测试）。

### 3.4 协议改动（若需要改协议：字段名 / 语义 / 默认值 / 兼容策略）

**新增字段（只增不改，`PROTOCOL.md §5.10` 的冻结允许）**

| 字段 | 类型 | 出现位置 | 语义 | 默认值 |
|---|---|---|---|---|
| `interlude_ms` | int | `game_end` **顶层**（与 `round_index` / `renchan_count` 同级，**不放进 `win`**，因为荒庄和场终也要用） | **从本帧发出时刻起，到服务端下发下一帧（`game_start` 或 `match_end`）的间隔，毫秒**。取值 = 本房间生效的 `--inter-hand-delay-ms`（`0..60000`）。`0` 表示服务端将立即推进（测试/极速场景）。**这是唯一权威的「下一局何时开始」**；客户端**不得**再用本地常量推算 | 服务端恒发；客户端缺省时按 `5000` |

**为什么叫 `interlude_ms` 而不是 `next_round_in_ms`**：这个间隔在**最后一局**通往 `match_end` 而不是 `game_start`（见事实 4）。叫 `next_round_in_ms` 会在场终那一局说谎。`interlude`（幕间）同时覆盖「局间」与「场终前」。

**服务端侧配套（必须一起改，否则字段会与行为脱节）**
1. `server/room.h:128`：`kDefaultInterHandDelayMs` **5000 → 11000**。
   构成：`5000`（原阅读窗，`UI_UX_SPEC §9.2` 的 5 s 保留不动）+ `6000`（播报硬预算 `BC_VOICE_BUDGET_MS`）。**给公式而不是给魔数**，并写进 `room.h` 注释。
2. `server/room.cc:1470` `FinishMatch()` 前**加同样的延迟**（复用 `timeout_scheduler_` 或直接在 `RoomSettled` 尾部 `co_await` 一个 timer）。否则最后一局的播报必被 `match_end` 秒杀（事实 4）。
   - 实现约束：**不得**用 `std::this_thread::sleep_for`（会阻塞 actor 线程/整个房间）。必须走既有的 `timeout_scheduler_` + 令牌机制（`MakeRoundAdvanceToken` 那套，`room.cc:1467`），或新增一个 `kMatchEndTokenBit` 走同一个 `OnWindowTimeout`。**这一条与客户端「不要 sleep」是同一条纪律的两端。**
3. 测试同步：`server/mahjong_room_test.cc:1702` 的 `EXPECT_EQ(kDefaultInterHandDelayMs, 5000)` → `11000`；`docs/A1_ACCEPTANCE.md:74` M8 行的「默认 5000 ms」→「默认 11000 ms（= 5000 阅读窗 + 6000 播报预算）」，并把判据里的「相邻 `game_end`→`game_start` 间隔 ≥5000ms」改成「≥ 参数值」（该行本来就注明了 e2e 用 `--inter-hand-delay-ms 300`，所以断言逻辑不变）。

**兼容策略（三向）**

| 组合 | 行为 | 结论 |
|---|---|---|
| 旧服务端（无字段）× 新客户端 | 客户端取缺省 5000 ⇒ 预算 `min(6000, 4500) = 4500`，自适应节拍压缩，播报放得下 | **可运行，只是语音可能被截** |
| 新服务端（11000）× 旧客户端 | 旧客户端仍按 `NEXT_ROUND_SECONDS = 5.0` 显示 ⇒ 数到 0 后**卡在「0 秒」6 秒** | **有可见回归** ⇒ 见 §3.8 的上线顺序 |
| 新 × 新 | 全功能 | 目标态 |

**上线顺序裁定：客户端先行 ① → 服务端后行 ②，且 ② 不得早于 ①。**
- ① 客户端：学会读 `interlude_ms`（缺省 5000）+ 播报 + 自适应节拍。此后对旧服务端**完全兼容**（走缺省分支）。
- ② 服务端：发字段 + 默认值 11000 + `FinishMatch` 延迟。
- **不要求同时上线**，前提是 P4（无旧客户端分发）。若存在分发，则 ② 必须与 ① 同版本发布 —— 因为 ② 单独上线会让旧客户端「卡 0 秒」。

### 3.5 文字时序图

**正常路径（局终、有下一局）**

```
服务端 RoomActor                    网络                 客户端 GameApp                       result.py（纯渲染）
    │                                │                     │                                    │
    │ RoundSettled：算分、连庄判定      │                     │                                    │
    ├─ BroadcastSettlement ──────────►│ game_end{... interlude_ms:11000} ──►│                        │
    │  (room.cc:1448)                 │                     ├─ state.apply()                     │
    │                                │                     │   result=frame, result_at=now      │
    │                                │                     │   screen=SCREEN_RESULT             │
    │                                │                     ├─ bc.start(frame, result_at)          │
    │                                │                     │   生成 lines[] / dwell / deadline   │
    │  timeout_scheduler_(11000)      │                     ├─ _sfx_later(100, "hand_end")       │
    │  (room.cc:1467)                │                     ├─ _sfx_later(500, "voice_win_ron")  │
    │  phase_=kRoundEnd, seq_++       │                     └─ render ─────────────────────────►│
    │                                │                     │                                    ├ 全量静态字幕立即显示
    │                                │                     │                                    ├ 末行「下一局…」**隐藏**
    │        ⋯ 播报进行中 ⋯           │                     │                                    │
    │                                │                     ├ t=+1.9s 高亮第 2 条 ──────────────►│ 高亮框移到第 2 行
    │                                │                     ├ t=+3.2s 高亮第 3 条 …             │
    │                                │                     ├ t=+3.9s bc.done=true               │
    │                                │                     ├ 显示「下一局即将开始（7 秒）」────►│ 倒计时出现
    │                                │                     ├ t=+5.4s → （6 秒）…               │
    │  timer 到点 (t≈11000)           │                     │                                    │
    ├─ StartRound ───────────────────►│ game_start ────────►│                                    │
    │                                │                     ├─ state.apply(): result=None,       │
    │                                │                     │   screen=SCREEN_TABLE              │
    │                                │                     └─ bc.abort("game_start")（应已 done，no-op）
```

**场终路径（最后一局）**

```
    ├─ BroadcastSettlement ──► game_end{interlude_ms:11000, match_will_continue:false}
    │                          │ 客户端照常播报（11s 预算）
    │  timer(11000)  ← §3.4 第 2 条新增的场终延迟
    ├─ FinishMatch ──────────► match_end ──► abort(若未完) + 切场终页
```

**竞态路径（理论上不该发生，但必须兜底）**

```
    ├─ BroadcastSettlement ──► game_end{interlude_ms:11000}
    │                          │ bc 播报进行中（例如 t=+2.0s）
    │  （因为网络抖动/客户端卡顿 等原因，game_start 提前到）
    ├────────────────────────► game_start ──► abort("race")
    │                          │  ① 丢弃播报（**不缓冲帧**）
    │                          │  ② audio.stop_all(0)（切屏静场，既有 app.py:1151-1153 已做）
    │                          │  ③ WARN "播报被 game_start 打断：abandoned_ms=2000"
    │                          └─ 直接按 game_start 渲染牌桌
```

### 3.6 异常矩阵（必须回答的边界全覆盖）

| # | 场景 | 触发点 | 期望行为（可断言） | 落点 / 依据 |
|---|---|---|---|---|
| 1 | **播报中 `game_start` 提前到** | 网络抖动、客户端卡顿、`interlude_ms` 极小 | **丢弃播报，不缓冲帧**（缓冲 = 拿旧帧覆盖权威状态，比丢播报糟得多）；`abort("race")`；WARN 记 `abandoned_ms`。**WARN 是探测器**：连续出现即说明预算偏小 | `game_state.py:290-295` 已硬重置，反向对抗它的代价远大于收益 |
| 2 | 播报中 `match_end` 到 | `match_will_continue == false` 且场终延迟未生效 | 同上（`abort("match_end")`），切场终页 | `app.py:788-794` |
| 3 | **播报中断线** | socket 关闭 | `abort("closed")` + 停语音；**停留结算页**（既有口径：断线前收到 `game_end` 就不显示断线蒙层）。服务端照常 timer 推进，座位转托管，**对局不受影响** | `app.py:1004-1007`；服务端零影响 |
| 4 | **用户中途跳过** | `Space` / 页面内左键 | `bc.skip()`：高亮跳至末行、`audio.stop_all(0)`、倒计时立即显示。**不发任何帧**（局间发帧会 `BAD_REQUEST`） | §3.1 事实 2 |
| 5 | **语音资产缺失** | 文件不存在 / 解码失败 | `audio.play()` 返回 `False`，**时间轴照走**（字幕不等待语音）。绝不重试、绝不补播 | `audio.py:180-184`（F-AU-25 已实现） |
| 6 | 语音比预算长 | `Sound.get_length()*1000 > budget` | 到 `deadline` 直接 `stop_all(0)`，字幕照常收尾 | `audio.py:208-218` |
| 7 | 副数极少（0–1 番） | 和牌但无番种 | 只有 3 行固定行，`dwell` clamp 到上限 700 ms ⇒ 播报 ~2.8 s 结束，剩余时间显示倒计时 | §4.3 |
| 8 | 副数极多（14 番种全中） | 封顶手 | `dwell` clamp 到下限 160 ms，14×160 + 1400 = 3.64 s ≤ 预算 | §4.3 |
| 9 | **荒庄** | `win == null` | 3 行：`荒庄（牌山摸尽）` / `本局不计分` / `庄家连庄`；语音 `voice_draw`。**不能复用和牌语音** | `result.py:27-31` `REASON_LABEL` |
| 10 | `interlude_ms` 缺失 | 旧服务端 | 以 5000 为总数（等价现状），预算 4500 | §3.4 |
| 11 | `interlude_ms < 2000` | 测试 / `--inter-hand-delay-ms 0` | **不播报**，静态页（等价现状）。保护既有 e2e（`room_full_game_test.py:853` 默认 300 ms、gtest 用 0） | 这是让本特性在测试下**完全惰性**的开关 |
| 12 | 4 名玩家 1 真人 + 3 AI | 常态 | 3 个 AI 座位**没有客户端**，不参与播报、不需要 ready。服务端 timer 与 AI 座位无关 | §3.1 事实 1 |
| 13 | 窗口失焦 / 最小化 | pygame 仍 60 fps | 时间轴用 `time.monotonic()` 差值，**不累加 dt、不数帧** ⇒ 回来时直接对齐到当前行，不出现「播报倒放/快进」 | `app.py:261` `self.now = time.monotonic()` |
| 14 | `--no-feel` 启动 | `FEEL_SPEC F-G-10` | 静态字幕照出（信息保留），无高亮推进、无语音。`F-G-10` 的覆盖范围需扩到本特性 | `FEEL_SPEC:439`（v1.8 明确「禁止硬编码 id 列表」，本特性自动被覆盖则不需改） |
| 15 | 超时上限 | — | **客户端不需要**自己设上限：窗口由服务端 `inter_hand_delay_ms_ ≤ kMaxInterHandDelayMs = 60000`（`room.h:129`）决定，客户端**没有能力延长**，所以「客户端挂住导致房间卡死」这类故障模式**在方案 A 里结构性不存在** | 这是选 A 的最重要非功能理由 |

### 3.7 明确「不要做」的方案及理由

| # | 不要做 | 理由 |
|---|---|---|
| X1 | **`time.sleep()` / `pygame.time.delay()` / 忙等在主循环里等播报播完** | `app.py:259-267` 的 `run()` 是 60 fps 单线程循环，阻塞它 = ① 输入全丢；② `_drain_net`（`app.py:551`）停摆，帧积压；③ `_heartbeat`（`app.py:1027`）发不出 `ping`，服务端 `OnIdleTimeout`（`player_actor.cc:329`）把玩家判空闲/托管；④ 窗口无响应（OS 标记 Not Responding）。**并且它连「跳过」都做不到**，因为跳过也要读事件。 |
| X2 | **嵌套等待循环**（`while not done: clock.tick()`） | 与 X1 同因，且会把 `pygame.event.get()` 的语义搞乱（同一帧事件被两个循环消费）。 |
| X3 | **客户端缓冲 `game_start` / `match_end`，等播报播完再 `apply()`** | 权威帧积压 = 故意制造陈旧状态。`seq` 判据（`game_state.py:563-566` 只认严格更大）会让后续帧被误判过期；且缓冲期间玩家看到的是「结算页」，服务端已在打牌，任何点击都是错的。**丢播报永远优于丢帧滞后。** |
| X4 | **客户端「请求延长」帧**（`hold` / `wait`） | 就是方案 B/C 的变种，且更差：它把「是否延长」的裁决权交给客户端，一个卡死的客户端能拖住整个房间。 |
| X5 | **删掉 `han_breakdown.tsumo` 字段或改名** | 破坏 `PROTOCOL.md:582`「字段只增不改」的冻结；收益只是文档可读性。见 §2.2 第 3 条。 |
| X6 | **把播报做成「服务端下发字幕文本」** | 字幕是 `game_end` 的纯函数（昵称、番种名、数字全部已在帧里），服务端下发等于把客户端文案搬到服务端，破坏 `UI_UX_SPEC §11` 的分层，并让本地化变成服务端职责。**文案生成的唯一位置是 `client/broadcast.py`。** |
| X7 | **`pygame.mixer.Channel` 之外另起一个线程做语音播放** | `pygame.mixer` 自带混音线程，另起线程只会增加竞态（且 `AudioEngine` 的 200 ms 去重表 `audio.py:114` 不是线程安全的）。 |
| X8 | **为播报加 `--broadcast-*` 一串命令行参数** | 一个旋钮就够（`--inter-hand-delay-ms`）。加参数 = 增加「配置组合爆炸」，而组合爆炸的代价最终都落在测试矩阵上。 |

---

## 4. 客户端实现约束

### 4.1 模块划分与状态机位置

**裁定：新建 `client/broadcast.py`，`result.py` 只画不写，`app.py` 持实例。**

| 层 | 文件 | 职责 | 禁止 |
|---|---|---|---|
| 文案与时间轴（纯逻辑） | **新建** `client/broadcast.py` | ① `lines_for(game_end) -> list[Line]`（纯函数，无 pygame import）；② `timeline(lines, budget_ms) -> Timeline`（dwell / deadline）；③ `Broadcast` 状态机：`start / update(now) / skip / finish / abort`；④ `BroadcastView` 只读视图 | 不 import `pygame`、不碰 `GameState`、不碰 `audio` |
| 驱动（主循环） | `client/app.py` | `game_end` 时 `self.bc.start(...)`；`update()` 里 `self.bc.update(now)`；`game_start`/`match_end`/`closed`/切屏 时 `self.bc.abort(...)`；`build_ui()` 里 `bc=self.bc.view()` | 不在这里写文案、不在这里算 dwell |
| 渲染（只读） | `client/render/result.py` | 读 `ui.bc` 画字幕与高亮；倒计时读 `interlude_ms` | **不写任何状态**（`UI_UX_SPEC §11` / `FEEL_SPEC R1`：动画/渲染不得修改 `game_state`） |
| 音频 | `client/audio.py` | 新增第 4 条总线 `VOICE`（ch3） | 不改既有 16 条样本与打断规则 |

**为什么不塞进 `result.py`**：`result.py` 是 539 行纯渲染，签名是 `render(surface, ui)`，没有 `self`、没有 `update`、没有时间拥有权。把状态机塞进去必须给它一个可变对象，直接违反 `FEEL_SPEC R1`（「动画/音效不得修改 `game_state`；所有绘制读状态、不写状态」）——而且它拿不到「帧到达时刻」，只能拿 `ui.now`（每帧都变），**没法判断「播报是否已经开始」**。

**为什么不塞进 `app.py`**：`app.py` 已 1667 行，且 `feel.py` 已经确立了「表现态单独成模块、`app.py` 只驱动」的先例。文案生成是**纯函数**，独立成模块后可以**不启 pygame 直接单测**（`tests/client_headless_test.py` 已有此能力）——这是它必须独立的最实际理由。

**状态机（5 个状态，全部由 `now` 驱动，无内部计时器）**

```
IDLE ──start(game_end)──► RUNNING ──now>=deadline / 末行播完──► DONE ──► (等 game_start)
                            │  │
                            │  └──skip()──────────────────────► SKIPPED ──► 显示倒计时
                            └─────abort(reason)──────────────► ABORTED（终态，不可恢复）
```

- **终态三个（DONE / SKIPPED / ABORTED）都不可回到 RUNNING**。同一帧重复 `game_end`（不可能，但便宜）⇒ `start()` 时若已在 `RUNNING`，先 `abort("restart")` 再起新的。
- 状态住在 `Broadcast` 实例上（`app.py` 持有），**不写进 `GameState`** —— `GameState` 是协议镜像（`game_state.py` 的字段全部来自帧），加一个本地表现字段会污染「整体覆盖」（`PROTOCOL.md` §5.12 客户端 MUST 把 `state_sync` 视为权威整体覆盖）。

### 4.2 播报内容生成（`lines_for`，单一文案来源）

**裁定：文案生成的唯一位置是 `broadcast.lines_for()`。`result.py:81-121 _yaku_detail()` 不再自己拼番种明细，改为消费 `bc.lines`（一处生成、两种呈现）。**

行序（`kind` 用于渲染样式与语音选段）：

| 序 | `kind` | 文本 | 数据来源 | 动态？ |
|---|---|---|---|---|
| 1 | `title` | `第 X/4 局 · 连庄 N · 自摸` / `… 荣和` / `… 荒庄（牌山摸尽）` | `round_index` / `round_count` / `renchan_count` / `reason` | 固定 1 行 |
| 2 | `winner` | `<和牌者> 自摸` / `<和牌者> 荣和，<放炮者> 放炮` | `win.seat` / `win.from_seat` / `results[].match_total` 无关 | 固定 1 行（荒庄时替换为「本局不计分」） |
| 3..N | `yaku` | `<中文名> <han> 番`（`yakuhai` 且 `han>1` ⇒ `<中文名> <han> 组`） | `win.yaku[]`（**顺序直渲，不排序**） | **动态 n 行（0..14）** |
| N+1 | `dealer` | `庄家 +1` | `han_breakdown.dealer`（**>0 才出现**） | 动态 0/1 行 |
| N+2 | `total` | `合计 <total> 番（<multiplier> 分）` + `已封顶`（`total==6` 才加） | `han_breakdown.total` / `win.multiplier` | 固定 1 行 |
| N+3 | `payment` | 自摸：`三家各付 <per_payer> 分，合计 <total> 分`；荣和：`<放炮者> 支付 <per_payer> 分` | `win.total`（**分数，不是番数**，`PROTOCOL.md:611` 明文警告） | 固定 1 行 |
| N+4 | `leader` | `本场累计：<昵称> 领先` / `… 并列领先` | `results[].match_total`（复用 `result.py:124-140 _leader_text` 的算法，**迁到 `broadcast.py`**） | 固定 0/1 行 |

**硬禁令（写进 `broadcast.py` 的模块注释，QA 可断言）**：
- ❌ **禁止出现「自摸 +0 番」**（议题 A：`tsumo` 恒 0、不是番种）。行列表里**没有** `tsumo` 这个 kind，**结构上**不可能生成。
- ❌ **禁止把 `win.total` 当番数**（`PROTOCOL.md:611`）。番数只用 `han_breakdown.total`。
- ❌ **禁止对 `yaku[]` 排序**。顺序 = `RULESET §6.4` 冻结判定序 = 结算页渲染序（`PROTOCOL.md:622`）。
- ❌ **禁止出现「平和」作为 0 番的文案**（`UI_UX_SPEC:941`：「平和」在 v2.0 已是 1 番番种名，0 番写「无番（0 番）」）。

### 4.3 节拍算法（自适应 dwell，随内容压缩）

```
输入：game_end 载荷
interlude = payload["interlude_ms"] if isinstance(int) and >= 0 else 5000   # 旧服务端缺省
if interlude < BC_MIN_INTERLUDE_MS:   ⇒ 静态页（不播报，等价现状）
budget  = min(BC_VOICE_BUDGET_MS, interlude - BC_TAIL_RESERVE_MS)
n_dyn   = len(yaku) + (1 if dealer > 0 else 0)
dwell   = clamp((budget - BC_FIXED_MS) / max(1, n_dyn), BC_MIN_DWELL_MS, BC_MAX_DWELL_MS)
deadline= result_at + (interlude - BC_TAIL_RESERVE_MS) / 1000.0
```

**常量（`broadcast.py` 冻结）**

| 常量 | 值 | 含义 |
|---|---|---|
| `BC_MIN_INTERLUDE_MS` | `2000` | 低于此值不播报（保护 `--inter-hand-delay-ms 0/300` 的测试） |
| `BC_VOICE_BUDGET_MS` | `6000` | 播报总预算；**同时也是语音资产的准入线**（单条 ≤2500、合计 ≤6000） |
| `BC_TAIL_RESERVE_MS` | `500` | 距服务端推进的保留余量，吸收网络延迟与调度抖动 |
| `BC_FIXED_MS` | `1400` | 固定段预算：标题 500 + 合计 500 + 收付 400 |
| `BC_MIN_DWELL_MS` | `160` | 逐条高亮最短驻留 |
| `BC_MAX_DWELL_MS` | `700` | 逐条高亮最长驻留 |
| `BC_VOICE_START_DELAY_MS` | `400` | 等 `F-AU-15 hand_end`（上界 400 ms）播完再起语音，避免 4 总线同时响 |
| `BC_MAX_DYNAMIC_LINES` | `14` | = 番种总数，`n_dyn` 的天然上界（拿到 >14 说明服务端有 bug，截断并 WARN） |

**预算自检（三组极端值，可直接写成单测）**

| 场景 | `interlude` | `n_dyn` | `budget` | `dwell` | 实占总长 | 判定 |
|---|---|---|---|---|---|---|
| 新服务端 · 极繁（封顶 14 项） | 11000 | 14 | 6000 | `(6000−1400)/14 = 328` | `1400 + 4592 = 5992` | ✅ ≤ 6000 |
| 新服务端 · 极简（0 番，无庄家） | 11000 | 0 | 6000 | `clamp(4600/1) = 700` | `1400 + 0 = 1400` | ✅ 早早结束，倒计时提前出现 |
| **旧服务端** · 极繁 | 5000（缺省） | 14 | 4500 | `(4500−1400)/14 = 221` | `1400 + 3094 = 4494` | ✅ ≤ 4500 |

> **关键设计点：字幕是「全量静态 + 逐条高亮」，不是「逐条出现」。**
> 所有行从 `t=0` 就全部绘制出来（这就是「分区静态」），高亮只是引导视线。因此 ① 读得慢的人不会被节拍卡住（随时能读完整推导）；② `dwell` 可以被压到 160 ms 而不影响可读性；③ 语音缺失/延迟**永远不会**让字幕错位（字幕不依赖音频长度）。这一条是整个节拍设计的支点，**不得**改成「逐条出现」。

### 4.4 解析降级（`han_breakdown` 缺失时如何退化）

**四级降级，逐级判定，每级都可渲染、都不崩溃、都不撒谎**：

| 级别 | 触发条件 | 番种明细 | 合计 / 收付 | 播报 | 语音 |
|---|---|---|---|---|---|
| **L0 完整** | `han_breakdown{base,tsumo,dealer,total}` 是 dict 且 `total` 是 int，`yaku[]` 是 list | `yaku[]` 逐条 | 取 `han_breakdown.total` / `multiplier` | ✅ 完整播报 | ✅ |
| **L1 无 breakdown** | `yaku[]` 在，`han_breakdown` 不是 dict | `yaku[]` 逐条 | `base = Σ yaku.han`；`dealer = 1 if win.dealer_bonus is True else 0`；`total = win.han if int else min(base+dealer, 6)`；`multiplier = win.multiplier if int else 2**total` | ✅ 完整播报（数字与 L0 一致，因为公式同源） | ✅ |
| **L2 无 yaku** | `yaku[]` 不在或为空，但 `han` / `han_breakdown` 有 | **不列番种**，改写一行 `番种明细不可用` | 取能用到的那个（`han_breakdown.total` > `win.han` > `—`） | ✅ **只播固定 3 行**（标题 / 收付 / 累计） | ✅（固定短语与番种无关） |
| **L3 全缺** | `yaku` / `han_breakdown` / `han` / `multiplier` 全部不可用 | 一行 `本局结算（明细不可用）` | `win.total` / `per_payer` 若在则显示，否则 `—` | ❌ **不播报**（没东西可播），直接静止 + 倒计时 | ❌ |

**L2/L3 的判定必须显式检查「键是否存在」，不能用 `len(yaku)==0` 判**：
```python
has_yaku_key = isinstance(win, dict) and "yaku" in win      # 「有键但空」= 真的 0 番
has_breakdown = isinstance(win, dict) and isinstance(win.get("han_breakdown"), dict)
```
这正是 §2.5 那个既有缺陷的根因（`result.py:92` 用 `if not yaku:` 把「缺失」和「0 番」混判）。L0/L1 走「真 0 番 ⇒ 显示『无番（0 番）』」，L2/L3 走「明细不可用」，**两者文案必须不同**。

**兼容性承诺**：L1 是「旧服务端 + 新客户端」的主路径（v1.5 之前没有 `han_breakdown`，但有 `tags`/`multiplier`）。L1 用 `dealer_bonus`（`PROTOCOL.md:625` 的 bool 派生字段）而不是重新推算庄家身份，是为了**不与服务端各算一次**（`PROTOCOL.md:929` 明令「派生字段 MUST 与 `yaku[]`/`han_breakdown` 恒等，不得各算一次」）。

### 4.5 音频：新增第 4 条总线（与 `FEEL_SPEC` 的两处冲突，必须显式登记）

**冲突 1 — `FEEL_SPEC §5.2` 三条音色总原则第 3 条「禁用项」**（`FEEL_SPEC:256`）：
> 「**不得**出现人声/语音、不得含背景音乐式循环…」

用户已拍板「播报 = 字幕 + **语音**」，此禁令与需求直接冲突。**处置（登记为 `FEEL_SPEC §8` 的 C9 行）**：把该禁令的适用范围**收窄并写清**为「**§5.2 那 16 条音效样本**不得含人声」，并声明「结算播报语音是**独立资产类**（`voice_<id>.ogg` → `client/assets/voice/`，见 §4.5.3），不属本禁令」。
**不得**悄悄删掉这行禁令 —— 它是「16 条音效不要喧闹」的设计依据，删了整个 §5.2 就失去约束。

**冲突 2 — `F-AU-20` 并发上限 3**（`FEEL_SPEC:272`）：
> 「同一时刻最多 **3 个**音效并存（WIN 组 1 + ACTION 组 1 + UI 组 1），各占独立通道」

新增 `VOICE` 总线后理论上可同时 4 条。**处置**：`F-AU-20` 改为「最多 **4** 个并存（WIN 1 + ACTION 1 + UI 1 + **VOICE 1**）」，并新增 `F-AU-28`（`F-AU` 编号已用到 27，按「编号不复用、新增取新号」规则取 28）：

| 常量 | 规则 |
|---|---|
| `F-AU-28` VOICE 总线 | `CHANNEL_VOICE = 3`（`CHANNEL_COUNT = 16` 不变，`audio.py:46`）；音量组乘子 **0.95**（要听清，但低于 WIN 的 1.00）；**VOICE 不抢占任何总线、也不被任何总线抢占**（它与局内 16 条音效不共享叙事，`F-AU-27` 的「同通道后到者抢占」不适用）；进 VOICE 前先 `channel.stop()`（单条独占），不做交叉淡入 |

**`VOICE` 与既有总线的时序关系（靠构造避免冲突，不靠抢占）**：
- `game_end` ⇒ `_sfx_later(100, "hand_end")`（`app.py:784`，ACTION 组，上界 400 ms）⇒ 最晚 500 ms 结束。
- 语音 `_sfx_later(BC_VOICE_START_DELAY_MS = 400, "voice_*")` ⇒ **t = 400 ms 起播**。
- ⇒ 两者有 **100 ms 重叠**（`hand_end` 400 ms 上界 vs 语音起点 400 ms）。裁定：**接受这 100 ms 的极短重叠**（不同总线、音色性质差异极大：一个是木牌归拢，一个是人声），不值得为它把语音推迟到 600 ms（会削掉语音预算）。若人耳复核觉得糊，改 `BC_VOICE_START_DELAY_MS = 500` 即可（一行常量，不动结构）。

**`F-AU-26` 切屏静场**（`FEEL_SPEC:278`，`audio.py:208-218`）：`_on_screen_changed`（`app.py:1150-1153`）在 `RESULT → TABLE` 时 `stop_all(40)` —— 这**正好**是竞态兜底需要的行为，**不要改它**。把 `VOICE` 纳入 `stop_all` 的遍历范围即可（`_channels` 加一项，`audio.py:212` 的循环自动覆盖）。

**4.5.3 语音资产契约（`voice_<id>.ogg`，路径 `client/assets/voice/`）**

| id | 触发条件 | 时长上界 | 优先级 |
|---|---|---|---|
| `voice_win_ron` | `reason == "win_ron"` | 2500 ms | P0-voice |
| `voice_win_tsumo` | `reason == "win_tsumo"` | 2500 ms | P0-voice |
| `voice_draw` | `reason == "draw_exhausted"` | 2000 ms | P0-voice |
| `voice_capped` | `han_breakdown.total == 6`（紧接上一句） | 1200 ms | P1-voice |

格式准入线沿用 `F-AU-00c`（44.1 kHz 单声道 / ≤ −3 dBFS / 首尾无静音 / 不循环），授权归档沿用 `AUDIO_SPEC.md D4`。缺资产 ⇒ 静音占位（`audio.py:180-184` 已实现），**不影响字幕与时间轴**。

> **`voice_capped` 与 `voice_win_*` 的串联**：两条都要播，合计 3700 ms ≤ 6000。串联用「`get_busy()` 轮询」在 `app.update()` 里推进（`channel.get_busy()` 是 O(1)、非阻塞），**不用**「定时器 + 猜长度」。若第一条缺失（`play` 返回 `False`），队列直接跳到第二条。

### 4.6 不要新增阻塞调用（逐条点名）

| 禁止 | 理由 | 正确做法 |
|---|---|---|
| `time.sleep(...)` | 见 §3.7 X1 | 时间轴用 `now` 比较 |
| `pygame.time.delay(...)` | 同 | 同 |
| `thread.join()` / `Event.wait()` | 同 | 同 |
| `while ...: pass` 忙等 | 同 | 同 |
| `snd.get_length()` 在**每帧**调用 | 虽然 O(1)，但无必要 | 只在 `start()` 时算一次，缓存进 `Timeline` |
| `Sound()` 在 `start()` 时加载 | 违反 `F-AU-00d`（运行时零解码、零磁盘 IO） | 启动期一次性预加载（`audio.py:136-146` 的循环里加 4 条） |
| `channel.queue()` 串行排队 | `pygame` 的 queue 在旧版本行为不一致，且无法观察进度 | `get_busy()` 轮询 + 自己的 1 元素待播队列 |

**唯一允许的「等待」是「在 `update()` 里比较 `now >= next_at`」** —— 这就是既有 `_pump_sfx`（`app.py:1120-1128`）的模式，本特性必须照抄这个模式，不发明第二种。

### 4.7 60 fps 预算影响

| 项 | 每帧成本 | 说明 |
|---|---|---|
| 字幕绘制 | `≤ 18 × W.draw_text`（14 番种 + 4 固定行） | 现有结算页已经画了约 20 段文本 + 14 张牌（`result.py:216-352`）。新增 18 段是**同一量级**，肉眼与 profile 都不可分辨 |
| 高亮框 | 1 × `pygame.draw.rect` | 可忽略 |
| 时间轴推进 | `O(1)` 比较（`now` vs `next_at`） | 无循环、无排序 |
| 音频 | **0**（混音在 `pygame.mixer` 自己的线程上） | 主循环只发一次 `play()` |
| 内存 | 4 条语音解码后 `≤ 600 KB`（6 s × 44100 × 2 B） | 磁盘上 ogg 压缩后 `≤ 100 KB` |

**不做表面缓存（surface caching）**：18 段文本每帧重绘的成本远低于维护缓存的复杂度（缓存失效条件 = 昵称/数字/语言/字号变化，任何一种漏判都会出现「旧数字」这类最难查的 bug）。若 profile 真的超阈，裁剪顺序是：① 非高亮行改为「只画文本不描边」→ ② 砍掉高亮推进动画（只保留静态字幕）→ ③ 才考虑缓存。

### 4.8 性能预算表（按角色硬规则：每条必须有「数值目标 + 测量方式 + 超阈处置」）

> **声明**：下表「自动降级」一栏点名的 `broadcast.timeline()` / `bc.finish()` / `bc.deadline` 是**本方案新增并随特性一起落地的契约函数**，不是既有代码——特性尚未实现，故此处是**规格**，不是对现状的核对。
> 既有代码中与本特性相关、**已核对存在**的降级点只有两处：`audio.AudioEngine.play()` 的缺资产静音（`client/audio.py:180-184`）与 `stop_all()`（`client/audio.py:208-218`）。

| # | 指标 | 数值目标 | 平台 | 测量方式 | 类型 | 超阈处置（触发点 / 谁判定 / 后果） |
|---|---|---|---|---|---|---|
| B-1 | 结算页单帧渲染耗时 | `render_frame` **p95 ≤ 16.7 ms**（60 fps 预算内） | mac 本机 | `python -m cProfile -s cumtime client/main.py`，结算页驻留 ≥10 s（≥600 帧），读 `render_frame` 的 cumtime / ncalls | **门禁**（无运行时降级机制：帧耗时不是可以「自动变快」的东西） | **卡在合入环节**：主程序出 cProfile 摘要贴 PR，**tech-architect 判定**；p95 > 16.7 ms ⇒ **不合入**，先按 §4.7 的裁剪顺序降级后重测（门禁类依据：该指标只有在开发期可测，运行期无自适应手段，硬造一个「降级开关」属于过度工程） |
| B-2 | 播报时间轴总长 | `≤ interlude_ms − 500` | 全平台 | 单测：遍历 `n_dyn ∈ 0..14` × `interlude ∈ {5000, 11000, 60000}`，断言 `timeline.total_ms ≤ budget` | **自动降级** | `broadcast.timeline()` 内 `dwell = clamp(..., BC_MIN_DWELL_MS, BC_MAX_DWELL_MS)`；若 `n_dyn > BC_MAX_DYNAMIC_LINES(14)` ⇒ 截断为前 14 条逐条高亮、其余行静态显示并记 WARN；**双保险**：`app.update()` 里 `if now >= bc.deadline: bc.finish()` ⇒ 无论节拍算出什么，到点必收尾 |
| B-3 | 语音单条时长 | 单条 ≤ **2500 ms**（`voice_draw` ≤ 2000、`voice_capped` ≤ 1200）；合计 ≤ **6000 ms** | 全平台 | 资产入库时 `pygame.mixer.Sound(path).get_length() * 1000`（`audio.py:141` 已有 `Sound()` 加载，加一行日志即可出数） | **门禁**（语音一旦开始播，运行期无法「让它变短」—— 除非整条掐掉，而掐掉就是 §4.4 的 L3，不是降级） | **卡在资产入库**：音效总监出实测表，**tech-architect 判定**；超阈 ⇒ **退回换样本或裁剪**，**不得**改 `BC_VOICE_BUDGET_MS` 往上怼（改预算 = 改协议默认值 × 改 M8 验收 × 改服务端常量，一条腿踩三条线） |
| B-4 | 语音资产体积 | 磁盘增量 ≤ **100 KB**；解码后 RAM ≤ **600 KB** | 全平台 | `du -sh client/assets/voice`；`tracemalloc` 前后差值（结算页驻留快照） | **门禁** | **卡在资产入库**：同上，tech-architect 判定；超阈 ⇒ 退回（降采样/缩短/换样本） |
| B-5 | 主循环阻塞调用 | `app.run` / `update` / `render_frame` 调用链上 **`sleep`/`delay`/`wait` 出现次数 = 0** | 全平台 | `grep -nE "(sleep|time\.delay|\.wait\()" client/*.py client/render/*.py` + 代码评审 | **门禁** | **卡在代码评审**：主程序自查 + tech-architect 复核；**出现即驳回**（含「临时为了看效果」的注释代码——这类代码 100% 会活到生产） |

> B-1 / B-3 / B-4 / B-5 都是**门禁类**而非「自动降级」，依据逐条写明：① B-1 帧耗时在运行期无自适应手段；② B-3/B-4 是**资产准入**，只能在入库时判定，运行期的「降级」等于不播语音（= L3，是功能缺失不是降级）；③ B-5 是**结构性禁令**，出现即为缺陷，无「阈值」可言。只有 B-2 是真·运行时降级（节拍自适应 + 到点强制收尾）。

---

## 5. 文档改动清单（汇总，按「必须 / 建议」标注）

### 必须（阻塞，与本特性同批）

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
| D-1 | `docs/PROTOCOL.md` | §5.10（`:620` 附近，`match_will_continue` 之后新增一行） | 新增 `interlude_ms` 字段定义（§3.4 表） |
| D-2 | `docs/PROTOCOL.md` | §5.10 `han_breakdown` 行（`:623`）、`:633`、`:653`、`:1097` | 议题 A 的四处修正（§2.3 A-1..A-4） |
| D-3 | `docs/PROTOCOL.md` | §5.10 完整示例（`:640-652`） | 示例里加 `"interlude_ms":11000`，并把 `han_breakdown` 注释对齐 |
| D-4 | `docs/PROTOCOL.md` | §13 帧表 `game_end` 行（`:1315`） | 字段列表追加 `interlude_ms` |
| D-5 | `docs/UI_UX_SPEC.md` | §9.1 线框图 `:926` / §9.2 下一局提示行 `:949` | 「N 由 5 递减」→「N 由 `interlude_ms` 递减」；线框图补播报区域 |
| D-6 | `docs/UI_UX_SPEC.md` | §9.2 新增一行「播报字幕区」 | 给 rect、内容、数据来源（`win.yaku[]` / `han_breakdown` / `interlude_ms`） |
| D-7 | `docs/UI_UX_SPEC.md` | §9.4 展示项表 | 追加「番数推导播报（字幕 + 逐条高亮 + 语音）」一行 |
| D-8 | `docs/UI_UX_SPEC.md` | §9.5 前置说明（`:1012-1013`） | 补一句「播报期间同样不得发帧；跳过只改本地表现，不发帧」 |
| D-9 | `docs/UI_UX_SPEC.md` | §9.1 `:914` / §9.2 `:941` / §9.4 `:1000` | 议题 A 的三处修正（§2.3 A-5..A-8） |
| D-10 | `docs/FEEL_SPEC.md` | §5.2 三条总原则第 3 条（`:256`） | 禁令收窄为「16 条音效样本」，登记 §8 C9（§4.5 冲突 1） |
| D-11 | `docs/FEEL_SPEC.md` | §5.4 `F-AU-20`（`:272`）+ 新增 `F-AU-28` | 并发 3→4；新增 VOICE 总线规则（§4.5 冲突 2） |
| D-12 | `docs/FEEL_SPEC.md` | §8 冲突清单 + §10 常量清单（`:426`） | 登记 C9；`F-AU` 28→**29**，合计 91→**92** |
| D-13 | `docs/FEEL_SPEC.md` | §6.5 结算页展示 | 新增 `F-AN-34` 播报高亮推进（≤ `BC_MAX_DWELL_MS`，不 gate 按钮，`F-AN-32` 仍然有效） |
| D-14 | `docs/FEEL_SPEC.md` | §9 验收断言 | 追加 `FE-A20..A24`（见 §6） |
| D-15 | `docs/A1_ACCEPTANCE.md` | `:74` M8 行 + `:202` Q2 行 | 默认值 5000→11000（构成写清）；Q2 追加「播报跳过不发帧」判据 |
| D-16 | `docs/RULESET.md` | §6.1（`:386`） | 已正确，**只补一句**「`game_end.han_breakdown.tsumo` 恒 0，字段为形状兼容保留」 |
| **D-25** | `client/render/result.py` | `:119-120` | **「已封顶」措辞缺陷**（art-model-director 报）：`if total >= 6` 把「正好打满 6 番」与「被截断」混为一谈。改为按 `base + dealer` 三态判定（`<6` 不提上限 / `==6` 「已达上限 6 番」/ `>6` 「{base+dealer} 番 → 封顶 6 番」）。详见 §10.6；同时修正本文件 §4.2 的对应写法 |

### 建议（不阻塞）

| # | 文件 | 改动 |
|---|---|---|
| D-17 | `server/mahjong_rules.h` `:363-370` | 公式块补「`tsumo` 恒 0」一行（A-9） |
| D-18 | `docs/AUDIO_SPEC.md` | 新增 §「语音资产（播报）」：4 条 id 的映射表 + 授权归档 + 时长实测（沿用 D1–D4 的格式） |
| D-19 | `client/README.md` | 补「播报」的实现位置与 5 个常量（新同学入口） |
| D-20 | `docs/GAME_DESIGN.md` / `GAME_DESIGN_V2_DRAFT.md` | 自查 `han_breakdown` 段落是否也写错（本次未逐行核） |

---

## 6. 给主程序的验收钩子清单（QA 可据此写断言）

> 每条都写成「**输入 → 可观察输出**」的形式，不写「应该好看」这类不可断言的项。`[自动]` = 可脚本化，`[人工]` = 必须人耳/人眼。

### 议题 A 相关

| # | 钩子 | 判据 |
|---|---|---|
| V-1 | `[自动]` `han_breakdown` 恒等 | 任意 `game_end`：`Σ yaku[].han == han_breakdown.base` **且** `han_breakdown.tsumo == 0` **且** `han_breakdown.total == min(base + dealer, 6)` **且** `win.han == total` **且** `multiplier == 2**total`（五条全过） |
| V-2 | `[自动]` 自摸不计番 | 构造「0 番 + 庄家 + 自摸」：`han==1`、`multiplier==2`、`per_payer==4`、`total==12`（§2.4 ①） |
| V-3 | `[自动]` 文档不回归 | `tests/docs_drift_test.py` 通过（`PROTOCOL.md` 不含 `min(base + tsumo + dealer, 6)`，含 `min(base + dealer, 6)`） |
| V-4 | `[自动]` 界面不出现自摸番数 | 抓帧做 OCR 或渲染快照：番种明细行**不含**「自摸 +」字样（含 +0） |
| V-5 | `[自动]` 0 番与缺失不混淆 | ① 真 0 番 ⇒ 文案「无番（0 番）」；② `yaku` 键不存在 ⇒ 文案「明细不可用」，且**不播报**（§4.4 L2/L3） |

### 议题 B 相关

| # | 钩子 | 判据 |
|---|---|---|
| V-6 | `[自动]` 字段一致性 | `game_end.interlude_ms` 存在且 `== 服务端 --inter-hand-delay-ms`；`match_will_continue == false` 的那一局也有该字段（**不能省略**，见事实 4） |
| V-7 | `[自动]` 倒计时不再硬编码 | 以 `--inter-hand-delay-ms 3000` 启动：结算页首次渲染的倒计时 = **3**（不是 5）；`interlude_ms` 缺失时 = **5** |
| V-8 | `[自动]` 播报不越界 | 埋点：`bc.start` 时刻 `t0`、`bc.done` 的时刻 `t1`；断言 `t1 − t0 ≤ (interlude_ms − 500)/1000 + 50ms`（50 ms 容差） |
| V-9 | `[自动]` 竞态兜底不卡死 | 人为把 `interlude_ms` 改成 `2000`（或断点卡住客户端 3 s）⇒ `game_start` 打断播报；断言：① 牌桌正常可操作；② 出现 `WARN 播报被 game_start 打断：abandoned_ms=N`；③ **无**任何 `error` 帧（因为没发帧） |
| V-10 | `[自动]` 局间零出站帧 | 从 `game_end` 到 `game_start` 之间，客户端出站帧数 = **0**（含跳过、含播报结束）。这是 `A1_ACCEPTANCE.md:74` M8 已冻结的「客户端全程无任何『继续提前』出站帧」的延续 |
| V-11 | `[自动]` 惰性开关 | `--inter-hand-delay-ms 0` 与 `300`：播报**不启动**（`bc.state == IDLE`），结算页与改动前逐像素一致（可做截图 diff） |
| V-12 | `[自动]` 荒庄分支 | `reason == "draw_exhausted"` ⇒ 播报 3 行（荒庄/不计分/庄家连庄），**无**番种行、**无**收付行；语音 = `voice_draw` |
| V-13 | `[自动]` 末局不丢播报 | 场终那一局：`t(match_end) − t(game_end) ≥ interlude_ms − 50ms`（§3.4 第 2 条生效），且播报完整播完才切场终页 |
| V-14 | `[自动]` 断线 | 播报中 `kill` 服务端 ⇒ 停语音、停留结算页（不出现断线蒙层以外的新状态）、无异常栈、进程不退出 |

### 音频与性能相关

| # | 钩子 | 判据 |
|---|---|---|
| V-15 | `[自动]` 缺语音资产不阻塞 | 把 `client/assets/voice/` 整个改名 ⇒ 播报时间轴、字幕、倒计时**完全不变**（只有没声音）；日志出现 4 条「音效缺失（静音占位）」 |
| V-16 | `[自动]` 静音键 | `M` 静音后语音不出声，重开**不补播**（`F-AU-25`） |
| V-17 | `[自动]` 阻塞调用为零 | `grep -nE "(sleep|time\.delay|\.wait\()" client/*.py client/render/*.py` 结果为空 |
| V-18 | `[自动]` 帧率 | 结算页驻留 10 s：`render_frame` p95 ≤ 16.7 ms（cProfile）；`clock.get_fps()` ≥ 58 |
| V-19 | `[人工]` 可读性 | 极繁（封顶 14 项）与极简（0 番）两个场景下，字幕**从第 1 帧起全部可读**（静态），高亮能跟上眼睛；高亮过快的观感问题 ⇒ 调 `BC_MIN_DWELL_MS`（只调常量） |
| V-20 | `[人工]` 人声不糊 | `voice_win_ron` / `voice_win_tsumo` / `voice_draw` 三者盲听可分辨；与 `hand_end`（木牌归拢）叠加时人声仍清晰（若不清晰 ⇒ `BC_VOICE_START_DELAY_MS` 400→500） |

---

## 7. 未决事项（需主理人裁决，不阻塞开工）

| # | 未决 | 我的建议 | 成本 | 若不同意的影响 |
|---|---|---|---|---|
| Q1 | **语音要不要念数字**（「合计 4 番 16 分」用嘴念出来） | **阶段 1 不念**：语音只播固定短语（荣和/自摸/荒庄/封顶），数字全部由字幕承担。理由：① 念数字要 `0-9 + 十 + 番 + 分 + 合计 + 庄家`≈ 18 条小样本 + 一条串播队列；② `FEEL_SPEC D4` 禁止联网 TTS，只能拼接离线样本，拼接处必然有接缝感；③ 用户的核心诉求是「番数计算的**详细过程**」，而那本质是**字幕/推导**的事 | 阶段 2 约 +1.5 人日（含 18 条资产） | 若念数字：`BC_VOICE_BUDGET_MS` 需 6000→9000、`kDefaultInterHandDelayMs` 11000→14000、M8 验收与 `--inter-hand-delay-ms` 文档全部联动。**接口已留（`BC_VOICE_MODE = "phrase" \| "digits"`），可后补** |
| Q2 | **`UI_UX_SPEC §9.2` 的「5 秒」是否允许被 11000 取代** | 允许，且**必须**：原 5 s 阅读窗**一字不动**保留为「11000 − 6000」，只是把「播报 6 s」插在它前面 | 0（只是文档） | 若不改，客户端显示 11→0 与文档的「5 秒」矛盾；或服务端不改默认值，退回方案 A0（语音必被截） |
| Q3 | 场终延迟用**新增令牌类型**还是**复用 `inter_hand_delay_ms_`** | 复用同一个值、新增一个 `kMatchEndTokenBit` 走同一个 `OnWindowTimeout`（`room.cc:1516` 的判据顺序是 `ai_pacing → round_advance → 窗口超时`，插一个分支即可） | ~0.3 人日 | 若复用 `round_advance` 令牌，会在 `kMatchEnd` 相位被丢弃（`room.cc:1551` 要求 `phase_ == kRoundEnd`）⇒ 场终延迟静默失效，末局播报仍被秒杀 |

---

## 9. 跨文档一致性裁定（v1.1 追加，覆盖兄弟规格的冲突）

> 本节针对同期并行产出的 `SETTLEMENT_BROADCAST_MOTION.md`（animation-director）、`SETTLEMENT_BROADCAST_DESIGN.md`（game-design-director）、`SETTLEMENT_BROADCAST_UI.md`（art-model-director）。**并行产出无先后优劣问题，本节只做「谁说了算」的收口**，避免主程序拿到两套互斥的协议需求。裁定原则：**内容质量采纳更细的一方，契约归属以本节为准。**

### 9.1 总表：7 项冲突与裁定

| # | 冲突 | 各方立场 | **裁定** |
|---|---|---|---|
| X-1 | **局间推进机制** | 本文件：方案 A（服务端延长 + `interlude_ms` 字段，无新帧）<br>MOTION `R1/R3/R4`：方案 B（新增 C→S 帧 `settlement_done`，客户端播完即报道） | ✅ **方案 A**。删除 `settlement_done`（MOTION `R1`/`R3`/`R4` 全部作废）。理由见 §9.2 |
| X-2 | **局间时长数值** | 本文件 v1.0：11000<br>MOTION `F-BC-00c`：9000<br>DESIGN `:689`：9000 + `NEXT_ROUND_SECONDS = 9.0` | ✅ **定为 11500**（当时 = `6200` 播报 + `5000` 阅读窗 + `300` 余量）<br>⚠️ **已被 §10.2 推翻为 `12000`**（= `9000` 播报 + `3000` 恢复窗）。实现取 **12000** |
| X-3 | **播报预算 B** | 本文件 v1.0：6000（粗公式）<br>MOTION `F-BC-00`：**6200**（含 `TOTAL = 2620 + MULT(han) + n×D` 与 14×4 校验表） | ✅ **6200，采纳 MOTION**。它比我的粗公式细一个量级，我没有理由用粗的覆盖细的。本文件 §4.3 的 `BC_*` 常量表由 MOTION §0.1/§3.2 取代（见 §9.4） |
| X-4 | **`BUDGET_TIGHT`（4200 ms 双形态）** | MOTION `F-BC-00b`：保留一个「纯字幕无语音」的兜底形态 | ✅ **删除双形态**。`budget = min(6200, interlude − 800)` 一个公式就够；`interlude = 5000` 时它**自然算出** 4200。两套时间轴 = 两套验收 × 两套测试，而触发它的组合（旧服务端）在 P1/P4 下不存在 |
| X-5 | **`--inter-hand-delay-ms` 语义** | MOTION `R2`：改为「**局间最长等待**」 | ✅ **不改语义**，仍是「固定停顿」。方案 A 没有「等谁」的机制，叫「最长等待」会让后来人去找那个「谁」，找不到就自己发明一个 ready 帧 —— 这是最坏的结果 |
| X-6 | **`han_breakdown.tsumo`** | DESIGN §6.1 / §2.4：A/B 两支并存（3-a、3-b、3-d、`vo_bonus_tsumo`），明示「待柯桥良裁定后删除另一支」<br>MOTION §11.3：也在等这一行裁定<br>UI `:260-263`：已按「`tsumo` 恒 0」写，且把「自摸 ×2」正确地放进**支付芯片**而非番数项 | ✅ **分支 B**（`tsumo` 恒 0、不计番）。删 A 支全部措辞、删 `vo_bonus_tsumo` 资产、DESIGN §2.4 删 3-a/3-b/3-d。**UI 的写法（×2 属支付不属番数）是正确解，作为基准** |
| X-7 | **段③ 自摸项** | MOTION `:655` rect 内容「自摸 +0 · 庄家 +1」、`:240` 留 `tsumo == 1` 分支 | ✅ **段③ 只渲染庄家加成**，`dealer == 0` 时**整段不进时间轴**（不是显示「+0」）。`han_breakdown.tsumo` **不得出现在任何段、任何 rect、任何语音**。与 `result.py:113-114` 既有实现（`if dealer > 0`）一致 |

### 9.2 为什么是 A 而不是 `settlement_done`（B）—— 四问四答

MOTION 的 `R1` 设计比我 v1.0 里假设的 B 更成熟（无字段、非局间状态静默丢弃、幂等键 `round_index`、断线不发）。我逐条重估后仍裁 A，理由按强度排序：

**① 用户要的是「暂停倒计时」，B 实质是「取消倒计时」。**（决定性）
MOTION §6.3 自己写：「**恢复时不重新数 5 秒** —— 下一局的开始时刻由客户端 `done` 驱动，数秒只会数到"游戏已经在等你了"」。也就是说 B 落地后，`UI_UX_SPEC §9.2` 冻结的「下一局即将开始（N 秒）」**这一行在播报结束后就永久消失了**（MOTION `C-b3` 也承认「局后不再恒显示秒数」）。
而用户的原话是「播放播报时**暂停**下一局倒计时」—— 暂停意味着**播完要继续**。A 保留完整 5 s 阅读窗；B 把它删了。**这是产品变更，不是技术选项**，且方向与用户措辞相反。

**② 收益是真金白银，但代价落在一个不对等的位置。**
B 的收益：每局省 ~5.3 s，一场 4–8 局省 21–42 s。这是真的，我不否认（v1.0 里我用「省 5 秒」淡化了它，此处修正）。
代价：`protocol.h:71` 加帧类型 + `player_agent.h` 绑定 + `player_actor.cc:144` 的 `static_assert` 三方联动；`room.cc:1653` 前插白名单（否则「静默丢弃」也要先过 `kRoundEnd` 那道 `BAD_REQUEST`）；`OnWindowTimeout` 的「单令牌推进」改「等 done 或超时」双条件；**`tests/e2e_full_game.py` 与所有测试客户端的 M8 语义反转**（MOTION §11.1 自己标了「必须与 tech-architect 对齐，否则 e2e 会红」）。
一个「省 42 秒」的收益，压在「改协议帧表 + 改房间状态机 + 反转一条已冻结的 e2e 断言」上 —— **不划算**，尤其这笔 42 秒本来就是我方主动删除一个已冻结的 UX 元素换来的。

**③ B 的失败路径静默退化成 A。** MOTION `R4` 自己写：断线不发 `done` ⇒「由 R2 的 9000 ms 兜底」。即 `done` 丢失时行为**就是** A。下限完全一样，上限只多一点，复杂度翻倍。

**④ 3 个 AI 座位在 B 里没有语义。** 「等齐」只能解释成「等所有真人」；1 真人 + 3 AI 时 B ≡ A，4 真人时又变成「一个人挂机拖住全场」。同一套协议要覆盖两种玩家构成 —— 这正是过度工程的教科书定义。

> **给 animation-director 的一句话**：你判断「现状下客户端无法单方面实现暂停」是**完全正确**的，这是全局最关键的一条洞察，我的裁定也建立在同一句话上。分歧只在解法：我选「服务端**延长**窗口并把窗口长度告诉客户端」，你选「服务端**等**客户端」。前者不动协议帧表，且保住了 5 s。

### 9.3 统一后的数值（唯一一套，三份规格都按这个改）

| 量 | 值 | 归属 | 备注 |
|---|---|---|---|
| 播报预算 `B` | **6200 ms** | MOTION `F-BC-00`（采纳） | `TOTAL ≤ B` |
| 原阅读窗 | **5000 ms** | `UI_UX_SPEC §9.2`（不动） | 播报结束后完整保留 |
| 服务端默认 `kDefaultInterHandDelayMs` | ~~**11500 ms**~~ → **12000 ms**（§10.2 修正） | 本节 | 构成改为 `9000 播报 + 3000 恢复窗`；`room.h:128` 注释写清两项构成 |
| 协议字段 | `interlude_ms`（`game_end` 顶层，int） | §3.4 | **恒发**；`match_will_continue == false` 那一局**也发**（场终也要延迟，见 §3.1 事实 4） |
| 客户端预算 | `budget = min(6200, interlude_ms − 800)` | 本节 | `BC_TAIL_RESERVE_MS = 800`（采纳 MOTION 更保守的 800，取代我的 500） |
| 硬截止 | `deadline = result_at + (interlude_ms − 800)/1000` | 本节 | 到点强制收尾 |
| 不播报阈值 | `interlude_ms < 2000` ⇒ 静态页 | 本节 | 保护 `--inter-hand-delay-ms 0/300` 的测试 |

> **禁止** `NEXT_ROUND_SECONDS = 9.0`（DESIGN `:689` 的建议）。`result.py:53` 那个硬编码 `5.0` 本身就是 bug（服务端改参数就在页面上卡 0 秒），把 bug 从 5 改成 9 只是把 bug 藏得更深。**倒计时必须读 `interlude_ms`，缺省才退回 5.0。**

> ⚠️ **本节 §9.3 的数值已被 §10.3 取代**（v1.2：音效总监用 `say` 实测数据证明 6200 ms 装不下任何人声）。§9.3 的**机制**（`interlude_ms` 字段、`budget = min(B, interlude − 800)`、单一时钟）全部保留，只有 `B` 与 `interlude` 的取值改为 `9000 / 12000`。保留下面的原文是为了留痕，**实现以 §10.3 为准**。

### 9.4 被 MOTION 取代的本文件章节（我让位）

| 本文件原内容 | 由谁取代 | 原因 |
|---|---|---|
| §4.3 的 `BC_FIXED_MS / BC_MIN_DWELL_MS=160 / BC_MAX_DWELL_MS=700` 与 dwell 公式 | MOTION §3.2 `D = clamp((B − 2820 − 96·han)/n, 240, 900)` + `TOTAL = 2620 + MULT(han) + n·D` + 14×4 校验表 | 它把「倍率阶梯占用段①的时间」显式扣掉了（`96×han`），我的粗公式没有这一项，压缩态会超预算 |
| §4.2 的 5 类 `Line`（title/winner/yaku/dealer/total/payment/leader） | MOTION 的 S0–S7 七段 + DESIGN 的六段文案表 | 它的分段粒度足以承载「倍率阶梯」「逐户支付」两项我漏掉的叙述需求。**但 `kind` → 段 的映射仍以 `broadcast.lines_for()` 为单一文案来源**（§4.2 的硬禁令全部保留，尤其「禁止自摸 +N」） |
| §4.5 语音 4 条固定短语 | MOTION §5.4 的 **34 条**清单（14 番种名 + 7 合计整句 + 2 和牌方式 + 庄家 + 封顶 + 7 倍率 + 支付 + 荒庄） | 它用「整句预录、不拼数字」绕开了我担心的接缝问题（番值只有 0–6，7 条就够）。技术上我的 Q1 顾虑被解掉了 |
| §4.5 `BC_VOICE_START_DELAY_MS = 400`（错开 `hand_end`） | MOTION §5（固定时长主时钟 + 语音从动 + 四条硬规则） | 它的「主时钟 + 从动」模型比我的「错开 400 ms」更彻底，且解决了我没处理的语音/字幕对齐 |

**但 §4.2 的 4 条硬禁令、§4.4 的四级解析降级、§4.6 的阻塞禁令、§4.8 的性能预算表 —— 全部保持有效，MOTION 未覆盖这三块。**

### 9.5 我在兄弟规格里发现的两个**真缺陷**（必须修，不是口味问题）

**缺陷 1 — 语音时长上界与压缩态 `D_MIN` 算术矛盾（会导致「番种名念一半被切断」）**

MOTION `A3`：`vo_yaku_*` ≤ **600 ms**；MOTION §3.2：`D_MIN = 240 ms`。
逐行语音在 `D = 240` 时，第 `i+1` 行的语音必然 `stop()` 掉第 `i` 行（`F-AU-28` 单条独占）⇒ **压缩态下每个番种名只念出前 40%**。被切断的人声比不念更糟（听感像卡顿）。
**裁定（新增常量）**：
```
BC_VOICE_MIN_DWELL_MS = 480        # 低于此 dwell 不播逐行语音
if D < 480:  只播 vo_mode_*（开场）与 vo_han_<total>（合计）两句，逐行与倍率语音全部关闭
```
判据可机械检查：`D` 由公式算出，比较即可，无需新逻辑。这样 34 条资产在压缩态只用到 3 条，且**永远不会出现念一半**。

**缺陷 2 — `C-b6`「红涨绿跌」是范围失控（建议不做）**

MOTION `C-b6` 要把结算页所有分数从「正绿负红」改成「红涨绿跌」，理由是「与播报同屏矛盾」。
**裁定：不改，保持 `T.POS`/`T.NEG` 现状。** 依据：① 四卡、排名表、战绩块**整页**都在用同一套色义，改色 = 整页回归，而播报只占页面的 1/4；② 「红涨绿跌」是**股市**约定，麻将结算页沿用「赢绿输红」已被 `UI_UX_SPEC §9.2` 冻结且当前没有玩家投诉；③ 同屏一致性只需播报**跟随**现有色义即可，不需要反向统一。**若主理人坚持改，属于独立需求，与播报解耦单独立项**（否则播报的回归面被放大到整页）。

### 9.6 采纳 MOTION 的 4 条冲突登记（我 v1.0 漏了）

以下四条 MOTION 发现得对，**登记为必须项**，我确认：
- `C-b1`：`FEEL_SPEC F-AN-06`「结算页不播任何动画」→ 解冻为「结算页仅允许 §12 播报动画，且必须可跳过」
- `C-b2`：`FEEL_SPEC F-AN-32`「结算页无输入」→ 改为「不响应出口操作；播报期允许跳过输入」。**注意：跳过不产生任何发帧**（原 `done` 那部分随 X-1 作废）
- `C-b7`：播报不消费 `dt`、按 `ui.now` 绝对时间求值 —— 与我 §4.3「节拍由 `now` 驱动、不数帧」同源，登记为 `F-G-02` 显式例外
- `C-b8`：播报不吃 `F-AN-04` 的 2.0 加速态 —— 与 `FEEL_SPEC` 冲突，需登记

### 9.7 文档改动清单追加（在 §5 的基础上）

| # | 文件 | 改动 |
|---|---|---|
| D-21 | `SETTLEMENT_BROADCAST_MOTION.md` | 删 `R1`/`R3`/`R4`（`settlement_done` 全链路）；`R2` 改「`interlude_ms` 字段 + 11500 + 语义仍是固定停顿」；删 `F-BC-00b`（`BUDGET_TIGHT`）；`F-BC-00c` 9000→11500；`:655` rect 改「庄家 +1」；`:240` 删 `tsumo==1` 分支；§5.4 删 `vo_bonus_tsumo`（若在）；§11.3 结论定为「分支 B」；新增 `BC_VOICE_MIN_DWELL_MS`；`C-b6` 撤回 |
| D-22 | `SETTLEMENT_BROADCAST_DESIGN.md` | §2.4 删 3-a/3-b/3-d，保留 3-c/3-e；§6.1 A/B 两支收敛为 B；`B-1`/`D-2` 示例删除；`:689` 的「9000 + `NEXT_ROUND_SECONDS 9.0`」改为「11500 + 读 `interlude_ms`」；`vo_bonus_tsumo` 不生产 |
| D-23 | `SETTLEMENT_BROADCAST_UI.md` | **无需改**（`:260-263` 的「自摸 ×2 属支付」是正确解，已被 §9.1 X-6 定为基准）；只需在 `:43` 的倒计时行注明「N 读 `interlude_ms`」 |
| D-24 | `SETTLEMENT_BROADCAST_AUDIO.md` | 按 `vo_*` 34 条（减 `vo_bonus_tsumo` = **33 条**）出授权归档；时长上界加「且 ≤ 对应的 `D`（压缩态按 §9.5 缺陷 1 关闭逐行语音）」 |

---

## 10. v1.2 追加：三方音频/布局需求裁定（含对 §9.3 数值的推翻）

> 触发：audio-director 用 `say -v Tingting`（22050 Hz，裁静音）对 8 类句子做了**实测**，证明 `6200 ms` 预算装不下任何人声。这是本轮唯一一条**实证**结论（其余都是推导），按它改。同时 art-model-director 报了 2 处既有实现缺陷，animation-director 报了 1 处性能缺陷，逐条裁。

### 10.1 事实：三套预算并存，且 6200 被实测证伪

| 项 | MOTION（动效） | UI（视觉） | AUDIO（音效） | **实测（r=175）** | 判定 |
|---|---|---|---|---|---|
| 播报总预算 | 6200（`TIGHT` 4200） | 4 番荣和 **6400** / 7 项+封顶 **8300** | 建议 **9000** | — | UI 的 6400 **已超** MOTION 的 6200 ⇒ 二者本来就不能并存 |
| 番种行停留 `D`（n=4） | 749（上限 900） | 700 | 建议 ≥1000 | 「三色同顺」**1060** / 「门前清」**879** | MOTION 的 900 上限与 UI 的 700 **都装不下 4 字名** |
| 单条番种语音上界 | ≤600 | 未定 | 建议 ≤1200 | 4 字名 **1060** | MOTION 的 600 **物理不可达**（14 条里 12 条永远不合格） |

**结论（可复算）**：`6200 ms` 下六段里**没有一段**装得下它自己的最短句子；`n` 越大 `D` 越小而句子长度不变，`n=6` 时 `D=467` 连 2 字「自摸」（492）都装不下。**结构上无解，不是调参问题。** 若强行保留 6200，玩家听到的是「自摸…四…庄家…一…已封…十…」——**碎片比没有语音更糟，它听起来像 bug。**

### 10.2 裁定：采纳 9000，删 4200，插 3000 恢复窗

| 量 | 值 | 依据 |
|---|---|---|
| **播报预算 `B`** | **9000 ms** | 采纳 audio（实测者）。`n=4` ⇒ `D=(9000−2820−584)/4 = 1399` ≥ 1100 ✅；`n=5` ⇒ `D=1100` 刚好；`n≥6` ⇒ `D<1060` 转摘要档（**降档解决，不靠 `fadeout` 截断**） |
| `BUDGET_TIGHT`（4200） | **删除** | 与 §9.1 X-4 一致：一个公式 `min(B, interlude − 800)` 就够，不要第二套时间轴 |
| **恢复窗 `RESUME_WINDOW_MS`** | **3000 ms**（新增） | 见 10.2.1 |
| **`interlude_ms` 默认** | **12000 ms** = 9000 + 3000 | `room.h:128` `kDefaultInterHandDelayMs` **5000 → 12000** |
| `BC_TAIL_RESERVE_MS` | **800** | 采纳 audio（比我的 500 保守） |
| 客户端预算 | `budget = min(9000, interlude_ms − 800)` | 单一时钟 |
| 硬截止（= 看门狗） | `deadline = result_at + (interlude_ms − 800)/1000` | **见 10.2.2** |
| `BC_VOICE_MIN_DWELL_MS` | **1060**（修订自己的 480） | 实测最长番种名 1060；低于此 ⇒ 关闭逐行语音转 Tier2 摘要。**480 是我拍的，1060 是量出来的** |

#### 10.2.1 为什么必须留 3000 ms 恢复窗（这是我这次唯一自己定数的地方）

用户原话是「播放播报时**暂停**下一局倒计时」。若 `interlude = 9000 + 800`，播报结束倒计时立刻归零 ⇒ **倒计时在玩家眼里没有"恢复"过**，等价于方案 B 的「取消倒计时」，而方案 B 正是我在 §9.2 否掉的。

留 3000 ms 才构成完整的「暂停 → 继续」：
```
t=0        播报开始，倒计时**不绘制**（不是冻结数字）
t=9.0 s    播报结束 → 倒计时出现「下一局即将开始（3 秒）」
t=12.0 s   下一局
```
**代价必须写清**：每局 12 s（现状 5 s），一场 4–8 局 ≈ 48–96 s，**比现状多 28–56 s**。这个代价是「人声把番数推导念完」本身的价格，不是实现浪费 —— 但它**改变了 `UI_UX_SPEC §9.2` 冻结的 5 s**，属**产品变更**，须主理人确认（见 Q2，已升级为必答）。

> 若主理人认为 56 s 太长，唯一诚实的替代是**砍语音**（回 6200 纯字幕，`SETTLEMENT_BROADCAST_AUDIO §7.4` 的 34 条句库也不必生产），而不是砍预算保留碎片音。

#### 10.2.2 看门狗必须是同一个截止点（audio 的「11000 独立硬上限」不采纳）

audio 提「硬上限 11 000 ms 是看门狗强制点」。**不采纳独立常量**，理由：`interlude = 12000` 时 `deadline = 11 200`，若另设 11 000 会**早于** deadline 触发 ⇒ 字幕还没播完就被砍；若设 13 000 则**晚于** deadline ⇒ 服务端已经推进而客户端还在播报（正是要避免的竞态）。**两个截止点必然一个错。**

**裁定：`VO_WATCHDOG ≡ bc.deadline`**，一个值。降档档位阈值也从 `budget` 推导，不写 11000。

### 10.3 五条音频硬需求裁定

| # | audio 的要求 | 裁定 |
|---|---|---|
| A1 | `set_reserved(4)`（现为 0 次调用，实测确认 `client/` 全仓无 `set_reserved`） | ✅ **接受**。顺序必须是 `set_num_channels(16)` → `set_reserved(4)` → `Channel(0..3)`。现状用显式 `Channel(0/1/2)` 不用 `find_channel`，所以这是**防御性**改动（防将来有人 `Sound.play()` 抢走 ch0-3），零风险 |
| A2 | VO 用 **ducking** 而非 P0 抢占：WIN duck −12 dB（提前 120 ms 起降 / 300 ms 完成 / VO 结束后 400 ms 回升）；ACTION `fadeout(80)`；`countdown_tick` 播报期禁用；`match_end` 使 VO `fadeout(80)` + 清队列 | ✅ **全部接受，且它取代我的两个旧结论**：① 删除 §9.4 里被我让位的 `BC_VOICE_START_DELAY_MS = 400`（ducking 已正确处理重叠，不需要错开）；② **修订 `F-AU-28`**：原文「VOICE 不被任何总线抢占」不成立 —— 它**被 `match_end`（P0）以 `fadeout(80)` 终止**。改为「不被 ACTION/UI 抢占；被 `match_end` 终止；起播时 duck WIN 而非 fadeout（和牌铃余韵是情绪高点，不该硬停）」 |
| A3 | 「先算后播」：装配 playlist 时用 `vo_manifest.json` 实测时长求总长，超预算按 Tier2/Tier3 降档 | ✅ **接受**，这是本批最重要的一条工程纪律（**不边播边算**）。但**阈值改为 `budget`**（见 10.2.2） |
| A4 | 零阻塞降级：缺句跳过不重试；全 VO 不可用 ⇒ fallback 时间轴（700/条、280/段间、≤6000）；覆盖「TTS 授权拿不到 ⇒ 只发字幕」 | ✅ **接受**。补一条：fallback 的总长仍须 ≤ `budget`，且 fallback 时**不改变 `interlude_ms`** ⇒ 倒计时剩余时间变长（9000→6000 省下 3 s 归入恢复窗），玩家体验为「这次播得快」 |
| A5 | 资源：56/57 条、22050 Hz WAV、≈2.5 MB；**首次进入结算页加载**（批次 A 同步 26 句 ≈25 ms 藏在 120 ms 淡入里，批次 B 每帧 ≤6 句） | ⚠️ **部分接受，改一处**：见 10.4 |

### 10.4 加载时机：我改 audio 的「首次进结算页」，改为「`game_start` 起分帧预载」

audio 的方案把 25 ms 同步解码**压在结算页第一帧**，靠「藏在 120 ms 淡入里」掩盖。问题是：**结算页第一帧正是整局的情绪高点**（和牌音刚落、金框要亮），此时掉 1–2 帧最贵；而且 `F-AU-00d` 承诺的「运行时零磁盘 IO」被破坏。

**改为**：
1. `game_start` 之后开始**分帧预载**，每帧 ≤6 句（≈2–4 ms/帧）。56 条 ≈ 10 帧完成；
2. 第 1 局的 `game_end` 至少在发牌 + 数十次出牌之后（≥ 数百帧），**必然加载完毕**；
3. 极端情况（`game_end` 来得早）⇒ 剩余同步补齐，且**补齐耗时 >25 ms 记 WARN**（这是探测器，不是错误）；
4. 加载完成后回到 `F-AU-00d` 的原始承诺：运行时零解码、零磁盘 IO。

**收益**：结算页第一帧零 IO、零解码；不需要赌「25 ms 能被淡入吃掉」。**成本**：一个分帧加载器（~20 行）+ 一条 WARN。**这是纯赚。**

### 10.5 修订性能预算表（§4.8 的 B-3/B-4 被推翻）

| # | 指标 | **新目标** | 测量 | 类型 | 超阈处置 |
|---|---|---|---|---|---|
| B-3′ | 单条语音时长 | 按 `AUDIO §7.2` 可行上界表：`vo_yaku_*` ≤**1200**、`vo_han_*` ≤**1050**、`vo_mode_*` ≤650、`vo_dealer_bonus` ≤1300、`vo_cap` ≤800、`vo_mult_*` ≤1050、`vo_pay` ≤1000、`vo_draw_hand` ≤1200 | 入库时 `wave.open()` 读实测 ms（AUDIO §5.5 管线已产 `vo_manifest.json`） | **门禁** | 卡在资产入库：audio-director 出数、**tech-architect 判定**；超阈 ⇒ 退回换句/缩写（「三色同顺」念「三色」），**不得**上调预算 |
| B-4′ | 语音资产体积 | 磁盘 ≤ **3 MB**；解码后 RAM ≤ **3 MB** | `du -sh client/assets/voice`；`tracemalloc` 差值 | **门禁** | 卡在资产入库；超阈 ⇒ 降采样或砍句。**注**：v1.0 的「100 KB / 600 KB」是按 4 条 44.1 kHz 估的，56 条 22050 Hz ⇒ ≈2.5 MB，原预算作废 |
| B-6 | 首次结算页帧时 | `game_end` 后首帧 p95 ≤ 16.7 ms（含同步补齐） | cProfile + 埋点 | **门禁** | 卡在合入；超阈 ⇒ 把补齐改为「播报 S0 期间补」并重测；仍超 ⇒ 记 WARN 并接受（**不允许**因此改回启动期全量预载） |

> 格式准入：`22050 Hz / 单声道 / 16-bit WAV` 是语音类的合法选择（`say` 原生输出，零重采样，`Sound()` 加载期转 44100 不进热路径）。`F-AU-00c` 的 44100 是**16 条音效**的准入线，语音是独立资产类，须在 `FEEL_SPEC` 登记例外（并入 C9）。

### 10.6 art-model-director 报的 2 处缺陷：1 处采纳，1 处驳回

**采纳：「已封顶」措辞把「打满」和「截断」混为一谈（真缺陷，与议题 A 同族）**

`result.py:119-120`：`if total >= 6: text += "已封顶"`。但 `清一色 5 + 门前清 1 = 6` 是**正好打满 6 番、根本没被截断**，玩家会以为少算了番。
**裁定（判据用 `base + dealer` 与 6 比较，不用 `total == 6`）**：

| `base + dealer` | 文案 | 语义 |
|---|---|---|
| `< 6` | 不加尾缀（或灰字「上限 6 番」） | 未触发上限 |
| `== 6` | 「已达上限 6 番」 | **打满，非截断** |
| `> 6` | 「{base+dealer} 番 → 封顶 6 番」 | 真截断 |

这条**同时修正我自己 §4.2 的写法**（我原来写「`total == 6` 时追加『已封顶』」，是错的）。登记为 D-25。

**驳回：「`R_NEXT_ROUND_HINT` 播报期秒数冻结不跳字」**

art 要求「秒数冻结不跳字」，MOTION §6.2 要求「不显示数字」。**裁定：不显示数字**，文案「播报中 · 空格跳过」。
理由：**冻结的数字会被读成卡死**（这是最常见的 UI 误读，玩家会去点屏幕救活）；而「不显示数字」同样满足 art 的原始要求（不出现递减数字）。art 提的「在 `result_at` 上加偏移」**不需要** —— 倒计时恒为 `interlude_ms − (now − result_at)`，播报期只是不绘制它，零额外状态（见 §10.7）。

### 10.7 art 的布局变更：批准，改动面已核

| 项 | 裁定 |
|---|---|
| 四座位卡片 `y 130→138`、`h 96→88` | ✅ **批准**。已核改动面：`R_RESULT_CARDS_Y/H`（`theme.py:285-286`）**全仓只被 `result.py:262` 使用，无任何测试依赖** ⇒ 实际改动 = theme 2 个常量。内容布局仍容得下（`match_total` 在 `y+h−22`，新 h=88 ⇒ `y+66..84`，未溢出） |
| `R_BC_CAPTION` 顶掉 `R_RESULT_TAGS` 的**局终页**用法 | ✅ 批准。**但 `R_RESULT_TAGS` 常量不得删除** —— 场终页 `result.py:371`（「最终排名如下」）仍在用 |
| 三个子区 `R_BC_YAKU / R_BC_CHAIN / R_BC_PAY` 绕开冻结的和牌张 `(500,246,120,120)` | ✅ 批准。**几何唯一权威归 `SETTLEMENT_BROADCAST_UI.md` §3**（见 10.9） |
| 收付块 `R_RESULT_PAYMENT` 被支付区取代；「牌山剩余」行删除、「本场累计领先」迁 `R_RESULT_FOOTER` | ✅ 批准。对应 `result.py:485-492` 与 `_leader_text` 的调用点要一起改 |
| `R_NEXT_ROUND_HINT` 位置不动 | ✅ 批准（`result.py:55`，不在 theme.py） |

### 10.8 animation-director 报的蒙层缺陷：确认，且升级为**本特性必须前置修复**

`result.py:218-220`：
```python
mask = pygame.Surface((T.LOGICAL_W, T.LOGICAL_H), pygame.SRCALPHA)
mask.fill(T.MASK)
surface.blit(mask, (0, 0))
```
`1280×720×4 B = 3.69 MB`，每帧新建 ⇒ 60 fps 下 **221 MB/s 的分配与丢弃**。且 `theme.py:281` 已有 `R_RESULT_MASK`（现未被这行使用）。

**裁定：这是真缺陷，且必须在本特性之前修**（提到 `__init__` 缓存一次，`R_RESULT_MASK` 的尺寸直接复用）。理由：播报要在这个函数里加 ~56 次 draw，GC 抖动会让 240 ms 级的高亮节奏开始抖 —— **它会把一个视觉问题变成「节奏不稳」的功能问题**。归入 B-1 门禁的前置项。

### 10.9 三方职责边界（一次性划清，停止互相改对方数字）

这是本轮混乱的根因：**三个人各自推导了同一组数字**。划清后各方只改自己的表：

| 层 | 权威文档 | 拥有什么 | **不得**做什么 |
|---|---|---|---|
| **契约层** | 本文件（ARCH） | `interlude_ms` 字段、`budget`/`deadline` 公式、`B=9000`、`interlude=12000`、解析降级 L0–L3、阻塞禁令、性能门禁 | 不定义段落时长、不定义像素坐标、不定语音文案 |
| **时间层** | `SETTLEMENT_BROADCAST_MOTION.md` | 段划分、`D` 公式、`TOTAL` 恒等式、缓动、跳过快进 | 不定义 `B`/`interlude` 数值（引用本节）、不定资产清单 |
| **几何层** | `SETTLEMENT_BROADCAST_UI.md` | 全部 rect、字号、色、三子区布局 | 不定义时长（引用时间层）、不定语音上界 |
| **资产层** | `SETTLEMENT_BROADCAST_AUDIO.md` | 句库、实测时长、上界、`vo_manifest.json`、流水线 | 不定义预算与 `interlude`（引用本节） |

**唯一数字源**：`B = 9000`、`interlude = 12000`、`reserve = 800`、`RESUME_WINDOW = 3000`、`BC_VOICE_MIN_DWELL = 1060`。任何文档出现其他值时，**以本节为准**。

### 10.10 模块命名收敛（三份文档出现过三个名字）

| 模块 | 裁定 | 理由 |
|---|---|---|
| `client/broadcast.py` | **纯逻辑**：`lines_for()` / `timeline()` / `Broadcast` 状态机 / `BroadcastView`。不 import `pygame` | 我 §4.1；可无 pygame 单测 |
| `client/render/broadcast.py` | **纯绘制**：只吃 `W.*`/`T.*`，**不读 `GameState`** | art 的切分，符合 `UI_UX_SPEC §11` |
| `client/broadcast_plan.py` | ❌ **不新建** | 与 `client/broadcast.py` 职责重合；三份文档三个名字 = 主程序必须猜 |

### 10.11 仍需闭环的 3 个数（资产侧）

| # | 冲突 | 裁定 |
|---|---|---|
| 1 | 语音条数：MOTION **34** / audio 消息 **56** / audio 文档 `:679` **57** | **定为 34 条**（`AUDIO §7.4` 方案 A 句库：14 番种名 + 7 合计 + 2 方式 + 庄家 + 封顶 + 7 倍率 + 支付 + 荒庄）。理由：`B=9000` + 名-only 语音不需要 `name+han` 整句（后者 1650 ms，14 条都装不下）。audio 文档的 57 条是「全句库」，在本预算下**不生产** |
| 2 | `vo_*` vs `vo_s2_*` 命名 | 采纳 audio 建议：**扁平 `vo_<组>_<id>`**，去掉段号（段号会随六段定义变动） |
| 3 | 「`vo_pay` 不念分数」的理由 | 采纳 audio 的纠正：`per_payer` 是**闭集 8 值** `{1,2,4,8,16,32,64,128}`（不是 `[1,384]` 连续区间），技术上可整句烘焙；不念的理由改为「**预算里没有它的位置**」 |

---

## 8. 修订记录

| 版本 | 变更 |
|---|---|
| v1.0 | 首次裁定：议题 A（番数口径，实现权威、改 8 处文档、补 2 条断言）、议题 B（方案 A：服务端延长窗口 + `interlude_ms` 字段，4 个方案对比 + 裁定 + 文字时序图 + 15 行异常矩阵 + 8 条「不要做」）、客户端实现约束（模块划分 / 四级解析降级 / 自适应节拍 / 第 4 条音频总线 / 5 条性能预算 / 60 fps 影响）、20 条验收钩子、3 项未决 |
| **v1.1** | **追加 §9 跨文档一致性裁定**：7 项冲突收口（X-1 裁定方案 A、作废 `settlement_done`；X-2 局间统一 11500；X-3 播报预算采纳 6200；X-4 删双形态；X-5 保留「固定停顿」语义；X-6 议题 A 定为分支 B；X-7 段③ 禁自摸）、4 处让位 MOTION（dwell 公式 / 分段粒度 / 34 条语音 / 主时钟从动）、2 个真缺陷（语音上界 vs `D_MIN` 矛盾、`C-b6` 红涨绿跌范围失控）、采纳 MOTION 4 条冲突登记、新增 D-21..D-24 |
| **v1.2** | **追加 §10，推翻 §9.3 的两个数值**：① **实测证伪 6200** —— audio-director 用 `say` 量出 4 字番种名 1060 ms，而 MOTION 的 `D` 上限 900、UI 的 700 都装不下，`6200` 下六段没有一段能装下自己的最短句；② **`B` 6200→9000、`interlude` 11500→12000**（含新增 `RESUME_WINDOW=3000`，否则「暂停→继续」退化成方案 B 的「取消倒计时」）；③ `BC_VOICE_MIN_DWELL` 480→**1060**（我拍的 480 换成量出来的 1060）；④ 看门狗 ≡ `bc.deadline`（否掉 audio 的独立 11000）；⑤ 接受 `set_reserved(4)` / ducking 取代 P0 抢占 / 先算后播 / 零阻塞降级，并**修订 `F-AU-28`**（VO 会被 `match_end` 终止）；⑥ **改 audio 的加载时机**为 `game_start` 起分帧预载（不把 25 ms 压在情绪最高点的首帧）；⑦ B-3/B-4 预算按实测重写（100 KB/600 KB → 3 MB/3 MB）；⑧ 采纳 art 的「已封顶」三子态（并修正我自己 §4.2 的写法）、批准卡片 `y/h` 变更（改动面已核 = 1 行）、驳回「冻结秒数」；⑨ 确认蒙层 3.69 MB/帧缺陷并**升级为本特性前置修复**；⑩ 新增 §10.9 四方职责边界 + §10.10 模块命名收敛 + §10.11 三个资产侧数字闭环 |
| **v1.1** | **追加 §9 跨文档一致性裁定**：7 项冲突收口（X-1 裁定方案 A、作废 `settlement_done`；X-2 局间统一 11500；X-3 播报预算采纳 6200；X-4 删双形态；X-5 保留「固定停顿」语义；X-6 议题 A 定为分支 B；X-7 段③ 禁自摸）、4 处让位 MOTION（dwell 公式 / 分段粒度 / 34 条语音 / 主时钟从动）、2 个真缺陷（语音上界 vs `D_MIN` 矛盾、`C-b6` 红涨绿跌范围失控）、采纳 MOTION 4 条冲突登记、新增 D-21..D-24 |
