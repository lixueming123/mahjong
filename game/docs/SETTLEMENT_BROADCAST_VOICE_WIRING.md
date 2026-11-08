# 结算播报 S2 · 语音接线与节拍重排（v1.3）

> 作者：主程序（寇豆码） · 2026-10-23 · 对应 `SETTLEMENT_BROADCAST_RULINGS.md` **§2.8 / v1.6**
> 输入：用户实机验收反馈两条 —— ①「没有语音」 ②「播报界面很快就退出了，然后就是原来的界面，还有剩余 7s 倒计时」
> 参照《明星三缺一 2004》（角色逐句把人声念出来，每个番种念清楚，不是一闪而过的字幕）。
> 本文件记录**怎么改、怎么验、验到了什么数字**；数字的唯一真值源仍是 RULINGS §2.1 + §2.8。
>
> **v1.3**：补 hold 改为 **§2.8 短局地板 8000（只保底、不拉伸，荒庄豁免）**；**L2 开播报**
> （只播结论三句 `vo_han_*`/`vo_mult_*`/`vo_pay_*`，番种段显示「明细不可用」，记 WARN）。

---

## 一、实现范围与依赖

### 1.1 两条反馈的定位（先分清，再改代码）

| 反馈 | 定位 | 证据 | 处置 |
| --- | --- | --- | --- |
| **没有语音** | 属实，S1 就只做了字幕（语音被授权问题压着），代码里 `VOICE_ENABLED = False`、`set_reserved` **一次都没调用过** | `grep -c set_reserved client/` = 0（改前） | 本轮接 VOICE 通道（§2.1） |
| **播报很快就退出 / 闪的太快** | **纯客户端节拍问题，与服务器无关**（旧进程假设已排除，见 §1.2）：旧算法 `budget=9000`、`dwell = clamp(avail/n, 240, 900)` ⇒ 2 番种整场只有 **4812 ms**，`DONE` 后还要在阶段 B 干等到 12000 ms | 用户原话「还有剩余 7s 倒计时开局」⇒ `interlude_ms = 12000` **已生效**，5000 是客户端自己提前收工 | 预算 9000→10500、逐条停留改语音驱动（≥1200 ms）、新增「不得提前收工」的补足规则（§2.3 第 3 条） |

### 1.2 `game_end` 抓帧与「旧进程假设」的排除

用**现存二进制**起一个真服务端，打一局，抓原始帧：

```
binary = build/game/mahjong_server      （mtime 2026-10-23 22:00:46）
source = game/server/room.cc            （mtime 2026-10-23 22:00:06）
命中：strings build/game/mahjong_server | grep interlude_ms → 1 条

{"type":21, "reason":2, "round_index": 0, "dealer": 0,
 "renchan_count": 0, "match_will_continue": true,
 "interlude_ms": 12000,        ← 关键字段在，值为 12000
 "results": [...], "win": {...}}
```

**结论**：

1. 源码与 22:00 那份二进制**都带** `interlude_ms: 12000` ⇒ 服务端侧没有缺陷。
2. 用户看到「剩余 7 s 倒计时」本身就是该字段生效的直接证据（`_seconds_to_next_round`
   只在 `interlude_ms` 存在时才算得出 7）。**⇒ 「旧进程」假设排除**：用户跑的是新服务端，
   5000 ms 是客户端自己的播报时长。
3. 真正的原因分两层：
   - **旧节拍**：`budget=9000` + `dwell ≤ 900` ⇒ 4812 ms 走完六段（每段 300–900 ms）；
   - **没有下限保护**：即便新节拍按语音算出 8084 ms，也没有任何机制阻止播报「提前收工」，
     于是阶段 B 被拉到 `12000 − 8084 = 3916 ms`。
   两者都在本轮修掉（§2.3 第 3 条的补足规则就是为第二层加的）。

### 1.3 依赖

