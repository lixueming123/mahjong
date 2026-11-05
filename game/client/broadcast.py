"""结算播报 S1（字幕版）：文案生成 + 自适应节拍 + 状态机。

优先级：`docs/SETTLEMENT_BROADCAST_RULINGS.md`（v1.2）> ARCH > UI > MOTION > DESIGN。
本文件是**纯逻辑**：不 import `pygame`、不碰 `GameState`、不碰 `audio`、不发帧。
因此它可以被 `tests/client_headless_test.py` / 任何纯 Python 脚本直接单测。

落点分工（ARCH §4.1）：

- 文案与时间轴的唯一生成位置 = 本模块（`plan_for` / `lines_for` / `timeline`）；
- 状态推进的唯一驱动者 = `app.py`（本模块只吃 `now`，自己不排计时器、不 sleep）；
- `render/result.py` + `render/broadcast.py` 只读 `Broadcast.view()`，不写状态。

**硬禁令**（ARCH §4.2，QA 可断言）：

- ❌ 不得出现「自摸 +N 番」——`tsumo` 恒 0、不是番种（RULINGS C9），行列表里没有
  `tsumo` 这个 kind，结构上不可能生成；
- ❌ 不得把 `win.total` 当番数（`PROTOCOL.md` §5.10）——番数只取 `han_breakdown.total`；
- ❌ 不得对 `yaku[]` 排序（顺序 = 冻结判定序 = 渲染序）；
- ❌ 不得用「平和」表示 0 番（「平和」是 1 番番种名）。

时间轴公式（RULINGS v1.3 §2.1 唯一公式）::

    interlude = game_end.interlude_ms（缺省 12000）
    budget    = clamp(interlude - 1500, 4200, 10500)     # 阅读窗 1500 固定扣除
    # 有语音（tier ≥ 6200）：逐条停留由该句实测时长驱动
    D_item    = max(1200, 该句实测时长 + 150)
    TOTAL     = Σ 各段停留                                     ≤ budget
    # 无语音（tier 5100..6199，或该步的句子被删）：「逐条」步回退到定额
    D         = clamp((budget - 2820 - 96×han) / n, 240, 900)

`n` = 逐条高亮步数 = 番种行数（≤7，§2.1「n 封 7」）+（庄家加成为 1 时 +1）。
**总长硬卡 ≤ budget**：超了按 AUDIO §8.8 的 `delete_rank` 从**番值最小的条目**开始
逐条删句（不截断句子、不加速语速）；被删句的那一步退回 `BC_VOICE_DELETED_DWELL_MS`
的快速推进，番种区仍静态显示该行。

**三档启动门槛按 `budget`，不按 `interlude`**（RULINGS v1.3 §2.1）::

    budget < 5100   ⇒ 完全不播报（退 v2.0 静态页；等价 interlude < 6600，覆盖测试的 0/300）
    5100 ≤ budget < 6200 ⇒ 播报但无语音（字幕 + 逐条高亮 + 「播报中」倒计时行）
    budget ≥ 6200   ⇒ 播报 + 语音（语音步见 `voice_cues` / `timeline().cues`）
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Callable, Iterable, Sequence

from . import vo_durations as VO
from .tiles import LIMIT_LABEL, RIICHI_YAKU_LABEL

log = logging.getLogger("mahjong.client.broadcast")

# ---------------------------------------------------------------- 预算与节拍（RULINGS §2.1）

#: 播报窗上限（RULINGS v1.3 §2.1：**10500**，取代 v1.1 的 9000）
BC_VOICE_BUDGET_MS = 10500
#: 阅读窗（RULINGS v1.3 §2.1：播报后 1500 固定扣除，局间总窗口仍 12000）
BC_READ_WINDOW_MS = 1500
#: 距服务端推进的余量（RULINGS §2.1：400）。budget 公式已用 −1500 覆盖它，**不再二次扣除**
BC_TAIL_RESERVE_MS = 400
#: 旧服务端缺 `interlude_ms` 时的缺省（RULINGS C2：12000）
BC_DEFAULT_INTERLUDE_MS = 12000
#: budget 下限（clamp 地板）
BC_MIN_BUDGET_MS = 4200
#: 三档启动门槛（RULINGS v1.2 §2.1，**按 budget 判，不按 interlude 判**）
#: ① budget < 5100 ⇒ 不播报；② 5100..6199 ⇒ 播报无语音；③ ≥ 6200 ⇒ 播报 + 语音
BC_TIER_SILENT_BUDGET_MS = 5100
BC_TIER_VOICE_BUDGET_MS = 6200

# 六段固定开销（MOTION §2 时间轴表的逐段时长，合计 2620）
BC_S0_MS = 120          # S0 铺满
BC_S1_TAIL_MS = 120     # S1 段尾 hold
BC_S2_MS = 520          # S2 番种合计
BC_S3_MS = 460          # S3 加成
BC_S4_MS = 300          # S4 封顶（未封顶也占位，不然段边界会漂）
BC_S6_MS = 880          # S6 各家支付
BC_S7_MS = 220          # S7 收束
BC_FIXED_MS = (
    BC_S0_MS + BC_S1_TAIL_MS + BC_S2_MS + BC_S3_MS + BC_S4_MS + BC_S6_MS + BC_S7_MS
)  # = 2620

# 倍率阶梯（MOTION §4.4）：段长 = 96×han + 200，clamp [200, 776]
BC_MULT_PER_HAN_MS = 96
BC_MULT_BASE_MS = 200
BC_MULT_MAX_MS = 776

# 单行停留（MOTION §3.2）
BC_MIN_DWELL_MS = 240
BC_MAX_DWELL_MS = 900
#: 番种槽位上限（RULINGS v1.2 §2.3：S1 区 8 槽、行高 26、字号 22；v1.1 的 7 槽作废）
BC_YAKU_SLOTS = 8
#: 逐条高亮步的番种条数上界（RULINGS §2.1「n 封 7」；第 8 槽只静态展示，不单独推进）
BC_MAX_YAKU_STEPS = 7
#: 协议上界（= 番种总数）；超过即服务端缺陷，截断并 WARN
BC_MAX_DYNAMIC_LINES = 14

# 荒庄不走短版公式：`timeline()` 按 budget 分档，荒庄与胜局共用同一套节拍
# （原来这里有一份 1260 ms 的 `_draw_timeline()` 短版，因判据 `lines[0].kind == "draw"`
#  永不命中而长期是死代码，2026-10-23 按 team-lead 指示删除；荒庄实测 4147 ms 走的是通用路径）。

# 交互（MOTION §6.4 / RULINGS §2.5）
BC_SKIP_MIN_MS = 100        # 起播后 100 ms 才可跳（防连点穿透）
BC_SKIP_FAST_MS = 180       # 快进一次走完
BC_SKIP_FAST_MIN_MS = 120   # 剩余不足 180 时用 120
BC_RESUME_CROSSFADE_MS = 200  # 倒计时行 PAUSED → 秒数的 crossfade

#: 语音起播延迟：让 `hand_end` / `draw_hand` 先按原时机起播（+100 ms / +60 ms），
#: 语音一开口就把它 `fadeout(80)`（AUDIO 定稿口径）。**不再等它播完**（ARCH §4.5 的 400 ms 作废）。
BC_VOICE_START_DELAY_MS = 160

# ---------------------------------------------------------------- 语音节拍（RULINGS v1.3）
#
# 逐条停留由**该步实际要念的句子时长**驱动（AUDIO §8.5 / §8.8）：
#
#     D_item = max(1200, 该句实测时长 + 150)
#
# 「该句」= 该步的所有语音 id 时长之和 + 步内句间隔（140/句）。步尾到下一次出声的间隔
# 取 150（同段）/ 240（跨段）/ 320（进 S6 前）；步内相邻两句固定 140。

#: 逐条停留地板（用户反馈「闪太快」后 240 → 1200：每个番种至少要停 1.2 s）
BC_VOICE_ITEM_MIN_MS = 1200
#: 语音步尾到下一次出声的默认间隔（RULINGS「+150」）
BC_VOICE_TAIL_MS = 150
#: 步内相邻两句的间隔（AUDIO §8.5 的 G2 段内）
BC_VOICE_GAP_IN_STEP_MS = 140
#: 跨段间隔（AUDIO §8.5 的 G3 跨段）
BC_VOICE_GAP_CROSS_MS = 240
#: 进 S6（收付）前的间隔（AUDIO §8.5 的 G4 结论前）
BC_VOICE_GAP_BEFORE_S6_MS = 320
#: 被删句的那一步仍要推进（视觉走完全部条目），给一个快速推进时长
BC_VOICE_DELETED_DWELL_MS = 320
#: 短局地板（RULINGS §2.8，v1.6 定稿）：和牌局自然总长不足 8000 时补到 8000，
#: **自然总长 > 8000 的局一律不动**（只保底、不拉伸）；荒庄豁免（3.9 s 合理，无番可讲）。
#: 1 番种局念完 5 句才 6.7 s，观感「没反应过来就完了」；补到 10.5 s 又变成「用静默掩盖内容少」。
BC_PAD_FLOOR_MS = 8000
#: 地板的收尾余量。状态机只能在**帧边界**观察到 `now >= end_at`，补到 `budget` 时
#: 实测会多 1~2 帧（16.7~33.4 ms）；留 2 帧才不会越过预算。
BC_END_SLACK_MS = 40

# ---------------------------------------------------------------- 状态与枚举

STATE_IDLE = "IDLE"
STATE_RUNNING = "RUNNING"
STATE_DONE = "DONE"
STATE_SKIPPED = "SKIPPED"
STATE_ABORTED = "ABORTED"
TERMINAL_STATES = frozenset({STATE_DONE, STATE_SKIPPED, STATE_ABORTED})

# 解析降级（ARCH §4.4 / RULINGS §2.6）
LEVEL_FULL = "L0"
LEVEL_NO_BREAKDOWN = "L1"
LEVEL_NO_YAKU = "L2"
LEVEL_UNUSABLE = "L3"

# 封顶三子态（UI §3.4：判 base+dealer vs 6，不看 total==6）
CAP_NONE = "none"
CAP_EXACT = "exact"
CAP_CAPPED = "capped"

# 支付模式（UI §3.5）
PAY_RON = "ron"
PAY_TSUMO = "tsumo"
PAY_DRAW = "draw"

# 高亮落点（与 `render/broadcast.py` 的槽位一一对应）
FOCUS_YAKU = "yaku"
FOCUS_CHAIN = "chain"
FOCUS_PAY = "pay"

REASON_LABEL = {
    "win_tsumo": "自摸",
    "win_ron": "荣和",
    "draw_exhausted": "荒庄（牌山摸尽）",
}

#: 14 番种 id → 中文名（`result.py` 的同名表迁到这里，成为唯一来源）
YAKU_LABEL = {
    "menzen": "门前清",
    "tanyao": "断幺九",
    "pinfu": "平和",
    "iipeikou": "一杯口",
    "yakuhai": "箭刻",
    "rinshan": "杠上开花",
    "sanshoku_doujun": "三色同顺",
    "ittsuu": "一气通贯",
    "toitoi": "对对和",
    "sanankou": "三暗刻",
    "honitsu": "混一色",
    "shousangen": "小三元",
    "seven_pairs": "七对子",
    "pure_one_suit": "清一色",
}

#: 字幕条字数上限（UI §5.1 R1：≤24 汉字；22 px × 24 = 528 px ≤ 856）
BC_CAPTION_MAX_CHARS = 24

# ---------------------------------------------------------------- 日麻文案（阶段 A 字幕与格子同源）
#
# 日麻没有「庄家加一番 / 自摸不加番 / 封顶 6 番 / 2^番 倍率」——`win.multiplier` 在日麻下
# 只是「点数」的兼容填充（PROTOCOL §12.5）。六段口径改为：番种 → 合计番 → 符 → 满贯分档
# → 点数 → 收付（单位「点」）。下面三个函数是**字幕条与 S3/S4/S5 格子的唯一来源**，
# 渲染层不得再自己拼一遍，否则又会漂移。

#: 日麻役种 → 可复用的 classic 语音（VO 库是「役名 + 番值」的整句资产）。
#: 只有**役名与番值都一致**才复用：`honitsu`（classic 2 番 / 日麻门清 3 番）、`yakuhai`
#: （「箭刻」不含日麻的场风/自风）、`chiitoitsu`/`chinitsu`（id 不同）等一律不念 ——
#: 宁可「语音有损、字幕无损」（RULINGS U2），也不能把 3 番念成 2 番。
RIICHI_VOICE_YAKU: dict[str, tuple[str, int]] = {
    "tanyao": ("vo_yaku_tanyao", 1),
    "pinfu": ("vo_yaku_pinfu", 1),
    "iipeikou": ("vo_yaku_iipeikou", 1),
    "rinshan": ("vo_yaku_rinshan", 1),
    "toitoi": ("vo_yaku_toitoi", 2),
    "sanankou": ("vo_yaku_sanankou", 2),
    "shousangen": ("vo_yaku_shousangen", 2),
    "sanshoku_doujun": ("vo_yaku_sanshoku_doujun", 2),
    "ittsuu": ("vo_yaku_ittsuu", 2),
}


def riichi_fu_text(fu: int | None) -> str:
    """日麻 S3 格子与段 3 字幕的唯一文案（符）。役满 `fu = 0` ⇒ 不计符。"""
    if fu is None:
        return "符数 —"
    return f"{fu} 符" if fu > 0 else "不计符"


def riichi_limit_text(limit: str) -> str:
    """日麻 S4 格子与段 4 字幕的唯一文案（满贯分档；未达显示「未满贯」）。"""
    return LIMIT_LABEL.get(str(limit or ""), "") or "未满贯"


def riichi_points_text(points: int | None) -> str:
    """日麻 S5 格子文案（点数；段 5 字幕另带番符，见 `_build_lines`）。"""
    return f"{points} 点" if points is not None else "— 点"


# ---------------------------------------------------------------- 语音 id 选取
#
# 命名唯一口径 = `client/assets/vo/vo_lines.tsv`（音效产出，本模块**只读不改**），
# 与 AUDIO §8.2 的「扁平、不带段号」一致。全部 57 条 id 在 `vo_durations.VOICE_MS` 里
# 有时长；这里只负责「哪一句什么时候念」，不拼路径、不碰文件。


def _mode_cue(pay_mode: str) -> str:
    """开场：和牌方式（3 选 1）。"""
    if pay_mode == PAY_DRAW:
        return "vo_mode_draw"
    return "vo_mode_tsumo" if pay_mode == PAY_TSUMO else "vo_mode_ron"


def _yaku_cue(row: "YakuRow", riichi_mode: bool = False) -> tuple[str, ...]:
    """段 1 的一条番种整句（AUDIO §8.3 的第 4–19 条）。

    - 箭刻按组数分三条：`vo_yaku_yakuhai_1/2/3`（`han` 是组数，>3 归 3）；
    - 合并行 / 「无番」「明细不可用」不念（合并回的番数已在「合计 N 番」里念到）；
    - 日麻：VO 资产是 classic 的「役名 + 番值」整句，只复用 `RIICHI_VOICE_YAKU`
      里役名与番值都一致的条目，其余只进字幕（不念错比念全重要）。
    """
    if not row.yaku_id:
        return ()
    if riichi_mode:
        entry = RIICHI_VOICE_YAKU.get(row.yaku_id)
        return (entry[0],) if entry is not None and entry[1] == row.han else ()
    if row.yaku_id == "yakuhai":
        groups = min(3, max(1, row.han))
        return (f"vo_yaku_yakuhai_{groups}",)
    cue = f"vo_yaku_{row.yaku_id}"
    return (cue,) if cue in VO.VOICE_MS else ()


def _han_cue(total_han: int | None) -> tuple[str, ...]:
    """段 2：合计番数（`vo_han_0..6`，0 番念「无番」）。"""
    if total_han is None:
        return ()
    cue = f"vo_han_{min(6, max(0, total_han))}"
    return (cue,)


def _mult_cue(mult: int | None) -> tuple[str, ...]:
    """段 5：倍率（`vo_mult_1/2/4/8/16/32/64`）。"""
    if mult is None or mult not in VO.MULT_VALUES:
        return ()
    return (f"vo_mult_{mult}",)


def _pay_cue(pay_mode: str, per_payer: int | None) -> tuple[str, ...]:
    """段 6：收付（荣和 7 条 / 自摸 7 条，都是整句）。"""
    if per_payer is None:
        return ()
    if pay_mode == PAY_TSUMO:
        if per_payer not in VO.PAY_TSUMO_VALUES:
            return ()
        return (f"vo_pay_tsumo_{per_payer}",)
    if per_payer not in VO.PAY_RON_VALUES:
        return ()
    return (f"vo_pay_ron_{per_payer}",)


# ---------------------------------------------------------------- 小工具


def _as_int(value: object) -> int | None:
    """只认真正的 int（`bool` 是 `int` 的子类，必须排除）。"""
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def _clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def interlude_of(game_end: object) -> int:
    """`game_end.interlude_ms`（缺省 12000，RULINGS C2）。`0` 是合法值（服务端立即推进）。"""
    payload = game_end if isinstance(game_end, dict) else {}
    raw = payload.get("interlude_ms")
    value = _as_int(raw)
    if value is None or value < 0:
        return BC_DEFAULT_INTERLUDE_MS
    return value


def budget_of(interlude_ms: int) -> int:
    """播报窗（RULINGS v1.3 §2.1）：`clamp(interlude - 1500, 4200, 10500)`。"""
    return int(_clamp(interlude_ms - BC_READ_WINDOW_MS, BC_MIN_BUDGET_MS, BC_VOICE_BUDGET_MS))


#: 三档门槛的返回值
TIER_OFF = "off"        # budget < 5100：不播报，退 v2.0 静态页
TIER_SILENT = "silent"  # 5100 ≤ budget < 6200：播报（字幕 + 逐条高亮），无语音
TIER_VOICE = "voice"    # budget ≥ 6200：播报 + 语音（语音步在 S2 批实现）


def tier_of(budget_ms: int) -> str:
    """启动门槛三档（RULINGS v1.2 §2.1）——**判据是 `budget`，不是 `interlude`**。

    `--inter-hand-delay-ms 0/300` 的既有 gtest/e2e 经 `budget_of` 落到 4200 ⇒ `off`，
    行为与播报上线前逐帧一致。
    """
    if budget_ms >= BC_TIER_VOICE_BUDGET_MS:
        return TIER_VOICE
    if budget_ms >= BC_TIER_SILENT_BUDGET_MS:
        return TIER_SILENT
    return TIER_OFF


def should_broadcast(budget_ms: int) -> bool:
    """是否播报（①档是唯一不播报的档）。"""
    return tier_of(budget_ms) != TIER_OFF


def mult_ms_of(han: int) -> int:
    """倍率阶梯段长（MOTION §4.4）：`clamp(96×han+200, 200, 776)`。"""
    return int(_clamp(BC_MULT_PER_HAN_MS * max(0, han) + BC_MULT_BASE_MS, BC_MULT_BASE_MS, BC_MULT_MAX_MS))


def _check_caption(text: str, where: str) -> str:
    """字幕条单行不得超 24 字（UI §5.1 R1/R8：超限属规格缺陷，不得靠缩字号救）。"""
    if len(text) > BC_CAPTION_MAX_CHARS:
        log.warning("播报字幕超过 %d 字（%s）：%r", BC_CAPTION_MAX_CHARS, where, text)
    return text


def _call_or(getter: Callable[[int], str] | None, seat: int | None, fallback: str) -> str:
    if not isinstance(seat, int):
        return fallback
    if getter is None:
        return fallback.format(seat=seat)
    try:
        return getter(seat)
    except Exception:  # pragma: no cover - 名称解析器不应影响播报
        log.warning("昵称解析失败（seat=%s），退占位名", seat)
        return fallback.format(seat=seat)


# ---------------------------------------------------------------- 数据结构


@dataclass(frozen=True)
class YakuRow:
    """番种区的一行（固定 7 槽的一槽）。"""

    yaku_id: str
    name: str
    han: int
    show_badge: bool = True    # 荒庄行不画徽标
    show_han: bool = True      # 无番 / 合并行的番数值列留空
    merged_count: int = 0      # >0 ⇒ 第 7 槽合并行「另 N 项 · M 番」


@dataclass(frozen=True)
class Line:
    """播报脚本的一行（ARCH §4.2 的行表）。

    `caption` 进字幕条；`focus` 是该行的高亮落点；`step=True` 参与逐条高亮推进
    （只有番种行与庄家加成行是真·动态步，固定段各自有固定时长）。

    `voice` 是该行要念的语音 id（顺序即播放序；空 = 该行不念）。**时长不在这里**——
    它是 `vo_durations.VOICE_MS` 的函数，因此「有没有 wav」不影响时间轴。
    """

    kind: str
    caption: str
    focus: tuple | None = None
    step: bool = False
    han: int = 0            # 仅 `kind == "total"` 行使用：合计番数（供 MULT(han) 计算）
    voice: tuple[str, ...] = ()


@dataclass(frozen=True)
class VoiceCue:
    """一条到点该出声的语音（`at_ms` 相对播报起点，与 `Step.at_ms` 同一时基）。"""

    at_ms: int
    cue: str
    step_index: int


@dataclass(frozen=True)
class Step:
    """时间轴的一步：什么时候把高亮放到哪里、字幕换成哪一句。"""

    focus: tuple | None
    caption: str
    at_ms: int
    dur_ms: int
    voice: tuple[str, ...] = ()


@dataclass(frozen=True)
class Timeline:
    """节拍结果。全部毫秒，`at_ms` 相对播报起点。"""

    steps: tuple[Step, ...]
    dwell_ms: int
    mult_ms: int
    total_ms: int      # RULINGS §2.1 的 TOTAL（预算口径，≤ budget）
    end_ms: int        # S7 结束时刻（≤ total_ms）
    budget_ms: int
    fixed_ms: int
    merged: bool = False
    # ---- 语音（S2 批）：`cues` 是到点出声的完整队列；空元组 = 这一步没语音
    cues: tuple[VoiceCue, ...] = ()
    s0_ms: int = BC_S0_MS          # 开场（S0 铺满 + 和牌方式那句）实际时长
    voice: bool = False            # 本次时间轴是否走「语音驱动」
    deleted: tuple[str, ...] = ()  # 因超预算被删掉的语音 id（记账用，可断言）
    padded_ms: int = 0             # 补到各段 hold 上的毫秒数（短局地板 8000，见 §2.8）

    def step_index_at(self, elapsed_ms: float) -> int:
        """`elapsed_ms` 时刻应处的高亮步序号（-1 = 还在 S0 铺满里）。"""
        index = -1
        for i, step in enumerate(self.steps):
            if elapsed_ms >= step.at_ms:
                index = i
            else:
                break
        return index

    def s7_at_ms(self) -> int:
        return max(0, self.end_ms - BC_S7_MS)


@dataclass(frozen=True)
class Plan:
    """一局结算页要用的全部派生数据（渲染只读它，不再自己解析协议）。"""

    level: str
    reason: str
    has_winner: bool
    round_text: str
    winner_name: str
    payer_name: str
    winner_seat: int | None
    from_seat: int | None
    yaku_rows: tuple[YakuRow, ...]
    yaku_count: int
    yaku_title: str
    base: int | None
    dealer: int | None
    total_han: int | None
    mult: int | None
    capped_input: int | None
    cap_state: str
    pay_mode: str
    per_payer: int | None
    total_score: int | None
    delta: dict = field(default_factory=dict)
    leader: str = ""
    lines: tuple[Line, ...] = ()
    interlude_ms: int = BC_DEFAULT_INTERLUDE_MS
    budget_ms: int = BC_VOICE_BUDGET_MS
    # 日麻（mode=riichi）：符 / 和牌点数 / 满贯分档 / 是否日麻。classic 下 fu 为 None。
    # `mult` 是 `win.multiplier`（日麻下服务端填点数，仅兼容用）；`points` 才是 `win.points`。
    fu: int | None = None
    points: int | None = None
    limit: str = ""
    riichi_mode: bool = False
    broadcast: bool = False          # 这一局是否应该播报
    static_caption: str = ""         # 不播报 / 播完常驻的结论句
    voice: tuple[str, ...] = ()      # 语音播放序（**删句前**的全量，供日志与断言）

    def captions(self) -> tuple[str, ...]:
        return tuple(line.caption for line in self.lines)


@dataclass(frozen=True)
class BroadcastView:
    """交给渲染层的只读快照（`UiView.bc`）。"""

    state: str
    plan: Plan | None
    focus: tuple | None
    done_focuses: frozenset
    caption: str
    step_index: int
    step_count: int
    done_age_ms: float
    fast_forward: bool
    abort_reason: str | None = None

    @property
    def active(self) -> bool:
        """有 plan ⇒ 播报三区有内容可画（即使不播报也画静态页）。"""
        return self.plan is not None

    @property
    def running(self) -> bool:
        """播报进行中（倒计时必须显示「播报中 · 空格跳过」）。"""
        return self.state == STATE_RUNNING

    @property
    def finished(self) -> bool:
        return self.state in (STATE_DONE, STATE_SKIPPED)

    @property
    def countdown_paused(self) -> bool:
        return self.state == STATE_RUNNING

    def row_state(self, focus: tuple) -> str:
        """某槽位的三态（UI §3.7）：`active` / `done` / `pending`。"""
        if self.focus == focus:
            return "active"
        if focus in self.done_focuses:
            return "done"
        return "pending"


# ---------------------------------------------------------------- 解析（L0–L3）


def _parse_yaku_rows(
    yaku_list: Sequence[dict], has_yaku_key: bool, labels: dict | None = None
) -> tuple[tuple[YakuRow, ...], int]:
    """番种行（UI §3.3 / §3.3.1）：≤7 槽，第 7 槽在超界时降级为合并行。"""
    rows: list[YakuRow] = []
    if not yaku_list:
        if not has_yaku_key:
            # L2 才会走到这里（键缺失）；由调用方改写文案
            # ⚠️ 必须返回**一行的元组**：`(X, 0)` 会让 `rows` 变成单个 YakuRow，
            # `rows[0]` 直接 TypeError（0 番和牌的实测崩溃点）。
            return (YakuRow(yaku_id="", name="番种明细不可用", han=0, show_badge=False, show_han=False),), 0
        # 有键但空 = 真的 0 番（ARCH §4.4 注：不得与「缺失」混判）
        return (YakuRow(yaku_id="", name="无番（0 番）", han=0, show_badge=True, show_han=False),), 0

    count = len(yaku_list)
    if count <= BC_YAKU_SLOTS:
        for item in yaku_list:
            rows.append(_yaku_row(item, labels))
        return tuple(rows), count

    # 防御分支（真实可达上限就是 7，见 UI §3.3.1）：前 6 条逐条 + 第 7 槽合并
    for item in yaku_list[: BC_YAKU_SLOTS - 1]:
        rows.append(_yaku_row(item, labels))
    rest = yaku_list[BC_YAKU_SLOTS - 1:]
    rest_han = sum(_as_int(item.get("han")) or 0 for item in rest)
    rows.append(
        YakuRow(
            yaku_id="",
            name=f"另 {len(rest)} 项",
            han=rest_han,
            show_badge=True,
            show_han=False,
            merged_count=len(rest),
        )
    )
    log.warning("番种条目 %d 项超过 %d 槽，第 7 槽降级为合并行（合计 %d 番）", count, BC_YAKU_SLOTS, rest_han)
    return tuple(rows), count


def _yaku_row(item: dict, labels: dict | None = None) -> YakuRow:
    yid = item.get("id")
    han = _as_int(item.get("han")) or 0
    label = (labels or YAKU_LABEL).get(str(yid), YAKU_LABEL.get(str(yid), str(yid)))
    # 箭刻合并为一条（han = 组数）：han > 1 时写「箭刻 N 组」
    name = f"{label} {han} 组" if (yid == "yakuhai" and han > 1) else label
    return YakuRow(yaku_id=str(yid), name=name, han=han)


def leader_text(results: Iterable[dict], name_of: Callable[[int], str] | None = None) -> str:
    """「本场累计：X 领先」（UI §4.5；由 `result.py` 的 `_leader_text` 迁来）。

    并列时「…、… 并列领先」；四家全 0 也是并列（首局荒庄且无人得分）。
    """
    totals: dict[int, int] = {}
    for entry in results:
        if not isinstance(entry, dict):
            continue
        seat = _as_int(entry.get("seat"))
        total = _as_int(entry.get("match_total"))
        if seat is not None and total is not None:
            totals[seat] = total
    if not totals:
        return ""
    best = max(totals.values())
    leaders = sorted(seat for seat, total in totals.items() if total == best)
    names = "、".join(_call_or(name_of, seat, "座位{seat}") for seat in leaders)
    if len(leaders) > 1:
        return f"本场累计：{names} 并列领先"
    return f"本场累计：{names} 领先"


def plan_for(
    game_end: object,
    *,
    name_of: Callable[[int], str] | None = None,
    rel_label: Callable[[int], str] | None = None,
) -> Plan:
    """`game_end` → 渲染/节拍要用的全部派生数据（含四级降级判定）。"""
    payload = game_end if isinstance(game_end, dict) else {}
    reason = payload.get("reason") if isinstance(payload.get("reason"), str) else "结算"
    interlude = interlude_of(payload)
    budget = budget_of(interlude)

    win = payload.get("win")
    win = win if isinstance(win, dict) else None
    winner_seat = _as_int(win.get("seat")) if win is not None else None
    has_winner = win is not None and winner_seat is not None
    from_seat = _as_int(win.get("from_seat")) if win is not None else None
    if has_winner and win.get("tsumo") is not True and from_seat is None and win.get("tsumo") is None:
        tsumo = True          # 协议：自摸时 `from_seat` 为 null（§5.10）
    else:
        tsumo = bool(win.get("tsumo")) if win is not None else False

    results = [r for r in (payload.get("results") or []) if isinstance(r, dict)] \
        if isinstance(payload.get("results"), list) else []
    delta = {
        seat: value
        for seat, value in ((_as_int(r.get("seat")), _as_int(r.get("delta_score"))) for r in results)
        if seat is not None and value is not None
    }
    round_index = _as_int(payload.get("round_index"))
    renchan = _as_int(payload.get("renchan_count"))
    round_text = _round_text(round_index, renchan, reason)

    if not has_winner:
        return _draw_plan(payload, reason, round_text, interlude, budget, delta, results, name_of)

    # ---- 四级解析降级（ARCH §4.4；「键是否存在」而非 `len(yaku)==0`）
    has_yaku_key = "yaku" in win
    raw_yaku = win.get("yaku")
    yaku_list = (
        [item for item in raw_yaku if isinstance(item, dict) and isinstance(item.get("id"), str)]
        if isinstance(raw_yaku, list)
        else []
    )
    breakdown = win.get("han_breakdown")
    breakdown_ok = isinstance(breakdown, dict) and _as_int(breakdown.get("total")) is not None
    han_field = _as_int(win.get("han"))
    mult_field = _as_int(win.get("multiplier"))

    if not has_yaku_key and not breakdown_ok and han_field is None and mult_field is None:
        level = LEVEL_UNUSABLE
    elif not has_yaku_key:
        level = LEVEL_NO_YAKU
    elif breakdown_ok:
        level = LEVEL_FULL
    else:
        level = LEVEL_NO_BREAKDOWN

    # 日麻：`win.fu` 只有日麻才下发（classic 恒缺）——必须按**键是否存在**判，不能判 `fu > 0`：
    # 役满手 `fu = 0`（riichi_rules.cc `ScoreHand`），`> 0` 会把役满整局误判成 classic 播报
    # （「13 番，32000 倍」+「按 6 番封顶」）。
    fu_field = _as_int(win.get("fu"))
    riichi_mode = fu_field is not None
    # `win.points` 是权威点数；`win.multiplier` 只是兼容填充（PROTOCOL §12.5），留作兜底。
    points_field = _as_int(win.get("points"))
    riichi_points = points_field if points_field is not None else (mult_field if riichi_mode else None)
    rows, yaku_count = _parse_yaku_rows(
        yaku_list, has_yaku_key, RIICHI_YAKU_LABEL if riichi_mode else None
    )
    sum_han = sum(_as_int(item.get("han")) or 0 for item in yaku_list)

    base = dealer = total_han = mult = None
    if level == LEVEL_FULL:
        base = _as_int(breakdown.get("base"))
        if base is None:
            base = sum_han
        dealer = _as_int(breakdown.get("dealer"))
        if dealer is None:
            dealer = 1 if win.get("dealer_bonus") is True else 0
        total_han = _as_int(breakdown.get("total"))
        mult = mult_field if (mult_field is not None and mult_field > 0) else (1 << total_han)
    elif level == LEVEL_NO_BREAKDOWN:
        base = sum_han
        dealer = 1 if win.get("dealer_bonus") is True else 0
        total_han = han_field if han_field is not None else min(base + dealer, 6)
        mult = mult_field if (mult_field is not None and mult_field > 0) else (1 << total_han)
    else:  # LEVEL_NO_YAKU / LEVEL_UNUSABLE：明细不可用，但合计可能还在
        total_han = han_field
        mult = mult_field

    capped_input = base + dealer if (base is not None and dealer is not None) else None
    if capped_input is None:
        cap_state = CAP_NONE
    elif capped_input > 6:
        cap_state = CAP_CAPPED
    elif capped_input == 6:
        cap_state = CAP_EXACT
    else:
        cap_state = CAP_NONE

    if riichi_mode:
        # `han_breakdown.dealer` 在日麻下是服务端按「赢家是否庄家」填的兼容值（§12.5），
        # 但日麻**没有庄家加番，也没有 6 番封顶**。播报侧一律按「无加成、无封顶」解释，
        # 否则役满（13 番）会被念成「N 番超过封顶，按 6 番计」。协议帧保持不动。
        dealer = 0
        capped_input = None
        cap_state = CAP_NONE

    per_payer = _as_int(win.get("per_payer"))
    total_score = _as_int(win.get("total"))
    pay_mode = PAY_TSUMO if tsumo else PAY_RON
    winner_name = _call_or(name_of, winner_seat, "座位{seat}")
    payer_name = _call_or(name_of, from_seat, "—") if not tsumo else "三家"

    lines = _build_lines(
        reason=reason,
        winner_name=winner_name,
        payer_name=payer_name,
        rel_label=rel_label,
        from_seat=from_seat,
        pay_mode=pay_mode,
        rows=rows,
        yaku_count=yaku_count,
        base=base,
        dealer=dealer,
        total_han=total_han,
        mult=mult,
        cap_state=cap_state,
        capped_input=capped_input,
        per_payer=per_payer,
        total_score=total_score,
        riichi_mode=riichi_mode,
        fu=fu_field,
        points=riichi_points,
        limit_text=riichi_limit_text(str(win.get("limit") or "")),
    )

    # RULINGS §2.6（v1.6 修订）：L2（协议里没有 `yaku` 键）**仍播报**，但只播结论
    # （合计番数 / 倍率 / 收付），番种段显示「明细不可用」。L3 全缺才不播报。
    # 理由：L2 是协议异常；异常时让整个功能消失，比降级播报更糟 —— 玩家会以为「这局没有播报」。
    broadcast = level in (LEVEL_FULL, LEVEL_NO_BREAKDOWN, LEVEL_NO_YAKU) and should_broadcast(budget)
    if level == LEVEL_NO_YAKU and broadcast:
        log.warning("game_end 缺 win.yaku 键（L2）：按降级播报处理，只念合计/倍率/收付，番种段显示「明细不可用」")

    return Plan(
        level=level,
        reason=reason,
        has_winner=True,
        round_text=round_text,
        winner_name=winner_name,
        payer_name=payer_name,
        winner_seat=winner_seat,
        from_seat=None if tsumo else from_seat,
        yaku_rows=rows,
        yaku_count=yaku_count,
        yaku_title=_yaku_title(yaku_count, has_winner=True, draw=False),
        base=base,
        dealer=dealer,
        total_han=total_han,
        mult=mult,
        fu=fu_field,
        points=riichi_points,
        limit=str(win.get("limit") or ""),
        riichi_mode=riichi_mode,
        capped_input=capped_input,
        cap_state=cap_state,
        pay_mode=pay_mode,
        per_payer=per_payer,
        total_score=total_score,
        delta=delta,
        leader=leader_text(results, name_of),
        lines=lines,
        interlude_ms=interlude,
        budget_ms=budget,
        broadcast=broadcast,
        voice=_voice_sequence(lines),
        static_caption=_conclusion(
            winner_name=winner_name, total_han=total_han, mult=mult,
            delta=delta.get(winner_seat) if winner_seat is not None else None,
            pay_mode=pay_mode, level=level, fu=fu_field, points=riichi_points,
        ),
    )


def _round_text(round_index: int | None, renchan: int | None, reason: str) -> str:
    label = REASON_LABEL.get(reason, str(reason or "结算"))
    x = (round_index + 1) if round_index is not None else 1
    n = renchan if renchan is not None else 0
    return f"第 {x} 局 · 连庄 {n} · {label}"


def _yaku_title(count: int, *, has_winner: bool, draw: bool) -> str:
    if draw:
        return "番种明细 · 无"
    return f"番种明细 · {count} 项"


def _conclusion(
    *,
    winner_name: str,
    total_han: int | None,
    mult: int | None,
    delta: int | None,
    pay_mode: str,
    level: str,
    fu: int | None = None,
    points: int | None = None,
) -> str:
    """结论句（DESIGN §2.5 的意图 + RULINGS C7：播完停在字幕条上不消失）。"""
    if level in (LEVEL_NO_YAKU, LEVEL_UNUSABLE):
        return "本局结算 · 番种明细不可用"
    if pay_mode == PAY_DRAW:
        return "荒庄 · 本局不计分"
    if fu is not None:
        # 日麻：符 × 番 → 点数（`win.points` 是权威值，`win.multiplier` 只是兼容填充），
        # 屏上不出现「倍」。与阶段 B 的结算明细、阶段 A 的 S5 格子共用同一个数。
        # 役满 `fu = 0` 不写符（分档由 S4 的「役满」承担）。
        delta_text = f"{delta:+d}" if isinstance(delta, int) else "—"
        han_text = f"{total_han}番" if total_han is not None else "—番"
        if fu > 0:
            han_text += f"{fu}符"
        point_value = points if points is not None else mult
        point_text = f"{point_value}点" if point_value is not None else "—点"
        return _check_caption(f"{han_text} {point_text} · {winner_name} {delta_text}", "结论句")
    han_text = f"合计 {total_han} 番" if total_han is not None else "合计 —"
    mult_text = f"{mult} 倍" if mult is not None else "—"
    delta_text = f"{delta:+d}" if isinstance(delta, int) else "—"
    return _check_caption(f"{han_text} · {mult_text} · {winner_name} {delta_text}", "结论句")


def _build_lines(
    *,
    reason: str,
    winner_name: str,
    payer_name: str,
    rel_label: Callable[[int], str] | None,
    from_seat: int | None,
    pay_mode: str,
    rows: Sequence[YakuRow],
    yaku_count: int,
    base: int | None,
    dealer: int | None,
    total_han: int | None,
    mult: int | None,
    cap_state: str,
    capped_input: int | None,
    per_payer: int | None,
    total_score: int | None,
    riichi_mode: bool = False,
    fu: int | None = None,
    points: int | None = None,
    limit_text: str = "",
) -> tuple[Line, ...]:
    """播报脚本（ARCH §4.2 行表 + UI §4.7 模板 + RULINGS §2.2 段结构）。

    段结构（RULINGS §2.2，六段 + 开场）：
    开场（和牌方式）→ ① 番种明细 → ② 番种合计 → ③ 加成 → ④ 封顶 → ⑤ 倍率 → ⑥ 收付。

    日麻同六个槽位，但语义换成「番种 → 合计番 → 符 → 满贯分档 → 点数 → 收付（点）」：
    classic 的庄家加成 / 6 番封顶 / 2^番 倍率在日麻都不存在，语音也只保留能正确复用的资产。
    """
    lines: list[Line] = []
    label = REASON_LABEL.get(reason, "结算")
    # 开场：和牌方式不是段（RULINGS C3），进标题行 + 字幕条首句
    if pay_mode == PAY_RON:
        opening = _check_caption(f"{winner_name} 荣和，{payer_name} 放炮", "开场")
    else:
        opening = _check_caption(f"{winner_name} 自摸", "开场")
    # 和牌方式那句挂在 title 行上（S0 窗口念），winner 行只是同文案的副行、不重复念
    lines.append(Line(kind="title", caption=opening, voice=(_mode_cue(pay_mode),)))
    lines.append(Line(kind="winner", caption=opening))

    # ① 番种明细：`yaku[]` 直渲、不排序；无番写「无番（0 番）」而不是「平和」
    if yaku_count == 0 and rows and rows[0].yaku_id == "" and rows[0].show_badge:
        # 「无番」只念一次（在段 2 的 `vo_han_0`），这里不重复念
        lines.append(Line(kind="yaku", caption="无番和牌", focus=(FOCUS_YAKU, 0), step=True))
    elif rows and rows[0].yaku_id == "" and not rows[0].show_badge:
        # L2：明细不可用 —— 不播报，行只作静态展示
        lines.append(Line(kind="yaku", caption="番种明细不可用", focus=(FOCUS_YAKU, 0)))
    else:
        for i, row in enumerate(rows):
            if row.merged_count:
                caption = _check_caption(f"另 {row.merged_count} 项，{row.han} 番", f"yaku[{i}]")
            elif row.yaku_id == "yakuhai" and row.han > 1:
                caption = _check_caption(f"箭刻 {row.han} 组", f"yaku[{i}]")
            else:
                caption = _check_caption(f"{row.name}，{row.han} 番", f"yaku[{i}]")
            # `han` 只有合并步在用（「另 K 项，H 番」要回算被合并行的番数和）
            lines.append(
                Line(kind="yaku", caption=caption, focus=(FOCUS_YAKU, i), step=True,
                     han=row.han, voice=_yaku_cue(row, riichi_mode))
            )

    # ② 牌型番合计：句库里没有「牌型 N 番」这条（AUDIO §8.2 只到合计番数），故不念
    if base is not None:
        lines.append(
            Line(kind="base", caption=_check_caption(f"牌型 {base} 番", "段2"), focus=(FOCUS_CHAIN, "base"))
        )

    # ③ 加成：庄家 +1 才有独立步（自摸不是番种，RULINGS C9 / §2.4 荣和非庄家整段跳过）
    #    日麻：没有庄家加成，也没有「自摸不加番」（门前清自摸在日麻是 1 番役）——这一格讲符。
    if riichi_mode:
        lines.append(
            Line(kind="dealer", caption=riichi_fu_text(fu), focus=(FOCUS_CHAIN, "dealer"), step=False)
        )
    elif dealer is not None and dealer > 0:
        lines.append(
            Line(kind="dealer", caption="庄家加一番", focus=(FOCUS_CHAIN, "dealer"), step=True,
                 voice=("vo_dealer_bonus",))
        )
    elif pay_mode == PAY_TSUMO:
        # 段 3 的 S3 步：自摸不加番。**只进字幕条**——屏上不得出现「自摸 +0」（RULINGS V6）
        lines.append(
            Line(kind="dealer", caption="自摸不加番", focus=(FOCUS_CHAIN, "dealer"), step=False,
                 voice=("vo_tsumo_no_add",))
        )

    # ④ 封顶：只有真被封顶才推进；「恰好 6 番」不是封顶（UI §3.4）
    #    日麻：没有 6 番封顶，这一格是满贯分档（与 S4 同源）；VO 库无分档句，只进字幕。
    if riichi_mode:
        lines.append(Line(kind="cap", caption=limit_text, focus=(FOCUS_CHAIN, "cap")))
    elif cap_state == CAP_CAPPED:
        lines.append(
            Line(
                kind="cap",
                caption=_check_caption(f"{capped_input} 番超过封顶，按 6 番计", "段4"),
                focus=(FOCUS_CHAIN, "cap"),
                voice=("vo_cap",),
            )
        )

    # ⑤ 倍率（段 5a 合计番数 + 5b 倍率是同一步，UI §4.8）：两句都念
    #    日麻：S5 是点数不是倍率，字幕写「N 番 M 符，X 点」，只念合计番数。
    #    `vo_han_*` 只覆盖 0..6 番，役满（13 番）不念，避免把 13 念成 6；
    #    `vo_mult_*` / `vo_pay_*` 都是 classic 的倍率与「分」整句，日麻一律不用。
    if total_han is not None and (mult is not None or points is not None):
        if riichi_mode:
            point_value = points if points is not None else mult
            fu_part = f" {fu} 符" if fu else ""
            caption = _check_caption(
                f"{total_han} 番{fu_part}，"
                f"{point_value if point_value is not None else '—'} 点",
                "段5",
            )
            voice = _han_cue(total_han) if 0 <= total_han <= 6 else ()
        else:
            caption = _check_caption(f"{total_han} 番，{mult} 倍", "段5")
            voice = _han_cue(total_han) + _mult_cue(mult)
        lines.append(
            Line(kind="total", caption=caption, focus=(FOCUS_CHAIN, "total"),
                 han=total_han, voice=voice)
        )

    # ⑥ 收付（日麻单位是「点」，且 `vo_pay_*` 只有 classic 的 1..64 分句，日麻不念）
    unit = "点" if riichi_mode else "分"
    per_text = per_payer if per_payer is not None else "—"
    total_text = total_score if total_score is not None else "—"
    if pay_mode == PAY_TSUMO:
        caption = _check_caption(f"三家各付 {per_text} {unit}，合计 {total_text} {unit}", "段6")
    else:
        where = _call_or(rel_label, from_seat, "放炮者")
        caption = _check_caption(f"{where}支付 {per_text} {unit}", "段6")
    lines.append(Line(kind="payment", caption=caption, focus=(FOCUS_PAY, "P1"),
                      voice=() if riichi_mode else _pay_cue(pay_mode, per_payer)))

    # 派生：本场累计领先（进底注行，UI §4.5），不是高亮步
    lines.append(Line(kind="leader", caption=""))
    return tuple(lines)


def _draw_plan(
    payload: dict,
    reason: str,
    round_text: str,
    interlude: int,
    budget: int,
    delta: dict,
    results: list[dict],
    name_of: Callable[[int], str] | None,
) -> Plan:
    """荒庄（`win == null`）：六段仍铺满，但只在「无番种 / 不计分 / 四家 0」三处推进。

    日麻（`tenpai[4]` 是 §12.5 的日麻专属键）：流局有**听牌罚符**，且「庄家听牌才连庄」，
    classic 的「本局不计分 / 四家均不计分 · 庄家连庄」在这里是假话（罚符局真的有收付）。
    语音同理：只念「荒庄」，不念 `vo_draw_no_score` / `vo_draw_renchan`（VO 库是 classic 口径）。
    """
    tenpai = payload.get("tenpai")
    riichi_mode = isinstance(tenpai, list)
    if riichi_mode:
        waiting = sum(1 for flag in tenpai if flag is True)
        dealer_seat = _as_int(payload.get("dealer"))
        dealer_tenpai = (
            tenpai[dealer_seat] is True
            if isinstance(dealer_seat, int) and 0 <= dealer_seat < len(tenpai)
            else False
        )
        has_penalty = any(value != 0 for value in delta.values())
        renchan_text = "庄家连庄" if dealer_tenpai else "庄家轮庄"
        lines = (
            Line(kind="title", caption="荒庄 · 牌山摸尽", voice=("vo_mode_draw",)),
            Line(kind="winner", caption="荒庄 · 牌山摸尽"),
            Line(kind="yaku", caption=f"听牌 {waiting} 家 · {'罚符结算' if has_penalty else '无罚符'}",
                 focus=(FOCUS_YAKU, 0), step=True),
            Line(kind="payment", caption=f"听牌 {waiting} 家 · {renchan_text}",
                 focus=(FOCUS_PAY, "P1")),
            Line(kind="leader", caption=""),
        )
        static_caption = f"流局 · 听牌 {waiting} 家 · {renchan_text}"
    else:
        lines = (
            Line(kind="title", caption="荒庄 · 牌山摸尽", voice=("vo_mode_draw",)),
            Line(kind="winner", caption="荒庄 · 牌山摸尽"),
            Line(kind="yaku", caption="本局不计分", focus=(FOCUS_YAKU, 0), step=True,
                 voice=("vo_draw_no_score",)),
            Line(kind="payment", caption="四家均不计分 · 庄家连庄", focus=(FOCUS_PAY, "P1"),
                 voice=("vo_draw_renchan",)),
            Line(kind="leader", caption=""),
        )
        static_caption = "荒庄 · 本局不计分"
    return Plan(
        level=LEVEL_FULL,
        reason=reason,
        has_winner=False,
        round_text=round_text,
        winner_name="—",
        payer_name="—",
        winner_seat=None,
        from_seat=None,
        yaku_rows=(YakuRow(yaku_id="", name="无番种（荒庄）", han=0, show_badge=False, show_han=False),),
        yaku_count=0,
        yaku_title=_yaku_title(0, has_winner=False, draw=True),
        base=None,
        dealer=None,
        total_han=None,
        mult=None,
        capped_input=None,
        cap_state=CAP_NONE,
        pay_mode=PAY_DRAW,
        per_payer=0,
        total_score=0,
        delta=delta,
        leader=leader_text(results, name_of),
        lines=lines,
        interlude_ms=interlude,
        budget_ms=budget,
        riichi_mode=riichi_mode,
        broadcast=should_broadcast(budget),
        voice=_voice_sequence(lines),
        static_caption=static_caption,
    )


def _voice_sequence(lines: Sequence[Line]) -> tuple[str, ...]:
    """行表 → 语音播放序（删句前的全量）。"""
    return tuple(cue for line in lines for cue in line.voice)


def lines_for(
    game_end: object,
    *,
    name_of: Callable[[int], str] | None = None,
    rel_label: Callable[[int], str] | None = None,
) -> list[Line]:
    """文案生成的唯一入口（ARCH §4.2 / RULINGS §2.2）。"""
    return list(plan_for(game_end, name_of=name_of, rel_label=rel_label).lines)


# ---------------------------------------------------------------- 节拍


def _script_line(lines: Sequence[Line], kind: str) -> Line | None:
    for line in lines:
        if line.kind == kind:
            return line
    return None


def timeline(lines: Sequence[Line], budget_ms: int) -> Timeline:
    """自适应节拍（RULINGS v1.3 §2.1）。

    - `budget ≥ 6200`（有语音档）⇒ `_voice_timeline`：逐条停留由该句实测时长驱动，
      `D_item = max(1200, 该句 + 150)`；超预算按 AUDIO §8.8 逐条删句。
    - 其余 ⇒ `_silent_timeline`：**v1.2 的字幕版算法一字不改**（`D` 定额 + MOTION §3.4 合并）。

    返回的 `steps` 是高亮推进的完整时序；`total_ms` 是预算口径的 TOTAL（**恒 ≤ budget**）。
    """
    budget_ms = max(0, int(budget_ms))
    if tier_of(budget_ms) == TIER_VOICE:
        return _voice_timeline(lines, budget_ms)
    return _silent_timeline(lines, budget_ms)


# ---------------------------------------------------------------- 语音节拍（tier ≥ 6200）


def _cues_ms(cues: Sequence[str]) -> int:
    """一串语音的「占位时长」= 各句实测之和 + 步内 140 ms/间隔。"""
    if not cues:
        return 0
    return sum(VO.duration_of(cue) for cue in cues) + BC_VOICE_GAP_IN_STEP_MS * (len(cues) - 1)


def _tail_for(kind: str) -> int:
    """步尾到下一次出声的间隔（AUDIO §8.5 的 G3 / G4；同段用 RULINGS 的 +150）。"""
    if kind == "total":
        return BC_VOICE_GAP_BEFORE_S6_MS     # 进 S6 前 320
    if kind == "yaku":
        return BC_VOICE_TAIL_MS              # 逐条：150（≥ 同段 140）
    return BC_VOICE_GAP_CROSS_MS             # 跨段 240


def _voice_timeline(lines: Sequence[Line], budget_ms: int) -> Timeline:
    """语音驱动节拍：六段固定开销 + 每步「该念的句子有多长」。

    与 `_silent_timeline` 的结构性差别只有两处：

    1. 每步时长 = `max(该段最小值, 句子时长 + 步尾间隔)`，逐条步另有 1200 ms 地板；
    2. 超预算时**不是**合并高亮步，而是按 AUDIO §8.8 从番值最小的句子开始逐条删句
       （视觉仍走完全部条目，被删句那一步退回 320 ms 快速推进）。
    """
    title_line = _script_line(lines, "title")
    total_line = _script_line(lines, "total")
    base_line = _script_line(lines, "base")
    dealer_line = _script_line(lines, "dealer")
    cap_line = _script_line(lines, "cap")
    pay_line = _script_line(lines, "payment")
    han = total_line.han if total_line is not None else 0
    mult_ms = mult_ms_of(han)

    # --- 步骤规格（顺序 = 渲染序；`min` 是该段的固定下限，`cues` 是这一步要念的句子）
    specs: list[dict] = []

    s0_cues = list(title_line.voice) if title_line is not None else []
    s0_ms = max(
        BC_S0_MS,
        BC_VOICE_START_DELAY_MS + _cues_ms(s0_cues) + _tail_for("title") if s0_cues else 0,
    )

    yaku_lines = [line for line in lines if line.step and line.kind == "yaku"]
    yaku_steps = yaku_lines[:BC_MAX_YAKU_STEPS]
    # 第 8 条起不再单独推进（RULINGS「n 封 7」），但**话还是要念**：把它并到最后一步的
    # 待念清单里，否则「清一色五番」这种最高番值反而永远不出声。
    overflow = [cue for line in yaku_lines[BC_MAX_YAKU_STEPS:] for cue in line.voice]
    for line in yaku_steps:
        specs.append({
            "kind": "yaku", "focus": line.focus, "caption": line.caption,
            "min": BC_VOICE_DELETED_DWELL_MS, "cues": list(line.voice),
            "han": line.han, "voice_floor": BC_VOICE_ITEM_MIN_MS,
        })
    if specs and overflow:
        specs[-1]["cues"].extend(overflow)

    specs.append({
        "kind": "base", "focus": base_line.focus if base_line else None,
        "caption": base_line.caption if base_line else "",
        "min": BC_S2_MS, "cues": list(base_line.voice) if base_line else (),
        "han": 0, "voice_floor": 0,
    })
    specs.append({
        "kind": "dealer", "focus": dealer_line.focus if dealer_line else None,
        "caption": dealer_line.caption if dealer_line else "",
        "min": BC_S3_MS, "cues": list(dealer_line.voice) if dealer_line else (),
        "han": 0, "voice_floor": 0, "dealer_step": bool(dealer_line is not None and dealer_line.step),
    })
    specs.append({
        "kind": "cap", "focus": cap_line.focus if cap_line else None,
        "caption": cap_line.caption if cap_line else "",
        "min": BC_S4_MS, "cues": list(cap_line.voice) if cap_line else (),
        "han": 0, "voice_floor": 0,
    })
    specs.append({
        "kind": "total", "focus": total_line.focus if total_line else None,
        "caption": total_line.caption if total_line else "",
        "min": mult_ms, "cues": list(total_line.voice) if total_line else (),
        "han": 0, "voice_floor": 0,
    })
    specs.append({
        "kind": "payment", "focus": pay_line.focus if pay_line else None,
        "caption": pay_line.caption if pay_line else "",
        "min": BC_S6_MS, "cues": list(pay_line.voice) if pay_line else (),
        "han": 0, "voice_floor": 0,
    })

    # 空步（既没有字幕、也没有高亮落点、也没有句子）不占时间：荒庄的 S2/S3/S4 就是三块
    # 死时间（原来 1280 ms 静默），语音版把它们去掉，不然「本局不计分」和「庄家连庄」
    # 之间会空 2.9 秒。胜局的六段都有字幕，一个都不会被去掉。
    specs = [spec for spec in specs
             if spec["focus"] is not None or spec["caption"] or spec["cues"]]

    def dur_of(spec: dict) -> int:
        value = spec["min"]
        if spec["cues"]:
            value = max(value, _cues_ms(spec["cues"]) + _tail_for(spec["kind"]))
            value = max(value, spec["voice_floor"])
        return value

    def total_ms_of(durs: Sequence[int]) -> int:
        return s0_ms + BC_S1_TAIL_MS + BC_S7_MS + sum(durs)

    durs = [dur_of(spec) for spec in specs]
    total = total_ms_of(durs)

    # --- 超预算：逐条删句（AUDIO §8.8 的 delete_rank；保底 5 项永不删）
    deleted: list[str] = []
    if total > budget_ms:
        victims: list[int] = []
        # rank 1..n：番种整句，`han` 升序；同 `han` 时 `yaku[]` 索引降序（保判定序靠前的）
        ranked = sorted(
            (
                (spec["han"], -index, index)
                for index, spec in enumerate(specs)
                if spec["kind"] == "yaku" and spec["cues"]
            ),
            key=lambda item: (item[0], item[1]),
        )
        victims.extend(item[2] for item in ranked)
        # rank n+1：庄家加成（「自摸不加番」是保底，不在此列）
        for index, spec in enumerate(specs):
            if spec["kind"] == "dealer" and "vo_dealer_bonus" in spec["cues"]:
                victims.append(index)
        # rank n+2：封顶（最后才动）
        for index, spec in enumerate(specs):
            if spec["kind"] == "cap" and spec["cues"]:
                victims.append(index)

        for index in victims:
            if total <= budget_ms:
                break
            spec = specs[index]
            while spec["cues"] and total > budget_ms:
                cue = spec["cues"].pop(0)
                deleted.append(cue)
                total = total - durs[index] + dur_of(spec)
                durs[index] = dur_of(spec)
            # 这一步现在没有句子了：退回固定下限（逐条步 = 320 ms 快速推进）
            durs[index] = dur_of(spec)
        total = total_ms_of(durs)

    # --- 短局地板（RULINGS §2.8，v1.6 定稿）：**只保底、不拉伸**
    #
    #     pad_target = max(自然总长, min(8000, budget - 40))    # 仅和牌局
    #     荒庄豁免（3.9 s 合理，无番可讲）
    #
    # `natural > 8000` 的局一律不动 —— 1 番种局念完 5 句只有 6.7 s，观感「没反应过来就完了」；
    # 但把短局补到 10.5 s 又变成「用静默掩盖内容少」。地板解决前者，不制造后者。
    # 40 = 2 帧收尾余量（状态机只在帧边界观察到 `now >= end_at`）。
    #
    # 荒庄的判据用行表推断（不强依赖调用方传参）：荒庄既没有「牌型 N 番」也没有「合计/倍率」行。
    is_draw = _script_line(lines, "base") is None and _script_line(lines, "total") is None
    natural = total
    padded = 0
    pad_target = natural if is_draw else max(
        natural, min(BC_PAD_FLOOR_MS, budget_ms - BC_END_SLACK_MS)
    )
    if natural < pad_target:
        holds = [i for i, spec in enumerate(specs)
                 if spec["kind"] in ("base", "dealer", "cap", "total", "payment")]
        if holds:
            delta = pad_target - natural
            share, remainder = divmod(delta, len(holds))
            for index in holds:
                durs[index] += share
            durs[holds[-1]] += remainder
            padded = delta
            total = total_ms_of(durs)

    # --- 兜底：连「无语音的最小值」都装不下 ⇒ 全部压到下限，再不行就按预算截断
    if total > budget_ms:
        floor_total = total_ms_of([spec["min"] for spec in specs])
        if floor_total > budget_ms:
            log.warning("播报预算 %d ms 低于六段最小开销 %d ms，按预算截断", budget_ms, floor_total)
        else:
            log.info("播报预算 %d ms 装不下全部句子，压到六段最小开销 %d ms", budget_ms, floor_total)
        durs = [spec["min"] for spec in specs]
        total = floor_total

    # --- 落成 Step + 语音队列
    steps: list[Step] = []
    cues: list[VoiceCue] = []
    cursor = s0_ms
    at = BC_VOICE_START_DELAY_MS
    for cue in s0_cues:
        cues.append(VoiceCue(at, cue, -1))
        at += VO.duration_of(cue) + BC_VOICE_GAP_IN_STEP_MS
    for index, spec in enumerate(specs):
        steps.append(Step(spec["focus"], spec["caption"], cursor, durs[index], tuple(spec["cues"])))
        at = cursor
        for cue in spec["cues"]:
            cues.append(VoiceCue(at, cue, index))
            at += VO.duration_of(cue) + BC_VOICE_GAP_IN_STEP_MS
        cursor += durs[index]
        if yaku_steps and index == len(yaku_steps) - 1:
            cursor += BC_S1_TAIL_MS

    end_ms = cursor + BC_S7_MS
    first_yaku = next((d for spec, d in zip(specs, durs) if spec["kind"] == "yaku"), 0)
    if deleted:
        log.info(
            "播报超预算（budget=%d ms）：删句 %s，最终 %d ms",
            budget_ms, deleted, min(end_ms, budget_ms),
        )
    return Timeline(
        steps=tuple(steps),
        dwell_ms=first_yaku,
        mult_ms=mult_ms,
        total_ms=min(total, budget_ms),
        end_ms=min(end_ms, budget_ms),
        budget_ms=budget_ms,
        fixed_ms=max(0, total - sum(durs)),
        merged=False,
        cues=tuple(cues),
        s0_ms=s0_ms,
        voice=True,
        deleted=tuple(deleted),
        padded_ms=padded,
    )


def _silent_timeline(lines: Sequence[Line], budget_ms: int) -> Timeline:
    """自适应节拍（RULINGS v1.2 §2.1 唯一公式 + MOTION §3.4 合并降级）。

    返回的 `steps` 是高亮推进的完整时序；`total_ms` 是预算口径的 TOTAL（**恒 ≤ budget**）。
    """
    total_line = _script_line(lines, "total")
    han = total_line.han if total_line is not None else 0
    mult_ms = mult_ms_of(han)

    dynamic = [line for line in lines if line.step]
    if len(dynamic) > BC_MAX_DYNAMIC_LINES:
        log.warning("动态播报行 %d 条超过协议上界 %d，截断", len(dynamic), BC_MAX_DYNAMIC_LINES)
        dynamic = dynamic[:BC_MAX_DYNAMIC_LINES]

    dealer_line = _script_line(lines, "dealer")
    # 庄家是**推进步**；「自摸不加番」的 S3 行只是字幕（step=False），不占预算
    dealer_is_step = dealer_line is not None and dealer_line.step
    # `D` 的分母 n = 番种步（≤7，§2.1「n 封 7」）+ 庄家步（≤1）
    yaku_lines = [line for line in dynamic if line.kind == "yaku"][:BC_MAX_YAKU_STEPS]

    # avail 与 §2.1 的 (budget - 2820 - 96×han) 同值（2820 = 2620 + 200）
    avail = budget_ms - BC_FIXED_MS - mult_ms
    # 装得下几步（步长地板 240）；一步都装不下 ⇒ 全部静态展示、不推进
    n_max = max(0, int(avail // BC_MIN_DWELL_MS))

    keep_yaku = len(yaku_lines)
    keep_dealer = 1 if dealer_is_step else 0
    merged = False
    if len(yaku_lines) + keep_dealer > n_max:
        # MOTION §3.4：逐条推进的条数让位给预算；多出的番种行仍然静态显示
        keep_dealer = 1 if (dealer_is_step and n_max > 1) else 0
        keep_yaku = max(0, n_max - keep_dealer)
        keep_yaku = min(keep_yaku, len(yaku_lines))
        merged = keep_yaku < len(yaku_lines)
        log.info(
            "播报预算偏紧（budget=%d ms, han=%d）：逐条高亮 %d/%d 条番种（合并 %s）",
            budget_ms, han, keep_yaku, len(yaku_lines), merged,
        )

    n_hi = keep_yaku + keep_dealer
    # 必须 min（不能只 clamp）：240 地板会让「装不下 240 的预算」超支，
    # 而 TOTAL ≤ budget 是硬约束，不能有例外分支。`n_max` 的取法已保证 dwell ≥ 240。
    dwell = min(int(avail // n_hi), BC_MAX_DWELL_MS) if n_hi else 0

    steps: list[Step] = []
    cursor = BC_S0_MS
    for i, line in enumerate(yaku_lines[:keep_yaku]):
        caption = line.caption
        if merged and i == keep_yaku - 1 and keep_yaku < len(yaku_lines):
            rest = yaku_lines[keep_yaku:]
            # 合并步的文案：「另 {k} 项，{h} 番」（与番种区末槽同句）
            rest_han = sum(item.han for item in rest)
            caption = _check_caption(f"另 {len(rest)} 项，{rest_han} 番", "合并步")
        steps.append(Step(line.focus, caption, cursor, dwell))
        cursor += dwell

    # S1 段尾 hold
    cursor += BC_S1_TAIL_MS
    # S2 番种合计
    base_line = _script_line(lines, "base")
    steps.append(Step(base_line.focus if base_line else None, base_line.caption if base_line else "", cursor, BC_S2_MS))
    cursor += BC_S2_MS
    # S3 加成（庄家步若被保留则在此窗口推进；「自摸不加番」只换字幕不推进）
    steps.append(
        Step(
            (FOCUS_CHAIN, "dealer") if (dealer_line is not None and keep_dealer) else None,
            dealer_line.caption if dealer_line is not None else "",
            cursor,
            BC_S3_MS,
        )
    )
    cursor += BC_S3_MS
    # S4 封顶（未封顶也占位）
    cap_line = _script_line(lines, "cap")
    steps.append(Step(cap_line.focus if cap_line else None, cap_line.caption if cap_line else "", cursor, BC_S4_MS))
    cursor += BC_S4_MS
    # S5 倍率
    steps.append(
        Step(
            total_line.focus if total_line else None,
            total_line.caption if total_line else "",
            cursor,
            mult_ms,
        )
    )
    cursor += mult_ms
    # S6 收付
    pay_line = _script_line(lines, "payment")
    steps.append(Step(pay_line.focus if pay_line else None, pay_line.caption if pay_line else "", cursor, BC_S6_MS))
    cursor += BC_S6_MS
    end_ms = cursor + BC_S7_MS

    total_ms = BC_FIXED_MS + mult_ms + n_hi * dwell
    return Timeline(
        steps=tuple(steps),
        dwell_ms=dwell,
        mult_ms=mult_ms,
        total_ms=total_ms,
        end_ms=min(end_ms, total_ms),
        budget_ms=budget_ms,
        fixed_ms=BC_FIXED_MS,
        merged=merged,
    )


# ---------------------------------------------------------------- 状态机


class Broadcast:
    """播报状态机（ARCH §4.1）：全部由 `now` 驱动，无内部计时器、无 sleep、不发帧。

    `IDLE → RUNNING → DONE`，另有 `SKIPPED`（用户跳过）与 `ABORTED`（终态，不可恢复）。
    """

    def __init__(self, *, enabled: bool = True) -> None:
        self._enabled = enabled
        self._plan: Plan | None = None
        self._timeline: Timeline | None = None
        self._state = STATE_IDLE
        self._t0 = 0.0
        self._end_at = 0.0
        self._s7_at = 0.0
        self._skip_until = 0.0
        self._focus: tuple | None = None
        self._done_focuses: set[tuple] = set()
        self._step_index = -1
        self._done_at: float | None = None
        self._abort_reason: str | None = None
        self._fast_forward = False
        # 语音队列（S2 批）：到点由 `collect_voice()` 交给 VOICE 通道，本模块只排时刻
        self._cues: tuple[VoiceCue, ...] = ()
        self._cue_i = 0

    # ---- 只读属性

    @property
    def state(self) -> str:
        return self._state

    @property
    def plan(self) -> Plan | None:
        return self._plan

    @property
    def timeline(self) -> Timeline | None:
        return self._timeline

    @property
    def running(self) -> bool:
        return self._state == STATE_RUNNING

    @property
    def finished(self) -> bool:
        """播报是否已收束（DONE/SKIPPED）——阶段 A → 阶段 B 的切换点。"""
        return self._state in (STATE_DONE, STATE_SKIPPED)

    @property
    def fast_forward(self) -> bool:
        """是否正处于「跳过后的 180 ms 快进」里（此时不再出声）。"""
        return self._fast_forward

    @property
    def started_at(self) -> float:
        """起播时刻（`time.monotonic()` 绝对时间；只读，诊断用）。"""
        return self._t0

    @property
    def end_at(self) -> float:
        """S7 收束时刻（绝对时间；只读，诊断用）。"""
        return self._end_at

    # ---- 生命周期

    def start(
        self,
        game_end: object,
        now: float,
        *,
        name_of: Callable[[int], str] | None = None,
        rel_label: Callable[[int], str] | None = None,
    ) -> bool:
        """`game_end` 到达 ⇒ 生成 plan（永远生成，静态页也要画）；返回是否真的起播。"""
        if self._state in (STATE_RUNNING,):
            self.abort("restart")
        self._plan = plan_for(game_end, name_of=name_of, rel_label=rel_label)
        self._timeline = timeline(self._plan.lines, self._plan.budget_ms) if self._plan.broadcast else None
        self._t0 = now
        self._focus = None
        self._done_focuses = set()
        self._step_index = -1
        self._done_at = None
        self._abort_reason = None
        self._fast_forward = False
        self._cues = ()
        self._cue_i = 0
        if not (self._plan.broadcast and self._enabled):
            self._state = STATE_IDLE
            log.info(
                "本局不播报：level=%s interlude=%d enabled=%s",
                self._plan.level, self._plan.interlude_ms, self._enabled,
            )
            return False
        assert self._timeline is not None
        self._state = STATE_RUNNING
        # 语音落位（RULINGS §2.4 / AUDIO §8.2）：`timeline.cues` 就是 VOICE 通道的入队时刻。
        # 本模块只排时刻，**不加载、不播放**——`assets/vo/` 为空时时间轴逐帧不变。
        self._cues = self._timeline.cues
        self._cue_i = 0
        if self._cues:
            log.debug("播报语音队列：%s", [cue.cue for cue in self._cues])
        self._end_at = now + self._timeline.end_ms / 1000.0
        self._s7_at = now + self._timeline.s7_at_ms() / 1000.0
        self._skip_until = 0.0
        log.info(
            "播报起播：level=%s 步数=%d dwell=%dms TOTAL=%dms 预算=%dms 语音=%d 句%s",
            self._plan.level, len(self._timeline.steps), self._timeline.dwell_ms,
            self._timeline.total_ms, self._timeline.budget_ms, len(self._cues),
            f"（删句 {len(self._timeline.deleted)}）" if self._timeline.deleted else "",
        )
        return True

    # ---- 语音（纯读取：返回「此刻该响的句子」，播放由 `audio.py` 负责）

    def collect_voice(self, now: float) -> tuple[str, ...]:
        """取出所有已到点的语音 id（同一帧可能到点两句，按序返回）。**不重复返回**。"""
        if self._state != STATE_RUNNING or self._fast_forward:
            return ()
        elapsed_ms = (now - self._t0) * 1000.0
        due: list[str] = []
        while self._cue_i < len(self._cues) and self._cues[self._cue_i].at_ms <= elapsed_ms:
            due.append(self._cues[self._cue_i].cue)
            self._cue_i += 1
        return tuple(due)

    def next_voice_in(self, now: float) -> float | None:
        """距下一句还有多少毫秒（`None` = 没有下一句）。供 WIN duck 的 120 ms 前瞻用。"""
        if self._state != STATE_RUNNING or self._fast_forward:
            return None
        if self._cue_i >= len(self._cues):
            return None
        elapsed_ms = (now - self._t0) * 1000.0
        return max(0.0, self._cues[self._cue_i].at_ms - elapsed_ms)

    def voice_finished(self) -> bool:
        """本场播报的语音是否已全部发出（或已终态）。"""
        return self._cue_i >= len(self._cues)

    def update(self, now: float) -> None:
        """唯一的时间驱动点（`app.update()` 每帧调一次）。"""
        if self._state != STATE_RUNNING:
            return
        if now >= self._end_at:
            self.finish(now)
            return
        if self._fast_forward:
            if now >= self._s7_at:
                self._fast_forward = False
            return
        timeline = self._timeline
        if timeline is None:
            self.finish(now)
            return
        elapsed_ms = (now - self._t0) * 1000.0
        index = timeline.step_index_at(elapsed_ms)
        if index == self._step_index:
            return
        self._step_index = index
        if index < 0:
            self._focus = None
            return
        step = timeline.steps[index]
        self._focus = step.focus
        for previous in timeline.steps[:index]:
            if previous.focus is not None:
                self._done_focuses.add(previous.focus)

    def skip(self, now: float) -> bool:
        """跳过（MOTION §6.4 / RULINGS §2.5）：一次走完，仍走 180 ms 快进 + 220 ms 收束。

        **不发任何帧**（局间出站帧必须为 0）。返回是否被接受。
        """
        if self._state != STATE_RUNNING or self._timeline is None:
            return False
        elapsed_ms = (now - self._t0) * 1000.0
        if elapsed_ms < BC_SKIP_MIN_MS:
            return False          # 起播 100 ms 内不响应（防连点穿透）
        if elapsed_ms >= self._timeline.s7_at_ms():
            return False          # 已在 S7 收束里，跳过没有意义
        fast_ms = BC_SKIP_FAST_MS
        if self._end_at - now < (BC_SKIP_FAST_MS + BC_S7_MS) / 1000.0:
            fast_ms = BC_SKIP_FAST_MIN_MS
        # 所有未到步同帧直达终态（UI §3.7：不闪回、不清空）
        for step in self._timeline.steps:
            if step.focus is not None:
                self._done_focuses.add(step.focus)
        self._focus = None
        self._step_index = len(self._timeline.steps)
        self._fast_forward = True
        self._cues = ()
        self._cue_i = 0          # 跳过 ⇒ 后续句子一律不念（已响的那句由 app 停掉）
        self._s7_at = now + fast_ms / 1000.0
        self._end_at = self._s7_at + BC_S7_MS / 1000.0
        self._skip_until = self._end_at
        log.info("播报跳过：已播 %.0f ms / %d ms（不发帧）", elapsed_ms, self._timeline.end_ms)
        return True

    def finish(self, now: float) -> None:
        if self._state in TERMINAL_STATES:
            return
        skipped = self._state == STATE_RUNNING and self._fast_forward
        self._state = STATE_SKIPPED if skipped else STATE_DONE
        self._fast_forward = False
        self._focus = None
        self._done_at = now
        if self._timeline is not None:
            for step in self._timeline.steps:
                if step.focus is not None:
                    self._done_focuses.add(step.focus)

    def abort(self, reason: str) -> None:
        """终态（`game_start` / `match_end` / 断线 / 切屏）。**不缓冲任何帧**（ARCH §3.7 X3）。"""
        if self._state == STATE_ABORTED:
            return
        if self._state == STATE_RUNNING and self._timeline is not None:
            log.warning("播报被 %s 打断（不缓冲帧）", reason)
        self._state = STATE_ABORTED
        self._abort_reason = reason
        self._focus = None
        self._fast_forward = False
        self._cues = ()
        self._cue_i = 0

    def clear(self) -> None:
        """`feel.snap_all()` 同款：清空到 IDLE（下场开局前调用）。"""
        self._state = STATE_IDLE
        self._plan = None
        self._timeline = None
        self._focus = None
        self._done_focuses = set()
        self._step_index = -1
        self._done_at = None
        self._abort_reason = None
        self._fast_forward = False
        self._cues = ()
        self._cue_i = 0

    # ---- 只读视图

    def view(self) -> BroadcastView:
        plan = self._plan
        done_age = -1.0
        if self._state in (STATE_DONE, STATE_SKIPPED) and self._done_at is not None:
            done_age = 0.0   # 具体毫秒由渲染层用 ui.now 差分；此处只标记「已收束」
        caption = ""
        if plan is not None:
            caption = plan.static_caption if self._state in TERMINAL_STATES or not self.running else ""
            if self._focus is None and self._state == STATE_RUNNING and self._step_index < 0:
                caption = plan.lines[0].caption if plan.lines else plan.static_caption
            elif self._focus is not None and self._timeline is not None:
                step = self._timeline.steps[self._step_index]
                caption = step.caption or caption
        tooltip = self._done_at
        return BroadcastView(
            state=self._state,
            plan=plan,
            focus=self._focus,
            done_focuses=frozenset(self._done_focuses),
            caption=caption,
            step_index=self._step_index,
            step_count=len(self._timeline.steps) if self._timeline else 0,
            done_age_ms=done_age,
            fast_forward=self._fast_forward,
            abort_reason=self._abort_reason,
        )


# ---------------------------------------------------------------- 语音落位

#: 语音总开关（`--no-feel` 之外的运行时闸门；`app.py` 在关掉时完全不动 VOICE 通道）。
VOICE_ENABLED = True


def voice_cues(plan: Plan | None) -> tuple[str, ...]:
    """整局要念的语音 id 播放序（**删句前**的全量；实际播放序见 `timeline().cues`）。

    命名唯一口径 = `client/assets/vo/vo_lines.tsv`（AUDIO §8.2 的「扁平、不带段号」）：:

        vo_mode_<tsumo|ron|draw>
        vo_yaku_<id> / vo_yaku_yakuhai_<1|2|3>
        vo_tsumo_no_add / vo_dealer_bonus
        vo_han_<0..6> / vo_cap / vo_mult_<1..64>
        vo_pay_<ron|tsumo>_<n> / vo_draw_no_score / vo_draw_renchan

    L2（`LEVEL_NO_YAKU`）**不返回空**：番种段没得念，但结论三句（合计/倍率/收付）照念。
    只有 L3（`LEVEL_UNUSABLE`，连合计与倍率都没有）才没有可念的内容。
    """
    if plan is None or plan.level == LEVEL_UNUSABLE:
        return ()
    return plan.voice
