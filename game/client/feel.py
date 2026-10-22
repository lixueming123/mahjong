"""手感表现层：动画状态机与四个开关（FEEL_SPEC v1.7 §2.4 / §3.2 / §6）。

**只做视觉插值，不持任何对局数据**（§7 R1）：所有数据仍从 `game_state` 读，本模块
只维护「进度」与「开关」。动画不得修改 `game_state`（R1），`state_sync` 到达时必须
`snap_all()`（R2）。

实现的常量：
- `F-AN-01` 300 ms 手感预算（本模块只**核算**，不负责裁剪——裁剪在调用点做）
- `F-AN-03` 新状态帧 / 新输入 ⇒ 立即 snap 到终态（一帧内到位）
- `F-AN-04` 距上次成功操作 < 400 ms ⇒ 速度倍率 2.0
- `F-AN-05` 同屏最多 3 个飞行体；第 4 个直接落位（不排队）
- `F-AN-06` 断线 / 托管 / PENDING 未清除 / 结算页 / `--no-feel` ⇒ 不播动画
- `F-AN-10..17` 出牌飞向牌河：140 ms，`ease-out cubic`，56×76 → 30×38（侧家 26×32）
- `F-AN-20..24` 摸牌进场：120 ms，y `+16 → 0`，`ease-out quad`，DRAWN 描边 alpha 0→255
- `F-TP-10` `T` 听牌条开关（关闭时**仍消费 tenpai 帧**）
- `F-CL-08` `H` 可吃碰提示开关
- `F-AU-25` `M` 静音；`F-AU-13` `N` 倒计时滴答（默认关）
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field

# ---------------------------------------------------------------- 常量（§6）

DISCARD_MS = 140.0            # F-AN-12（加速态 70）
DRAW_MS = 120.0               # F-AN-21（加速态 60）
MELD_FLY_MS = 160.0           # §6.4 ①
MELD_SLIDE_MS = 160.0         # §6.4 ②
MELD_FRAME_MS = 120.0         # §6.4 ③
ACCEL_WINDOW_MS = 400.0       # F-AN-04
ACCEL_FACTOR = 2.0            # F-AN-04
MAX_FLIGHTS = 3               # F-AN-05
HAND_TILE_SIZE = (56.0, 76.0)      # F-AN-14 起点
RIVER_TILE_CENTER = (30.0, 38.0)   # F-AN-14 终点（自家 / 对家）
RIVER_TILE_SIDE = (26.0, 32.0)     # F-AN-14 终点（上家 / 下家）
DRAW_RISE_PX = 16.0           # F-AN-22
TP_APPEAR_MS = 160.0          # F-TP-06
TP_UPDATE_MS = 200.0          # F-TP-07
TP_FADE_MS = 120.0            # F-TP-08


def ease_out_cubic(t: float) -> float:
    """F-AN-13：`p = 1 − (1 − t)³`。"""
    t = max(0.0, min(1.0, t))
    return 1.0 - (1.0 - t) ** 3


def ease_out_quad(t: float) -> float:
    """F-AN-22：`p = 1 − (1 − t)²`。"""
    t = max(0.0, min(1.0, t))
    return 1.0 - (1.0 - t) ** 2


def clamp01(t: float) -> float:
    return max(0.0, min(1.0, t))


# ---------------------------------------------------------------- 动画体


@dataclass
class Flight:
    """出牌飞行体（§6.2）。直线、不旋转（F-AN-15）。"""

    seat: int
    tile: str
    x0: float
    y0: float
    x1: float
    y1: float
    w0: float
    h0: float
    w1: float
    h1: float
    started: float
    duration: float

    def progress(self, now: float) -> float:
        if self.duration <= 0:
            return 1.0
        return clamp01((now - self.started) / self.duration)

    def rect(self, now: float) -> tuple[float, float, float, float]:
        p = ease_out_cubic(self.progress(now))
        w = self.w0 + (self.w1 - self.w0) * p
        h = self.h0 + (self.h1 - self.h0) * p
        cx = self.x0 + (self.x1 - self.x0) * p
        cy = self.y0 + (self.y1 - self.y0) * p
        return (cx - w / 2.0, cy - h / 2.0, w, h)


@dataclass
class DrawEntry:
    """摸牌进场（§6.3）：只记录进度，位移与描边在渲染层按 `progress` 取。"""

    hand_index: int
    started: float
    duration: float

    def progress(self, now: float) -> float:
        if self.duration <= 0:
            return 1.0
        return clamp01((now - self.started) / self.duration)


@dataclass
class MeldAnim:
    """鸣牌组牌（§6.4 ①②③，部分重叠：②③ 在 ① 的 60 ms 后起）。"""

    seat: int
    started: float
    duration: float
    has_claimed_tile: bool   # ankan/kakan 没有「被鸣的那张牌」⇒ 跳过 ①

    def progress(self, now: float) -> float:
        if self.duration <= 0:
            return 1.0
        return clamp01((now - self.started) / self.duration)


@dataclass
class TenpaiAnim:
    """听牌条出现/消失动画（F-TP-06 / F-TP-08）。"""

    kind: str          # "appear" | "fade"
    started: float
    duration: float
    new_tiles: list[str] = field(default_factory=list)   # F-TP-07 新增项高亮


class Feel:
    """手感状态机。渲染层只读，逻辑层只调 `on_*` 钩子。"""

    def __init__(self, *, animations_enabled: bool = True) -> None:
        # 四个开关（会话内记忆）
        self.animations_on = animations_enabled   # F-AN-06 `--no-feel`
        self.tenpai_bar_on = True                 # F-TP-10  `T`
        self.claim_hints_on = True                # F-CL-08  `H`
        self.muted = False                        # F-AU-25  `M`
        self.countdown_on = False                 # F-AU-13  `N`（默认关）

        self.flights: list[Flight] = []
        self.draw_entry: DrawEntry | None = None
        self.meld_anim: MeldAnim | None = None
        self.tenpai_anim: TenpaiAnim | None = None
        self._last_op_at: float = -1e9

    # ------------------------------------------------------------ 开关

    @property
    def animations_active(self) -> bool:
        return self.animations_on

    def toggle_tenpai(self) -> bool:
        self.tenpai_bar_on = not self.tenpai_bar_on
        return self.tenpai_bar_on

    def toggle_hints(self) -> bool:
        self.claim_hints_on = not self.claim_hints_on
        return self.claim_hints_on

    def toggle_mute(self) -> bool:
        self.muted = not self.muted
        return self.muted

    def toggle_countdown(self) -> bool:
        self.countdown_on = not self.countdown_on
        return self.countdown_on

    # ------------------------------------------------------------ 节奏

    def note_operation(self, now: float | None = None) -> None:
        """F-AN-04：一次「成功操作」后记时间戳（用于加速态判定）。"""
        self._last_op_at = now if now is not None else time.monotonic()

    def speed(self, now: float | None = None) -> float:
        """F-AN-04：距上次操作 < 400 ms ⇒ 2.0，否则 1.0。"""
        now = now if now is not None else time.monotonic()
        return ACCEL_FACTOR if (now - self._last_op_at) * 1000.0 < ACCEL_WINDOW_MS else 1.0

    def scaled(self, ms: float, now: float | None = None) -> float:
        """把 ms 常量换算成**秒**（`time.monotonic()` 的量纲），并叠加加速态倍率。

        ⚠️ 所有 `duration` 字段一律以**秒**存储，进度一律 `(now - started) / duration`。
        混用量纲会让 140 ms 被当成 140 s（动画永远停在起点）。
        """
        return ms / self.speed(now) / 1000.0

    # ------------------------------------------------------------ 启动

    def start_discard(
        self,
        seat: int,
        tile: str,
        start_rect: tuple[float, float, float, float],
        end_rect: tuple[float, float, float, float],
        *,
        side: bool = False,
        now: float | None = None,
    ) -> bool:
        """F-AN-10/11/12/14。返回是否真的起飞（超过 3 个飞行体则直接落位）。"""
        if not self.animations_on:
            return False
        now = now if now is not None else time.monotonic()
        self.flights = [f for f in self.flights if f.progress(now) < 1.0]
        if len(self.flights) >= MAX_FLIGHTS:
            return False  # F-AN-05：第 4 个直接落位，不排队
        tw, th = RIVER_TILE_SIDE if side else RIVER_TILE_CENTER
        self.flights.append(
            Flight(
                seat=seat,
                tile=tile,
                x0=start_rect[0] + start_rect[2] / 2.0,
                y0=start_rect[1] + start_rect[3] / 2.0,
                x1=end_rect[0] + end_rect[2] / 2.0,
                y1=end_rect[1] + end_rect[3] / 2.0,
                w0=float(start_rect[2]),
                h0=float(start_rect[3]),
                w1=tw,
                h1=th,
                started=now,
                duration=self.scaled(DISCARD_MS, now),
            )
        )
        return True

    def start_draw(self, hand_index: int, now: float | None = None) -> None:
        if not self.animations_on:
            return
        now = now if now is not None else time.monotonic()
        self.draw_entry = DrawEntry(hand_index, now, self.scaled(DRAW_MS, now))

    def start_meld(self, seat: int, has_claimed_tile: bool, now: float | None = None) -> None:
        if not self.animations_on:
            return
        now = now if now is not None else time.monotonic()
        total = MELD_FLY_MS if has_claimed_tile else MELD_SLIDE_MS
        self.meld_anim = MeldAnim(seat, now, self.scaled(total, now), has_claimed_tile)

    def start_tenpai(self, kind: str, new_tiles: list[str] | None = None,
                     now: float | None = None) -> None:
        if not self.animations_on:
            return
        now = now if now is not None else time.monotonic()
        duration = TP_APPEAR_MS if kind == "appear" else TP_FADE_MS
        self.tenpai_anim = TenpaiAnim(kind, now, self.scaled(duration, now), list(new_tiles or []))

    # ------------------------------------------------------------ 冲突处理

    def snap_all(self) -> None:
        """F-AN-03：任何新输入 / 新状态帧 ⇒ 进行中的动画立即到终态（不排队、不续播）。"""
        self.flights = []
        self.draw_entry = None
        self.meld_anim = None

    def update(self, now: float | None = None) -> None:
        """回收已结束的动画体（渲染层不必关心）。"""
        now = now if now is not None else time.monotonic()
        if self.flights:
            self.flights = [f for f in self.flights if f.progress(now) < 1.0]
        if self.draw_entry is not None and self.draw_entry.progress(now) >= 1.0:
            self.draw_entry = None
        if self.meld_anim is not None and self.meld_anim.progress(now) >= 1.0:
            self.meld_anim = None
        if self.tenpai_anim is not None:
            if (now - self.tenpai_anim.started) >= self.tenpai_anim.duration and (
                self.tenpai_anim.kind == "fade"
            ):
                self.tenpai_anim = None

    # ------------------------------------------------------------ 查询（渲染层用）

    def flying_seats(self, now: float | None = None) -> set[int]:
        """正在飞行中的座位集合（渲染牌河时用：该座位最后一张**跳过不画**，避免重影）。"""
        now = now if now is not None else time.monotonic()
        return {f.seat for f in self.flights if f.progress(now) < 1.0}

    def draw_offset(self, hand_index: int, now: float | None = None) -> tuple[float, float]:
        """F-AN-22/23：返回 `(dy, alpha)`；无动画时 `(0, 255)`。"""
        entry = self.draw_entry
        if entry is None or entry.hand_index != hand_index:
            return (0.0, 255.0)
        now = now if now is not None else time.monotonic()
        p = ease_out_quad(entry.progress(now))
        return (DRAW_RISE_PX * (1.0 - p), 255.0 * p)