| 依赖 | 状态 | 影响 |
| --- | --- | --- |
| 57 条 wav + `vo_manifest.json` | **已落盘**（`client/assets/vo/`，57/57，总长 66.40 s，实测与 §8.3 逐条一致，0 漂移） | 有语音路径可实跑 |
| 服务端 `interlude_ms = 12000` | **已就绪**（`room.h:131` / `room.cc:689`） | 播报窗 10500 + 阅读窗 1500 |
| TTS 分发授权 | **仍未澄清**（仅本地验收） | 缺资产路径必须是一等公民 ⇒ §2.4 |

---

## 二、系统实现方案

### 2.1 VOICE 总线（`client/audio.py`）

- **职责**：把「到点该念哪一句」变成一个真实的播放动作；缺资产时**什么都不做**。
- **数据结构**：`_channels["VOICE"] = Channel(3)`；`voices: dict[str, Sound]`（启动期一次性解码）。
- **关键流程**
  1. 启动期：`set_num_channels(16)` → **`set_reserved(4)`**（此前从未调用，ch0..3 会被
     `Sound.play()` 的自动选道抢走）→ 建 WIN/ACTION/UI/VOICE 四条通道 →
     `_load_voice()` 扫 `assets/vo/*.wav` 逐条 `Sound()`。
  2. 运行期：`play_voice(cue)` = `channel.stop()` → `play()` → `set_volume(MASTER×0.90)`；
     同帧把 ACTION 通道 `fadeout(80)`（收掉 `hand_end` / `draw_hand`）。
  3. **解码只发生在启动期**：`update()` / `render()` 调用栈里没有 `Sound()`（可 grep 断言）。
- **对外接口**：`play_voice(cue, now) -> bool` / `stop_voice()` / `voice_busy()` /
  `update(now, next_voice_in_ms)`（只维护 duck）/ `duck_gain()`（测试读数）。

### 2.2 WIN duck（−12 dB，**禁 fadeout**）

- **职责**：语音说话时把和牌铃压下去，但不掐掉（它是全场唯一情绪高点）。
- **数据结构**：`_duck_gain`（当前增益）/ `_duck_hold_until`（持留窗）/ `DUCK_GAIN = 10^(-12/20) = 0.2512`。
- **关键流程**：每帧 `set_volume`，**从不 `fadeout`**（fadeout 会把通道停掉）。
  - 前瞻：`next_voice_in_ms ≤ 120` ⇒ 立即开始降（起播前 120 ms）；
  - 下降速率 = 0.7488 / 300 ms ⇒ 300 ms 内到位；
  - 语音播放期间每帧刷新 `hold_until = now + 400 ms`（句间 140/240/320 ms 都落在窗内，不来回抖）；
  - 停止出声满 400 ms 后按 400 ms 线性回升。
- **实测曲线**（`t=0` 播 `win_tsumo`，第一句在 `+160 ms`）：

  | t | +0 ms | +100 | +300 | +900 | … | +10 s |
  | --- | --- | --- | --- | --- | --- | --- |
  | duck | 1.000 | 0.833 | 0.333 | **0.251** | 0.251（持续） | 0.251 → 播完 400 ms 内回 1.0 |

### 2.3 语音节拍（`client/broadcast.py`）

- **职责**：决定「每一步停多久、每一句什么时候出声」，仍是纯逻辑（不 import pygame、不碰文件）。
- **数据结构**
  - `Line.voice: tuple[str, ...]`：该行要念的句子（id 与 `vo_lines.tsv` 一一对应）；
  - `Timeline.cues: tuple[VoiceCue, ...]`：`(at_ms, cue, step_index)` 的**绝对时刻表**；
  - `Timeline.deleted`：因超预算被删掉的句子（可断言，不静默）；
  - `client/vo_durations.py`：57 条实测时长（**随代码走**，不从 manifest 读，理由见 §2.4）。
- **关键流程**
  1. 每步时长 = `max(该段固定下限, 该步句子时长 + 步尾间隔)`；逐条步另有 **1200 ms 地板**。
     步尾间隔：同段 150 / 跨段 240 / 进 S6 前 320；步内相邻两句 140。
  2. 全部句子按 `Step.at_ms` 铺开 ⇒ **句首与步切换同刻（差值 0 ms）**，天然满足 ≤120 ms。
  3. **短局地板（§2.8，v1.6 定稿）**：`pad_target = max(自然, min(8000, budget−40))`，差额均匀摊到
     S2/S3/S4/S5/S6 的 hold；**只保底、不拉伸**（自然 > 8000 的局一律不动），**荒庄豁免**。
     40 ms = 2 帧收尾余量（状态机只能在帧边界观察到 `now ≥ end_at`）。
     实测：荣和 1 番种 6 764 → **8 000**（补 1 236）；荣和 2 番种 8 320 / 3 番种 9 558 /
     密集 10 151–10 361 / 荒庄 3 907 **全部不动**。
     （历史表述 U1「不补」、S-3「补到预算」、以及中途提过的「+2000 上限」**均已作废**，以 §2.8 为准。）
  4. 总长 > `budget` ⇒ 按 AUDIO §8.8 的 `delete_rank` **从番值最小的条目逐条删句**
     （番值升序、同值按 `yaku[]` 索引降序 → 庄家加成 → 封顶）；被删句那一步退回 320 ms
     快速推进，**番种区仍静态显示该行**。保底 5 项（开场/合计/倍率/支付/自摸不加番）永不删。
  5. 空步（无字幕、无落点、无句子）直接不占时间 —— 荒庄原来在 S2/S3/S4 有 1280 ms 死时间。
  6. **S-2 裁决 B**：`vo_yaku_sum_<n>`（共 N 项）只在「不逐条念」的摘要路径出现，逐条路径
     **不接**（并存会固定多 1200 ms，把「荣和非庄 n=3」顶到 10525 破预算）。代码里留了注释位。
- **对外接口**：`timeline(lines, budget) -> Timeline`；
  `Broadcast.collect_voice(now)`（取出到点句子）/ `next_voice_in(now)`（duck 前瞻）/
  `started_at` / `end_at`（诊断读数）。**播放动作在 `audio.py`，时刻表在本模块**。

### 2.4 缺资产 = 一等公民（关键回归项）

时长表写进代码（`vo_durations.py`）而不是运行时读 `vo_manifest.json`，原因只有一条：
**时间轴必须与「有没有 wav」完全解耦**。若时长来自 manifest，把 `assets/vo/` 改名就会
连带改掉时间轴 —— 「改名后逐帧一致」这条验收项在原理上就不可能成立。

manifest 的角色降级为**回归比对物**：`vo_durations.verify_manifest()` 逐条对账
（本轮 `checked=57 / missing=0 / drift=0`），`verify_lines()` 对齐 `vo_lines.tsv`（`[]`）。

### 2.5 静音 `M` / 跳过 / 倒计时

| 项 | 行为 |
| --- | --- |
| `M` 静音 | `set_muted(True)` ⇒ `stop_all(0)` + `stop_voice()`；**时间轴照走**（句子照样"发出"，只是不出声） |
| 空格/回车/ESC/左键跳过 | 180 ms 快进 + 220 ms 收束；`skip()` 里把剩余队列清空 ⇒ **后续句子一律不念**，已响那句由 `stop_voice()` 停掉；**局间出站帧 0** |
| `countdown_tick` | 播报期（`bc.plan is not None`）**一律不播**，倒计时数字也不显示 |

### 2.6 语音 id 自检（team-lead 要求：比听感可靠）

遍历 52 个可达 id（14 番种 + 箭刻 1/2/3 组 + han 0..6 + mult 1..64 + pay ron 1..64 + pay tsumo 2..128
+ 庄家 + 封顶 + 荒庄三句），逐个 `os.path.isfile(client/assets/vo/<id>.wav)`：

```
voice_cues() 产出的 52 个 id 全部有 wav：缺 []
全部 id 在时长表里（否则时长为 0）：[]          ← 时长表漏一条就会 0ms
S-2 裁决 B：逐条路径不出现摘要句 vo_yaku_sum_*
未用到：vo_yaku_sum_2/3/4/5/6（5 条，按裁决 B 保留不接）
```

这 6 处曾与磁盘不符、现已按 `vo_lines.tsv` 修正：

| 曾发 | 现发 |
| --- | --- |
| `vo_yaku_yakuhai`（缺后缀） | `vo_yaku_yakuhai_<1\|2\|3>`（按 `han` 组数，>3 归 3） |
| `vo_total_<n>` | `vo_han_<0..6>`（`vo_han_0` = 「无番」） |
| `vo_dealer` | `vo_dealer_bonus` |
| `vo_capped` | `vo_cap` |
| `vo_draw`（一句） | `vo_mode_draw`（开场）+ `vo_draw_no_score` / `vo_draw_renchan`（结果） |
| `LEVEL_NO_YAKU` 返回 `()` | 不再返回空：`plan.voice` 里仍有 `vo_han_*` / `vo_mult_*` / `vo_pay_*`（**但 L2 整局是否播报仍归 RULINGS §2.6 的「不播报」，见 §6.1 D4**） |

---

## 三、工程组织

```
client/
  vo_durations.py    57 条实测时长（唯一副本）+ manifest/tsv 对账函数   [新]
  broadcast.py       时刻表：budget/删句/cues（纯逻辑，零 IO、零 pygame）
  audio.py           VOICE 通道 + duck + 启动期解码（平台碰点之一）
  app.py             `_pump_voice()`（唯一把「时刻表」变成「播放」的地方）
  assets/vo/         57 × wav + vo_lines.tsv + vo_manifest.json（音效侧产出，本模块只读）
  render/broadcast.py 阶段 A 渲染（**本批一行未改**）
  render/result.py   `is_broadcasting(ui)` 两阶段开关（**本批一行未改**）
tests/
  client_headless_test.py  新增 5 条播报语音断言；budget 9000→10500
  e2e_full_game.py         `broadcast_budget_ms()` 公式同步（M8 的下界）
docs/
  SETTLEMENT_BROADCAST_RULINGS.md   v1.3（唯一数字源）
  SETTLEMENT_BROADCAST_VOICE_WIRING.md  本文件
```

**一句话分工**：`broadcast.py` 只排时刻，`audio.py` 只出声，`app.py` 每帧把两者对上；
阶段 B（播报结束后的 v2.0 静态页）**一个 rect 都没动**，新代码全部关在
`if bc.running / bc.finished` 分支里。

---

## 四、资产接入清单

| 资产类型 | 来源角色 | 接入方式 | 验证点 |
| --- | --- | --- | --- |
| 57 条语音 wav（22050/1ch/16bit） | 音效总监 | 启动期 `_load_voice()` 一次性 `Sound()`，运行时零解码 | `AudioEngine().voices` 长度 = 57；`update`/`render` 栈内无 `Sound()` |
| `vo_lines.tsv`（id ↔ 文本） | 音效总监 | **只读**，用 `vo_durations.verify_lines()` 对账 | 差集为空 |
| `vo_manifest.json`（实测 ms/峰值/RMS） | 音效总监 | **回归比对物**，不参与运行时 | `checked=57 / drift=0`（容差 0.51 ms） |
| 16 条既有 sfx | 音效总监 | 不变；新增 `set_reserved(4)` 防自动选道抢占 | `ch0..3` 独占；`win_tsumo` 在语音期是 −12 dB 而非静音 |

---

## 五、构建步骤（每步可独立验证）

1. **起服务端**（旧进程假设已排除，但换版本时仍要重启）
   `./start_server.sh`（等价 `build/game/mahjong_server --port 9527`），默认 `interlude_ms = 12000`。
   验证：连一局，`game_end` 里能看到 `"interlude_ms": 12000`。
2. **起客户端** `./start_client.sh`。
   验证：启动日志出现 `语音预加载：57/57 条`；缺失时出现
   `语音资产未就绪（0/57 条）：播报按纯字幕运行`（**不是 WARN/错误**）。
3. **打一局到结算**。
   验证：字幕条逐句推进、每句都有声音、和牌铃没被掐（只是变小）、倒计时行显示
   「播报中…（空格跳过）」且不显示秒数；**播报约 7–10 秒**（随番种数），播完回到静态页，
   剩余秒数就是阅读时间（普通局 3.5–5.2 秒）。
4. **按空格跳过**。
   验证：约 400 ms 内回到 v2.0 静态页；声音立即停；抓包局间出站帧 = 0。
5. **把 `client/assets/vo/` 整个改名**（例如 `vo_off`）再打一局。
   验证：画面/字幕/停留时长**逐帧一致**，只是没声（§6.2 的 digest 比对）。
6. **诊断模式**：`MAHJONG_BC_TRACE=1 ./start_client.sh`，结算期终端会打印
   `[BC] 起播 t0=… end_at=… TOTAL=10460ms budget=10500ms 语音=6句`、
   `[BC] step=0/5 t=+898.1ms 计划=888ms 差=+10.1ms …`，以及每句 `[BC] cue …`。
7. **回归**：`SDL_VIDEODRIVER=dummy ../.venv/bin/python tests/client_headless_test.py`
   （**314 PASS / 0 FAIL**）、`../.venv/bin/python tests/docs_drift_test.py`（OK）。

---

## 六、技术债与风险

### 6.1 已知偏离（需主理人裁）

| # | 项 | 现状 | 影响 | 建议 |
| --- | --- | --- | --- | --- |
| ~~D1~~ | ~~阅读窗不是固定 1500 / 播报提前收工~~ | **已裁决（U1）**：**不补死时间**。播报按语音自然总长收工（荣和 1 番种 6 764 ms 是合理节奏），阅读时间动态为 `12000 − TOTAL` | — | 已闭环 |
| D2 | **dense 手牌会删句** | 荣和 4 番种 自然 10 911 → 删 1；庄家自摸 3 番种 → 删 2；8 条封顶自摸 → 只留保底 5 句。反例：**荣和 2/3 番种不删句**（8 320 / 9 558） | 与「每个番种念清楚」有张力 | **U2 已裁：维持现状** ——「语音有损、字幕无损」是分工，不是缺陷 |
| D3 | 荒庄 3 907 ms | 三句（荒庄/本局不计分/庄家连庄）完整念完，阅读时间 8 093 ms | V13 的 3600 只是参考上限 | **U3 已裁：完整念完，不砍「庄家连庄」**（禁止截断句子优先） |
| D4 | ~~L2（无 `yaku` 键）整局不播报~~ | **已开（v1.6）**：`broadcast` 白名单加 `LEVEL_NO_YAKU` ⇒ 只念结论三句 `vo_han_*`/`vo_mult_*`/`vo_pay_*`，番种段显示「明细不可用」，记一条 WARN 不弹提示。实测 L2：`vo_mode_ron`+`vo_han_3`+`vo_mult_8`+`vo_pay_ron_8`，total 7 045 | — | 已闭环；RULINGS §2.6 同步改口径 |

### 6.2 已验证项（数字，全部来自**真服务端 + 真音频引擎**）

**A. 端到端实测（`build/game/mahjong_server`，seed 20251005，抓真实 `game_end`）**

```
interlude_ms = 12000        budget = 10500
total_ms = 8084（= 语音自然总长；该局已在地板 8000 之上，不补）
实测 DONE = +8118.6 ms（偏差 +34.6 = 2 帧）      删句 = []

计划 steps：step0=888+1200(番种) | step1=2208+520(牌型) | step2=2728+1470(自摸不加番)
            | step3=4198+1897(合计+倍率) | step4=6095+1769(收付)

S0  t=+   86.7 ms（首帧；含测试脚本自建 AudioEngine 的耗时，产品里在启动期完成）
S1  t=+  892.9 ms  计划=888   差  +4.9   focus=('yaku',0)
S2  t=+ 2210.7 ms  计划=2208  差  +2.7   focus=('chain','base')
S3  t=+ 2742.6 ms  计划=2728  差 +14.6   focus=('chain','dealer')
S5  t=+ 4202.8 ms  计划=4198  差  +4.8   focus=('chain','total')
S6  t=+ 6118.8 ms  计划=6095  差 +23.8   focus=('pay','P1')
S7  收束 t=+ 8095.0 ms（结论句常驻 → DONE）

实测 game_end → game_start = 12008.1 ms
实测 DONE     → game_start =  3887.9 ms    ← 这就是**阅读时间**（动态，不是常量）
```

**五档 + 两种 1 番种（真机帧 + 同步实测）**：

| 形态 | 自然 | 补 hold | TOTAL | 阅读时间 | 删句 |
| --- | --- | --- | --- | --- | --- |
| 荣和 1 番种 | 6 764 | **+1 236（地板生效）** | **8 000** | 4 000 | 0 |
| 自摸 1 番种（真机帧） | 8 084 | 0（已在 8000 之上） | **8 084** | 3 916 | 0 |
| 荣和 2 番种 | 8 320 | 0 | 8 320 | 3 680 | 0 |
| 荣和 3 番种 | 9 558 | 0 | 9 558 | 2 442 | 0 |
| 荣和 4 番种 | 10 361 | 0 | 10 361 | 1 639 | 1 |
| 庄家自摸 3 番种 | 9 519 | 0 | 9 519 | 2 481 | 2 |
| 8 条封顶庄家自摸 | 10 151 | 0 | 10 151 | 1 849 | 9 |
| 荒庄（豁免） | 3 907 | 0 | 3 907 | 8 093 | 0 |

**真机复测（`build/game/mahjong_server` seed 20251005，1 番种自摸）**：
`total = 8084`（自然已过地板）→ 实测 `DONE = +8118.6 ms`（+34.6 = 2 帧）；
`game_end → game_start = 12006.5 ms`，`DONE → game_start = 3887.9 ms`（= 阅读时间）。

**B. 其它口径**

| 项 | 结果 |
| --- | --- |
| 每步首句 vs 步切换 | 计划差 **0 ms**；实测出声偏移 +1.6…+6.9 ms ✅ |
| 有 wav / 无 wav 的 digest 比对 | `steps/cues/caption 序列/total/end` **逐字节相同**（仅墙钟抖动）✅ |
| 语音 id 自检 | 52 个可达 id **全部有 wav**，全部在时长表；摘要句 5 条按裁决 B 不接 ✅ |
| manifest 对账 | `checked=57 / missing=0 / drift=0`（容差 0.51 ms，实测是浮点）✅ |
| 静默降级 | `play_voice` 返回 False，无重试、无等待、无异常；日志 1 条 INFO ✅ |
| 跳过 | 180 ms 快进 + 220 ms 收束；局间出站帧 **0** ✅ |
| `grep -nE "(sleep\|time\.delay\|\.wait\()" client/*.py client/render/*.py` | 4 条命中**全部是注释/文档字符串**；带括号的真实调用 **0** ✅ |
| 测试 | `client_headless_test.py` **314 PASS / 0 FAIL**；`docs_drift_test.py` OK ✅ |
| 阶段 A 最坏帧 draw | **39 次** ≤ 56 ✅ |
| WIN duck | +0=1.000 → +100=0.833 → +300=0.333 → +900=**0.251**(−12 dB)，结束后 400 ms 回 1.0；零 `fadeout` ✅ |

### 6.3 后续优化项

- ~~`_draw_timeline()` 死代码~~ —— **已删除**（连同 `BC_DRAW_S1_MS` / `BC_DRAW_S6_MS` / `BC_DRAW_TOTAL_MS`
  三个随之失效的常量）。删前确认：荒庄实测 **3 907 ms**，走的是通用 `_voice_timeline` 路径，
  从未命中过那份 1260 ms 短版。
- `vo_yaku_sum_2..6`（5 条「共 N 项」摘要句）在「逐条删句」方案下**永久闲置**，随库保留
  （约 4 KB），将来若要回到摘要档不用重跑管线。**按 S-2 裁决 B，逐条路径永不接**。
- 启动期一次性解码 57 条 ≈ 5.2 MB RAM；若后续加到 200 条以上，建议另找合法加载点
  （`update`/`render` 栈内不许 `Sound()`，所以不能简单挪到结算页 onEnter）。
