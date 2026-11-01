"""牌桌界面（UI_UX_SPEC §4；v3.0 起按参考视频《明星三缺一 2004》复原）。

图层顺序（关键在**桌布压在人物之上**：桌沿那条线就是把人物切开的那条线）：

    art.hall()                       背景：糊掉的室内照片（暖灰墙 + 货架）
    三家人物半身像 + 名牌            都贴在上沿，下半身等着被桌布切掉
    art.table_cloth()                粉色桌布：米白斜格 + 浅蓝线稿 + 桌沿高光
    牌                               他家手牌背 / 副露 / 牌河；本家副露 + 手牌

**本家没有立绘**（参考视频是第一人称视角），只有右下角一块分数牌 + 系统菜单。
坐标全部取自 `theme.py`（= §4.3 v3.0 坐标表）。本模块是纯读取渲染：
- 他家手牌**只**通过 `draw_tile_back` 以 `hand_count` 张牌背呈现（§4.6 / §7.2）；
- 他家的 `SeatView` 结构里没有任何牌面字段可读。
"""

from __future__ import annotations

import logging
import math

import pygame

from .. import theme as T
from ..game_state import SCREEN_TABLE, GameState
from ..tiles import multiset_remove, tile_label
from ..view import UiView
from . import art
from . import broadcast as broadcast_view
from . import widgets as W

log = logging.getLogger("mahjong.client.table")

BREATH_PERIOD_MS = 1200.0     # §4.9 呼吸周期 1.2 s
FLASH_PERIOD_MS = 400.0       # §6.1 危险色闪烁 400 ms 相位

#: 牌桌透视：桌面近端（下沿）= 1.0，远端（上沿）= 0.90。
#: v3.0 调浅（0.80 → 0.90）：参考视频的桌布几乎是正对镜头的梯形，只微微收窄。
PERSPECTIVE_NEAR = 1.0
PERSPECTIVE_FAR = 0.84

#: 副露与手牌同一行时，两组之间的间距
HAND_MELD_GAP = 18
#: 侧家牌河每下一行的水平偏移（顺着桌边斜）
SIDE_RIVER_SHEAR = 0
#: 副露排布用的「自然宽度」上限（4 组杠刚好放得下，不会触发收缩）
MELD_ROW_MAX_W = 560


# ---------------------------------------------------------------- 布局计算


def depth_scale(y: float) -> float:
    """桌面某一行（屏幕 y）的透视缩放系数。"""
    top = T.TABLE_FELT[1]
    bottom = top + T.TABLE_FELT[3]
    t = max(0.0, min(1.0, (y - top) / max(1, bottom - top)))
    return PERSPECTIVE_FAR + (PERSPECTIVE_NEAR - PERSPECTIVE_FAR) * t


def project(rect: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    """把一个「桌面坐标」矩形按透视投到屏幕上（位置与尺寸同时按深度缩放）。

    所有摆件（牌河 / 副露 / 手牌墙 / 手牌）都过同一个函数，所以牌、桌布、木边
    三者的透视是一致的；命中测试也用投影后的矩形，点哪张就是哪张。
    """
    cx = rect[0] + rect[2] / 2
    scale = depth_scale(rect[1] + rect[3] / 2)
    width = max(2, int(round(rect[2] * scale)))
    height = max(2, int(round(rect[3] * scale)))
    x = int(round(640 + (cx - 640) * scale - width / 2))
    return (x, int(rect[1]), width, height)


def selection_hint(face: str) -> str:
    """「已选中」提示文案（UI_UX_SPEC v1.4 §5.4 ②，逐字冻结）。

    牌名走 §5.3 文案公式（`tiles.tile_label`），**不再显示线上牌码**；
    调用点不得自行拼接牌名。
    """
    return f"已选中 {tile_label(face)}，再点一次或按 Enter 打出"


def _meld_row_groups(state: GameState) -> list:
    """本家副露在「手牌行」里的分组（自然宽度）。"""
    if not state.melds:
        return []
    return meld_layout((0, 0, MELD_ROW_MAX_W, T.HAND_TILE_H), state.melds, vertical=False)


def meld_row_width(state: GameState) -> int:
    groups = _meld_row_groups(state)
    return groups[-1][0].right if groups else 0


def _hand_row_origin(state: GameState) -> tuple[int, int, int]:
    """手牌行的 (起始 x, 副露宽, 间隔)：副露在左、手牌在右，整体居中。"""
    n = len(state.hand)
    hand_w = n * T.HAND_PITCH - 2 if n > 0 else 0
    meld_w = meld_row_width(state)
    gap = HAND_MELD_GAP if (meld_w > 0) else 0
    total_w = meld_w + gap + hand_w
    x0 = max(T.R_SELF_HAND[0], 640 - total_w // 2)   # 左边界由 R_SELF_HAND 让位给分数牌
    return x0, meld_w, gap


def hand_layout(state: GameState) -> list[tuple[int, int, int, int]]:
    """§4.5（v2.5）：手牌与副露同一行，整体在 `R_SELF_HAND` 内水平居中。

    `total_w = meld_w + gap + (n*58 - 2)`；`x0 = max(150, 640 - total_w//2)`；`y0 = 566`。
    无副露时与原公式完全一致（14 张 ⇒ 235..1045）。
    """
    n = len(state.hand)
    if n <= 0:
        return []
    x0, meld_w, gap = _hand_row_origin(state)
    start = x0 + meld_w + gap
    return [
        project((start + i * T.HAND_PITCH, T.HAND_Y, T.HAND_TILE_W, T.HAND_TILE_H))
        for i in range(n)
    ]


def meld_row_layout(state: GameState) -> list:
    """本家副露的实际绘制矩形（与手牌同一行，靠左）。"""
    if not state.melds:
        return []
    x0, meld_w, _gap = _hand_row_origin(state)
    area = (x0, T.R_SELF_MELD[1], meld_w, T.R_SELF_MELD[3])
    return meld_layout(area, state.melds, vertical=False)


def hand_hit_test(state: GameState, pos: tuple[float, float]) -> int | None:
    """命中测试（§4.5）：含选中/摸牌标记造成的上移。"""
    rects = hand_layout(state)
    for i, rect in enumerate(rects):
        lift = 0
        if state.selected_index == i:
            lift = T.HAND_SELECT_LIFT
        elif state.drawn_index == i:
            lift = T.HAND_DRAWN_LIFT
        elif state.pending and state.pending_index == i:
            lift = -6
        x, y, w, h = rect
        if x <= pos[0] < x + w and y - lift <= pos[1] < y - lift + h:
            return i
    return None


def center_river_rects(area: tuple[int, int, int, int], count: int) -> list[tuple[int, int, int, int]]:
    """中央牌河（自家 / 对家）：6×4，超过 24 张切换紧凑 7×5（§4.7）。"""
    ox, oy = area[0], area[1]
    if count <= T.RIVER_CENTER_CAPACITY:
        tw, th = T.RIVER_CENTER_TILE
        cw, ch = T.RIVER_CENTER_COL_PITCH, T.RIVER_CENTER_ROW_PITCH
        cols = T.RIVER_CENTER_COLS
    else:
        tw, th = T.RIVER_CENTER_COMPACT_TILE
        cw, ch = T.RIVER_CENTER_COMPACT_COL_PITCH, T.RIVER_CENTER_COMPACT_ROW_PITCH
        cols = T.RIVER_CENTER_COMPACT_COLS
    out = []
    for i in range(count):
        row, col = divmod(i, cols)
        out.append((ox + col * cw, oy + row * ch, tw, th))
    return out


def _side_river_col_y(col: int, cols: int) -> float:
    """横排槽在桌沿上的 y：col 0 靠近本家，递增往远端。"""
    near_y = float(T.HAND_Y - 22)
    far_y = float(T.SIDE_RIVER_TOP)
    if cols <= 1:
        return near_y
    t = col / (cols - 1)
    return near_y + (far_y - near_y) * t


def side_river_centers(left: bool, count: int) -> list[tuple[float, float]]:
    """侧家牌河：6 张横排沿桌边，满行后向桌心叠下一行（与真实牌河一致）。"""
    cols = T.RIVER_SIDE_COLS
    out: list[tuple[float, float]] = []
    for index in range(count):
        row, col = divmod(index, cols)
        if row >= T.RIVER_SIDE_ROWS:
            break
        inset = T.SIDE_RIVER_INSET + row * float(T.RIVER_SIDE_ROW_PITCH)
        y = _side_river_col_y(col, cols)
        x, y = T.side_point_at_screen_y(left, inset, y)
        out.append((x, y))
    return out


def _side_river_tile_size() -> tuple[int, int]:
    """(沿轨长, 朝桌心的厚)；与 ``RIVER_SIDE_TILE`` 一致，不压小弃牌。"""
    tw, th = T.RIVER_SIDE_TILE
    along = max(18, tw - 2)
    across = max(14, th - 2)
    return along, across


def discard_flight_start(state: GameState, rel: int) -> tuple[float, float, float, float]:
    """他家出牌动画起点：侧家从手牌轨近端起飞，对家从牌背行，不再从桌心。"""
    if rel == 2:
        tw, th = T.OPP_BACK_TILE
        rect = T.R_OPP_BACK
        return (float(rect[0]), float(rect[1]), float(tw), float(th))
    if rel in (1, 3):
        slots = side_hand_slots(rel, 1)
        if slots:
            slot = slots[0]
            return (float(slot[0]), float(slot[1]), float(slot[2]), float(slot[3]))
    return (float(T.R_CENTER[0]), float(T.R_CENTER[1]), float(T.R_CENTER[2]), float(T.R_CENTER[3]))


def discard_flight_end(
    state: GameState,
    seat: int,
    rel: int,
    discard_count: int,
) -> tuple[float, float, float, float] | None:
    """出牌飞行终点，与牌河绘制槽位一致。"""
    if rel == 0:
        rects = center_river_rects(T.R_SELF_RIVER, discard_count)
    elif rel == 2:
        rects = center_river_rects(T.R_OPP_RIVER, discard_count)
    elif rel == 1:
        return _side_discard_flight_end_rect(False, discard_count)
    else:
        return _side_discard_flight_end_rect(True, discard_count)


def _side_discard_flight_end_rect(left: bool, count: int) -> tuple[float, float, float, float] | None:
    centers = side_river_centers(left, count)
    if not centers:
        return None
    cx, cy = centers[-1]
    along, across = _side_river_tile_size()
    return (
        float(cx - across / 2.0),
        float(cy - along / 2.0),
        float(across),
        float(along),
    )


def side_river_rects(rect: tuple[int, int, int, int], count: int) -> list[tuple[int, int, int, int]]:
    """侧家牌河命中/飞行动画（中心在轨上）。"""
    left = rect[0] + rect[2] // 2 < T.LOGICAL_W // 2
    along, across = _side_river_tile_size()
    out: list[tuple[int, int, int, int]] = []
    for x, y in side_river_centers(left, count):
        out.append((
            int(round(x - across / 2.0)),
            int(round(y - along / 2.0)),
            across,
            along,
        ))
    return out


def meld_layout(
    area: tuple[int, int, int, int], melds, vertical: bool = False
) -> list[tuple[pygame.Rect, list[tuple[tuple[int, int, int, int], str | None]]]]:
    """副露分组布局（§4.8）：组内牌 28×36 pitch 30，组间距 8；空间不足时等比收缩 pitch。

    v2.0 暗杠空数组形态：`tiles == []` 按占位 4 张（face=None ⇒ 由调用方画牌背，
    §4.8 两形态之二），组宽按 4 张计。
    """
    if not melds:
        return []
    gap = T.MELD_GROUP_GAP
    outer = 2  # 组框内边距
    tw, th = T.MELD_TILE

    def width_of(meld) -> int:
        return len(meld.tiles) if meld.tiles else 4

    total_tiles = sum(width_of(m) for m in melds)

    if vertical:
        max_w = area[2]
        pitch = T.MELD_TILE_PITCH
        out = []
        y = area[1]
        for meld in melds:
            height = T.MELD_ROW_HEIGHT - 4
            n = width_of(meld)
            group_w = min(max_w, max(1, n) * pitch - 2 + 2 * outer)
            group_rect = pygame.Rect(area[0], y, group_w, height)
            inner_pitch = pitch
            if (n * inner_pitch - 2 + 2 * outer) > group_w:
                inner_pitch = max(14, (group_w - 2 * outer + 2) // max(1, n))
            faces: list[str | None] = list(meld.tiles) or [None] * 4
            tiles = [
                ((area[0] + outer + i * inner_pitch, y + 2, min(tw, inner_pitch - 2), height - 4), face)
                for i, face in enumerate(faces)
            ]
            out.append((group_rect, tiles))
            y += T.MELD_ROW_HEIGHT
        return out

    max_w = area[2]
    pitch = T.MELD_TILE_PITCH
    widths = [max(1, width_of(m)) * pitch - 2 + 2 * outer for m in melds]
    total = sum(widths) + gap * (len(melds) - 1)
    if total > max_w and total_tiles > 0:
        # 收缩：解出货架宽度 sum(n_i*pitch) + 常量 = max_w
        constant = len(melds) * (2 * outer - 2) + gap * (len(melds) - 1)
        pitch = max(16, int((max_w - constant) / total_tiles))
        widths = [max(1, width_of(m)) * pitch - 2 + 2 * outer for m in melds]
    out = []
    x = area[0]
    for meld, width in zip(melds, widths):
        group_rect = pygame.Rect(x, area[1], width, area[3])
        tiles = []
        faces: list[str | None] = list(meld.tiles) or [None] * 4  # 暗杠空数组 ⇒ 4 张牌背占位
        for i, face in enumerate(faces):
            tile_w = min(tw, pitch - 2)
            tiles.append(((x + outer + i * pitch, area[1], tile_w, area[3]), face))
        out.append((group_rect, tiles))
        x += width + gap
    return out


# ---------------------------------------------------------------- 绘制


_LAYERS: dict[tuple, pygame.Surface] = {}


def cloth_layer(width: int, height: int) -> pygame.Surface:
    """粉色桌布（整屏尺寸，梯形以外透明）：只在首次或换分辨率时构建。"""
    key = ("cloth", width, height)
    layer = _LAYERS.get(key)
    if layer is None:
        felt = T.TABLE_FELT
        layer = art.table_cloth(
            width, height, felt[1], T.TABLE_FAR_HALF, T.TABLE_NEAR_HALF,
        )
        _LAYERS[key] = layer
    return layer


def hall_layer(width: int, height: int) -> pygame.Surface:
    """人物身后的糊背景：整屏缓存。"""
    key = ("hall", width, height)
    layer = _LAYERS.get(key)
    if layer is None:
        layer = art.hall(width, height)
        _LAYERS[key] = layer
    return layer


def render(surface: pygame.Surface, ui: UiView, feel=None) -> None:
    width, height = surface.get_size()
    # ① 背景（糊掉的室内照片）
    surface.blit(hall_layer(width, height), (0, 0))
    # ② 三家人物：先画人，**再画桌布**——桌沿那条线负责把他们切开
    for seat in range(4):
        _draw_seat_character(surface, ui, seat)
    surface.blit(cloth_layer(width, height), (0, 0))
    # ④ 角标（参考视频没有顶栏，房间/局号压到右上角两行小字）
    _draw_hud(surface, ui)
    # ⑤ 牌：他家手牌背 / 副露 / 牌河
    for seat in range(4):
        _draw_seat_tiles(surface, ui, seat, feel)
    # ⑥ 桌心情报 + 本家面前的牌山
    _draw_center(surface, ui)
    _draw_near_wall(surface, ui)
    # ⑦ 本家手牌（含副露）
    _draw_self_hand(surface, ui, feel)
    _draw_toast(surface, ui)


# ---------------------------------------------------------------- 可吃碰提示（FEEL_SPEC §3）

#: 每个动作从手牌里消耗的**同面张数** / 构成张（F-CL-02）
_CLAIM_HAND_COUNT = {"pong": 2, "kong": 3, "ankan": 4, "kakan": 1}


def claim_hand_faces(state, window) -> dict[str, int]:
    """F-CL-02：本家手牌里参与鸣牌的牌面 → 需要高亮的张数。

    `chow` 取**所有吃法的并集**（每条吃法贡献其 2 张）；多条吃法同一牌面时取最大张数。
    **不改 `options`、不给建议**（§3.1 红线 1/2）：这里只算「哪些牌要描边」。
    """
    out: dict[str, int] = {}
    for opt in window.options:
        action = opt.get("action")
        tiles = [t for t in (opt.get("tiles") or []) if isinstance(t, str)]
        if action == "chow":
            for face in tiles[:2]:
                out[face] = max(out.get(face, 0), 1)
            continue
        count = _CLAIM_HAND_COUNT.get(action)
        if count is None or not tiles:
            continue
        face = tiles[0]
        out[face] = max(out.get(face, 0), count)
    return out


def draw_flight_tile(
    surface: pygame.Surface,
    ui: UiView,
    rect: tuple[float, float, float, float],
    face: str,
    *,
    side: bool = False,
    seat: int = -1,
    feel=None,
) -> None:
    """F-AN-14/15：飞行体牌面。侧家用与牌河相同的 rail 贴图；中央仍用正视牌。"""
    cx = rect[0] + rect[2] / 2.0
    cy = rect[1] + rect[3] / 2.0
    if side:
        left = ui.state.rel_index(seat) == 3
        angle = T.side_face_angle(left)
        along, across = _side_river_tile_size()
        outline = None
        mark_claimed = (
            seat == ui.state.last_discard_seat
            and claim_highlight_active(ui.state)
            and (feel is None or getattr(feel, "claim_hints_on", True))
        )
        if mark_claimed:
            if ui.state.pending:
                outline = (T.PANEL_EDGE, 1.0, 1)
            else:
                outline = (T.ACTIVE, _active_alpha(ui.now), 2)
        sprite = _rail_face_sprite(
            ui.fonts, face, along, across, angle, outline=outline,
        )
        dest = sprite.get_rect(center=(int(round(cx)), int(round(cy))))
        surface.blit(sprite, dest)
        return
    projected = project((int(rect[0]), int(rect[1]), max(2, int(rect[2])), max(2, int(rect[3]))))
    W.draw_tile(
        surface,
        ui.fonts,
        projected,
        face,
        rank_size=T.FS_TILE_RIVER_RANK,
        suit_size=T.FS_TILE_RIVER_SUIT,
        honor_size=T.FS_TILE_Z_RIVER_CENTER,
    )


def claim_highlight_active(state) -> bool:
    """F-CL-04/06：只在「claim 窗口且有非 pass 选项」时才画高亮。"""
    window = state.window
    if window is None or window.window != "claim":
        return False
    return any(opt.get("action") not in (None, "pass") for opt in window.options)


# ---------------------------------------------------------------- HUD


def _ellipsize(text: str, font: pygame.font.Font, max_width: int) -> str:
    """按像素宽度截断（不换行、不溢出座位牌）。"""
    if font.size(text)[0] <= max_width:
        return text
    for cut in range(len(text) - 1, 0, -1):
        candidate = text[:cut] + "…"
        if font.size(candidate)[0] <= max_width:
            return candidate
    return ""


def _draw_hud(surface: pygame.Surface, ui: UiView) -> None:
    """v3.0：参考视频**没有顶栏**。

    房间 / 局号 / 阶段 / 连接状态全部压成右上角两行 12 px 小字——不铺底色、不画分隔线，
    顶部整条让给三家名牌。异常（非法操作、张数异常）优先占用第二行。
    """
    state, fonts = ui.state, ui.fonts
    box = (T.LOGICAL_W - 196, 5, 188, 40)
    dot_color = T.POS if ui.connected else T.NEG
    pygame.draw.circle(surface, dot_color, (box[0] + 5, box[1] + 8), 4)
    pygame.draw.circle(surface, art.shade(dot_color, 1.4), (box[0] + 5, box[1] + 8), 2)
    line1 = f"房间 {state.room_id or '—'}" if ui.connected else "未连接"
    if ui.connected and ui.rtt_ms is not None and ui.rtt_ms > 1000:
        line1 += "（延迟高）"
    W.draw_text(
        surface, line1, fonts.get(T.FS_MICRO), T.TEXT_DIM,
        (box[0] + 14, box[1], box[2] - 14, 18), W.ALIGN_LEFT,
    )

    if ui.illegal_count > 0:
        line2 = f"非法操作 {ui.illegal_count}/10"
        color = T.NEG if ui.illegal_count >= 7 else T.WARN
    elif state.hand_warning:
        line2, color = "张数异常", T.WARN
    elif state.game_started:
        line2 = f"{state.round_index + 1}/{state.round_count} · {state.phase}"
        color = T.TEXT_DIM
    else:
        line2, color = state.phase, T.TEXT_DIM
    W.draw_text(
        surface, line2, fonts.get(T.FS_MICRO), color,
        (box[0], box[1] + 19, box[2], 18), W.ALIGN_RIGHT,
    )


# ---------------------------------------------------------------- 人物与名牌


def _active_alpha(now: float) -> float:
    phase = (now * 1000.0 % BREATH_PERIOD_MS) / BREATH_PERIOD_MS
    return 0.6 + 0.4 * (0.5 + 0.5 * math.sin(2 * math.pi * phase))


def _seat_bust_rect(seat: int, rel: int) -> tuple[int, int, int, int]:
    """rel 0 = 本家，不出立绘（v3.0）；返回空矩形让调用方跳过。"""
    if rel == 2:
        return T.R_OPP_BUST
    if rel == 1:
        return T.R_SIDE_R_BUST
    if rel == 3:
        return T.R_SIDE_L_BUST
    return (0, 0, 0, 0)


def _seat_name_rect(rel: int) -> tuple[int, int, int, int]:
    if rel == 2:
        return T.R_OPP_NAME
    if rel == 1:
        return T.R_SIDE_R_NAME
    if rel == 3:
        return T.R_SIDE_L_NAME
    return T.R_SELF_PLATE


def _wind_of(seat: int, state: GameState) -> str:
    """门风：以庄家为東，逆时针排東南西北。"""
    dealer = state.dealer if state.dealer is not None else 0
    return "東南西北"[(seat - dealer) % 4]


def _draw_plate(surface: pygame.Surface, ui: UiView, seat: int, rel: int, *, active: bool) -> None:
    """参考视频的座位名牌：上一条深色名字条（风位徽标 + 名字），下一条洋红分数条。

    分数条原本放的是 `$ 96300`——我们这局没有累计分，就把它用来放张数（金色数字），
    配色与节奏和参考一致。
    """
    state, fonts = ui.state, ui.fonts
    view = state.seat_view(seat)
    rect = _seat_name_rect(rel)
    name_h = T.PLATE_NAME_H
    edge = T.ACTIVE if active else (146, 112, 100)

    surface.blit(
        art.strip(rect[2], name_h, T.PLATE_NAME, edge=edge, radius=4),
        (rect[0], rect[1]),
    )
    badge = pygame.Rect(rect[0] + 4, rect[1] + 3, 22, name_h - 6)
    pygame.draw.rect(
        surface, T.ACTIVE if active else T.PLATE_SCORE, badge, border_radius=3,
    )
    W.draw_text(
        surface, _wind_of(seat, state), fonts.get(T.FS_MICRO, bold=True), (28, 20, 24),
        (badge.x, badge.y, badge.w, badge.h), W.ALIGN_CENTER,
    )

    name_font = fonts.get(T.FS_SMALL, bold=True)
    label = _ellipsize(
        f"{state.name_of(seat)} · {state.rel_label(seat)}", name_font, rect[2] - 40,
    )
    W.draw_text(
        surface, label, name_font, T.WARN if view.disconnected else T.TEXT,
        (rect[0] + 30, rect[1], rect[2] - 34, name_h), W.ALIGN_LEFT,
    )

    score = (rect[0], rect[1] + name_h, rect[2], rect[3] - name_h)
    surface.blit(
        art.strip(score[2], score[3], T.PLATE_SCORE, edge=art.shade(T.PLATE_SCORE, 1.45), radius=4),
        (score[0], score[1]),
    )
    face_font = fonts.get(T.FS_MICRO, bold=True)
    if view.disconnected:
        W.draw_text(surface, "断线托管", face_font, (255, 216, 216),
                    (score[0] + 8, score[1], score[2] - 16, score[3]), W.ALIGN_LEFT)
    else:
        W.draw_text(surface, f"{view.hand_count} 张", face_font, (255, 232, 176),
                    (score[0] + 8, score[1], score[2] - 60, score[3]), W.ALIGN_LEFT)
    if state.dealer == seat:
        tag = pygame.Rect(score[0] + score[2] - 30, score[1] + 2, 26, score[3] - 4)
        pygame.draw.rect(surface, T.DEALER, tag, border_radius=3)
        W.draw_text(surface, "庄", fonts.get(T.FS_MICRO, bold=True), (48, 30, 8),
                    (tag.x, tag.y, tag.w, tag.h), W.ALIGN_CENTER)


def _draw_seat_character(surface: pygame.Surface, ui: UiView, seat: int) -> None:
    """座位名牌（v3.1：**人物立绘已移除**）。

    只保留名牌——上一条深色名字条（风位 + 昵称），下一条洋红分数条（张数 / 庄）。
    人物一旦去掉，牌桌本身就成了画面主体，名牌必须自己站得住。
    """
    state = ui.state
    if seat == state.my_seat:
        return
    rel = state.rel_index(seat)
    if rel == 0:
        return
    active = state.current_seat == seat and state.screen == SCREEN_TABLE
    _draw_plate(surface, ui, seat, rel, active=active)


def _draw_dealer_badge(surface: pygame.Surface, ui: UiView, rect: tuple[int, int, int, int]) -> None:
    """庄家角标（金色圆牌 + 「庄」）。"""
    if ui.state.dealer is None:
        return
    size = min(rect[2], rect[3])
    center = (rect[0] + rect[2] // 2, rect[1] + rect[3] // 2)
    pygame.draw.circle(surface, T.DEALER, center, size // 2)
    pygame.draw.circle(surface, art.shade(T.DEALER, 1.35), center, size // 2, width=1)
    W.draw_text(
        surface, "庄", ui.fonts.get(max(10, size - 5), bold=True), (52, 34, 8),
        (rect[0], rect[1], rect[2], rect[3]), W.ALIGN_CENTER,
    )


# ---------------------------------------------------------------- 牌


#: 侧家手牌整条（已按桌沿旋转）的缓存：单帧只 blit 一次
_SIDE_STRIP_CACHE: dict[tuple, tuple[pygame.Surface, int, int]] = {}
#: 旋转后的单张牌背 / 牌山叠，按朝向缓存
_RAIL_SPRITE_CACHE: dict[tuple, pygame.Surface] = {}


def side_hand_slots(rel: int, count: int) -> list[tuple[int, int, int, int]]:
    """侧家手牌槽位（近端在前）。"""
    if count <= 0:
        return []
    left = rel != 1
    points = T.side_rail_points(left, T.SIDE_HAND_INSET, count)
    along, across = _rail_tile_size(count)
    return [
        (
            int(round(x - across / 2.0)),
            int(round(y - along / 2.0)),
            across,
            along,
        )
        for x, y in points
    ]


def _rail_pitch(count: int) -> float:
    """相邻槽位的间距。不超过 13 张时用满轨间距，近端固定。"""
    sample = max(2, count)
    points = T.side_rail_points(True, T.SIDE_HAND_INSET, sample)
    return math.hypot(points[1][0] - points[0][0], points[1][1] - points[0][1])


def _rail_tile_size(count: int, *, hand: bool = False) -> tuple[int, int]:
    """(沿轨长, 朝桌心的厚)。长边跟槽距走，厚按立牌比例。"""
    pitch = _rail_pitch(count)
    if hand:
        pitch *= T.SIDE_HAND_TILE_FILL
    along = max(16, int(round(pitch)))
    across = max(12, int(round(along * 0.72)))
    return along, across


def _rotated_offset(ox: float, oy: float, angle: float) -> tuple[float, float]:
    """屏幕坐标下 pygame 的逆时针旋转（y 向下）。"""
    theta = math.radians(angle)
    cos_t = math.cos(theta)
    sin_t = math.sin(theta)
    return (ox * cos_t + oy * sin_t, -ox * sin_t + oy * cos_t)


def _rail_back_sprite(
    along: int, across: int, angle: float, *, plain: bool = False,
) -> pygame.Surface | None:
    key = ("back", along, across, round(angle, 2), plain)
    cached = _RAIL_SPRITE_CACHE.get(key)
    if cached is not None:
        return cached
    base = W.tile_back_surface(across, along, plain=plain)
    if base is None:
        return None
    rotated = pygame.transform.rotate(base, angle)
    if len(_RAIL_SPRITE_CACHE) > 32:
        _RAIL_SPRITE_CACHE.clear()
    _RAIL_SPRITE_CACHE[key] = rotated
    return rotated


def _draw_side_backs(surface: pygame.Surface, left: bool, count: int) -> None:
    """没有整条贴图时（不足 7 张），按槽位逐张画牌背。近端固定。"""
    points = T.side_rail_points(left, T.SIDE_HAND_INSET, count)
    angle = T.side_face_angle(left)
    along, across = _rail_tile_size(count, hand=True)
    sprite = _rail_back_sprite(along, across, angle, plain=True)
    for x, y in points:
        if sprite is None:
            W.draw_tile_back(
                surface,
                (int(round(x - across / 2)), int(round(y - along / 2)), across, along),
            )
            continue
        rect = sprite.get_rect(center=(int(round(x)), int(round(y))))
        surface.blit(sprite, rect.topleft)


def _blit_side_strip(
    surface: pygame.Surface,
    strip: pygame.Surface,
    left: bool,
    count: int,
    inset: float,
    full: int,
    from_far: bool = False,
    *,
    tile_fill: float = 1.0,
) -> None:
    """把整条橙色牌墙贴到一条侧边上。手牌近端锚死；牌山远端锚死。"""
    key = (count, left, inset, full, from_far, round(tile_fill, 3))
    cached = _SIDE_STRIP_CACHE.get(key)
    if cached is None:
        points = T.side_rail_points(left, inset, count, full, from_far)
        angle = T.side_face_angle(left)
        raw_w, raw_h = strip.get_size()
        if count <= 1:
            center_span = float(_rail_tile_size(count, hand=tile_fill < 1.0)[0])
        else:
            center_span = math.hypot(
                points[-1][0] - points[0][0], points[-1][1] - points[0][1],
            )
        image_span = raw_w * (count - 1) / count if count > 1 else float(raw_w)
        scale = center_span / image_span if image_span else 1.0
        scale *= tile_fill
        scaled_w = max(1, int(round(raw_w * scale)))
        scaled_h = max(1, int(round(raw_h * scale)))
        scaled = pygame.transform.smoothscale(strip, (scaled_w, scaled_h))
        tile_along = scaled_w / count
        # 图像 +x 旋转后若指向远端，左端就是近端；否则右端是近端。
        axis = _rotated_offset(1.0, 0.0, angle)
        rail = (points[-1][0] - points[0][0], points[-1][1] - points[0][1])
        left_is_near = axis[0] * rail[0] + axis[1] * rail[1] >= 0
        if left_is_near:
            src_x = tile_along / 2.0
        else:
            src_x = scaled_w - tile_along / 2.0
        offset = _rotated_offset(src_x - scaled_w / 2.0, 0.0, angle)
        rotated = pygame.transform.rotate(scaled, angle)
        rw, rh = rotated.get_size()
        blit_x = int(round(points[0][0] - (rw / 2.0 + offset[0])))
        blit_y = int(round(points[0][1] - (rh / 2.0 + offset[1])))
        if len(_SIDE_STRIP_CACHE) > 24:
            _SIDE_STRIP_CACHE.clear()
        cached = (rotated, blit_x, blit_y)
        _SIDE_STRIP_CACHE[key] = cached
    image, blit_x, blit_y = cached
    surface.blit(image, (blit_x, blit_y))


def _draw_side_hand(surface: pygame.Surface, rel: int, hand_count: int) -> None:
    """侧家扣着的手牌：沿桌沿，近端固定；1..18 张均用橙色 side 贴图（不足 7 张从 side_07 裁）。"""
    if hand_count <= 0:
        return
    shown = min(hand_count, T.OPP_BACK_MAX)
    if hand_count > T.OPP_BACK_MAX:
        log.warning("他家 hand_count=%d 超过 %d，超出部分不绘制", hand_count, T.OPP_BACK_MAX)
    left = rel != 1
    strip = W.side_strip_surface(shown)
    if strip is None:
        _draw_side_backs(surface, left, shown)
        return
    _blit_side_strip(
        surface,
        strip,
        left,
        shown,
        T.SIDE_HAND_INSET,
        T.SIDE_HAND_FULL,
        tile_fill=T.SIDE_HAND_TILE_FILL,
    )


def _flat_orange(count: int) -> pygame.Surface | None:
    """横放的橙色牌墙（对家手牌 / 本家与对家牌山）。"""
    return W.side_strip_surface(count)


def _blit_flat_orange(
    surface: pygame.Surface, count: int, rect: tuple[int, int, int, int],
) -> None:
    """把橙色牌墙横放进矩形里，保持比例并居中。张数少时自然变短。"""
    if count <= 0:
        return
    image = _flat_orange(count)
    if image is None:
        return
    iw, ih = image.get_size()
    scale = min(rect[3] / ih, rect[2] / iw)
    width = max(1, int(round(iw * scale)))
    height = max(1, int(round(ih * scale)))
    key = ("flat", count, width, height)
    scaled = _RAIL_SPRITE_CACHE.get(key)
    if not isinstance(scaled, pygame.Surface):
        scaled = pygame.transform.smoothscale(image, (width, height))
        _RAIL_SPRITE_CACHE[key] = scaled
    x = rect[0] + (rect[2] - width) // 2
    y = rect[1] + (rect[3] - height) // 2
    surface.blit(scaled, (x, y))


def _draw_back_row(
    surface: pygame.Surface,
    rect: tuple[int, int, int, int],
    hand_count: int,
) -> None:
    """对家手牌：橙色牌墙横放，只显示张数，不露牌面。"""
    shown = min(hand_count, T.OPP_BACK_MAX)
    if hand_count > T.OPP_BACK_MAX:
        log.warning("他家 hand_count=%d 超过 %d，超出部分不绘制", hand_count, T.OPP_BACK_MAX)
    _blit_flat_orange(surface, shown, rect)


def _winner_seat(state: GameState) -> int | None:
    result = state.result if isinstance(state.result, dict) else {}
    win = result.get("win") if isinstance(result.get("win"), dict) else {}
    seat = win.get("seat")
    return seat if isinstance(seat, int) else None


def _revealed_hands(state: GameState) -> dict[int, list[str]]:
    """game_end.results[].hand → 每家扁平手牌（含副露），仅结算期使用。"""
    result = state.result if isinstance(state.result, dict) else {}
    out: dict[int, list[str]] = {}
    for entry in result.get("results") or []:
        if not isinstance(entry, dict) or not isinstance(entry.get("seat"), int):
            continue
        hand = entry.get("hand")
        if isinstance(hand, list):
            out[entry["seat"]] = [t for t in hand if isinstance(t, str)]
    return out


def _concealed_of(state: GameState, seat: int, flat: list[str]) -> list[str]:
    """从扁平手牌里扣掉副露，只留门前牌；数据不匹配时原样返回。"""
    pool = list(flat)
    for meld in state.seat_view(seat).melds:
        tiles = list(meld.tiles)
        if tiles:
            multiset_remove(pool, tiles)
    return pool


def _rail_tight_blit_pos(sprite: pygame.Surface, cx: float, cy: float) -> tuple[int, int]:
    """按不透明区域中心对齐槽位（旋转后透明边距不再把框/牌整体推偏）。"""
    mask = pygame.mask.from_surface(sprite)
    boxes = mask.get_bounding_rects()
    if not boxes:
        rect = sprite.get_rect(center=(int(round(cx)), int(round(cy))))
        return rect.x, rect.y
    tight = boxes[0]
    for box in boxes[1:]:
        tight.union_ip(box)
    px = int(round(cx - (tight.x + tight.width / 2.0)))
    py = int(round(cy - (tight.y + tight.height / 2.0)))
    return px, py


def _blit_rail_at(
    surface: pygame.Surface, sprite: pygame.Surface, cx: float, cy: float,
) -> pygame.Rect:
    px, py = _rail_tight_blit_pos(sprite, cx, cy)
    surface.blit(sprite, (px, py))
    return pygame.Rect(px, py, sprite.get_width(), sprite.get_height())


def _draw_side_faces(
    surface: pygame.Surface,
    ui: UiView,
    rel: int,
    faces: list[str],
    *,
    highlight: bool,
) -> None:
    """摊牌：与扣牌同轨、同朝向。"""
    if not faces:
        return
    left = rel != 1
    points = T.side_rail_points(left, T.SIDE_HAND_INSET, len(faces))
    angle = T.side_face_angle(left)
    pitch = _rail_pitch(max(2, min(len(faces), T.SIDE_HAND_FULL)))
    width = max(18, int(round(pitch * T.SIDE_HAND_TILE_FILL * 1.06)))
    height = max(28, int(round(width * 1.45)))
    outline = (T.SELECT, 0.9, 2) if highlight else None
    for (x, y), face in zip(points, faces):
        sprite = _rail_face_sprite(
            ui.fonts, face, height, width, angle, outline=outline,
        )
        px, py = _rail_tight_blit_pos(sprite, x, y)
        dest = pygame.Rect(px, py, sprite.get_width(), sprite.get_height())
        _blit_outside_stage(surface, sprite, dest)


def _broadcast_stage() -> pygame.Rect:
    from .broadcast import R_STAGE

    return pygame.Rect(R_STAGE)


def _blit_outside_stage(surface: pygame.Surface, sprite: pygame.Surface, dest: pygame.Rect) -> None:
    """侧边摊牌不画进结算面板，避免斜牌透出来和分数行叠在一起。"""
    stage = _broadcast_stage()
    if not dest.colliderect(stage):
        surface.blit(sprite, dest)
        return
    bands: list[pygame.Rect] = []
    if dest.top < stage.top:
        bands.append(pygame.Rect(dest.left, dest.top, dest.width, stage.top - dest.top))
    if dest.bottom > stage.bottom:
        bands.append(pygame.Rect(dest.left, stage.bottom, dest.width, dest.bottom - stage.bottom))
    mid_top = max(dest.top, stage.top)
    mid_bottom = min(dest.bottom, stage.bottom)
    if mid_bottom > mid_top:
        if dest.left < stage.left:
            bands.append(pygame.Rect(dest.left, mid_top, stage.left - dest.left, mid_bottom - mid_top))
        if dest.right > stage.right:
            bands.append(pygame.Rect(stage.right, mid_top, dest.right - stage.right, mid_bottom - mid_top))
    for band in bands:
        visible = band.clip(dest)
        if visible.width <= 0 or visible.height <= 0:
            continue
        area = (visible.x - dest.x, visible.y - dest.y, visible.width, visible.height)
        surface.blit(sprite, visible.topleft, area)


def _rail_face_sprite(
    fonts,
    face: str,
    along: int,
    across: int,
    angle: float,
    *,
    outline: tuple[tuple[int, int, int], float, int] | None = None,
) -> pygame.Surface:
    """侧边牌面：4× 超采样后旋转；描边在旋转前画在牌面上，与牌一起转。"""
    cache_key = None
    if outline is None:
        cache_key = ("face", face, along, across, round(angle, 2))
        cached = _RAIL_SPRITE_CACHE.get(cache_key)
        if cached is not None:
            return cached
    scale = 4
    bw, bh = across * scale, along * scale
    base = W.tile_face_surface(
        fonts, face, bw, bh, 3 * scale,
        T.FS_TILE_SIDE_RANK * scale, T.FS_TILE_SIDE_SUIT * scale,
        T.FS_TILE_Z_RIVER_SIDE * scale,
    )
    if outline is not None:
        color, alpha, line_w = outline
        W.draw_border_alpha(
            base,
            (0, 0, bw, bh),
            color,
            alpha,
            width=max(1, line_w * scale),
            radius=3 * scale,
        )
    rotated = pygame.transform.rotate(base, angle)
    sharp = pygame.transform.smoothscale(
        rotated,
        (
            max(1, int(round(rotated.get_width() / scale))),
            max(1, int(round(rotated.get_height() / scale))),
        ),
    )
    if cache_key is not None:
        if len(_RAIL_SPRITE_CACHE) > 80:
            _RAIL_SPRITE_CACHE.clear()
        _RAIL_SPRITE_CACHE[cache_key] = sharp
    return sharp


def draw_revealed_hands(surface: pygame.Surface, ui: UiView) -> None:
    """结算播报：把四家手牌在各自桌边摊开（本家手牌本来已正面朝上）。

    只覆盖 game_end 有 `hand` 的座位；缺数据时保留牌背，不伪造牌面。
    """
    state = ui.state
    hands = _revealed_hands(state)
    if not hands:
        return
    result = state.result if isinstance(state.result, dict) else {}
    win = result.get("win") if isinstance(result.get("win"), dict) else {}
    winner = win.get("seat")
    fonts = ui.fonts
    for seat, flat in hands.items():
        if seat == state.my_seat:
            continue
        rel = state.rel_index(seat)
        concealed = _concealed_of(state, seat, flat)
        if not concealed:
            continue
        if rel == 2:
            rect = T.R_OPP_BACK
            tw, th = T.OPP_BACK_TILE
            for i, face in enumerate(concealed):
                row, col = divmod(i, T.OPP_BACK_ROW_CAPACITY)
                tile_rect = project((
                    rect[0] + col * T.OPP_BACK_PITCH, rect[1] + row * rect[3], tw, th,
                ))
                W.draw_tile(surface, fonts, tile_rect, face,
                            rank_size=T.FS_TILE_SIDE_RANK, suit_size=T.FS_TILE_SIDE_SUIT,
                            honor_size=T.FS_TILE_Z_RIVER_SIDE)
                if winner == seat:
                    W.draw_border_alpha(surface, tile_rect, T.SELECT, 0.9, width=2)
        else:
            _draw_side_faces(surface, ui, rel, concealed, highlight=winner == seat)


def _draw_seat_tiles(surface: pygame.Surface, ui: UiView, seat: int, feel=None) -> None:
    """一家摆在桌面上的牌：手牌背 / 副露 / 牌河。"""
    state = ui.state
    rel = state.rel_index(seat)
    view = state.seat_view(seat)
    if rel == 0:
        return  # 本家由 `_draw_self_hand` 画
    # 播报/结算期：要摊牌的座位不再画牌背，避免“牌背 + 牌面”叠在一起。
    reveal_now = (
        state.match_end is None
        and broadcast_view.is_broadcasting(ui)
        and seat in _revealed_hands(state)
    )
    if rel == 2:
        if not reveal_now:
            _draw_back_row(surface, T.R_OPP_BACK, view.hand_count)
        _draw_wall_row(surface, T.R_OPP_WALL, wall_stacks(ui))
        _draw_melds(surface, ui, seat, T.R_OPP_MELD, vertical=False)
        _draw_river(
            surface, ui, seat, T.R_OPP_RIVER, center=True, feel=feel, project_tiles=False,
        )
    elif rel == 1:
        if not reveal_now:
            _draw_side_hand(surface, rel, view.hand_count)
        _draw_side_wall(surface, rel, wall_stacks(ui))
        _draw_melds(
            surface, ui, seat, T.R_SIDE_R_MELD, vertical=False, project_tiles=False,
            highlight=_winner_seat(state) == seat and reveal_now,
        )
        _draw_river(surface, ui, seat, T.R_SIDE_R_RIVER, center=False, feel=feel)
    else:
        if not reveal_now:
            _draw_side_hand(surface, rel, view.hand_count)
        _draw_side_wall(surface, rel, wall_stacks(ui))
        _draw_melds(
            surface, ui, seat, T.R_SIDE_L_MELD, vertical=False, project_tiles=False,
            highlight=_winner_seat(state) == seat and reveal_now,
        )
        _draw_river(surface, ui, seat, T.R_SIDE_L_RIVER, center=False, feel=feel)


def _draw_side_melds(
    surface: pygame.Surface,
    ui: UiView,
    seat: int,
    *,
    left: bool,
    highlight: bool = False,
) -> None:
    """侧家副露：每组横排沿桌边，整组在牌山内侧，不叠牌、不压牌山。"""
    melds = ui.state.seat_view(seat).melds
    if not melds:
        return
    angle = T.side_face_angle(left)
    along, across = _side_river_tile_size()
    group_count = len(melds)
    for group_index, meld in enumerate(melds):
        faces: list[str | None] = list(meld.tiles) or [None] * 4
        centers = T.side_meld_group_centers(left, group_index, len(faces), group_count)
        for face, (x, y) in zip(faces, centers):
            outline = (T.SELECT, 0.9, 2) if highlight and face is not None else None
            if face is None:
                sprite = _rail_back_sprite(along, across, angle, plain=True)
            else:
                sprite = _rail_face_sprite(
                    ui.fonts, face, along, across, angle, outline=outline,
                )
            if sprite is None:
                continue
            dest = sprite.get_rect(center=(int(round(x)), int(round(y))))
            if highlight:
                _blit_outside_stage(surface, sprite, dest)
            else:
                surface.blit(sprite, dest)


def _draw_side_river_rail(
    surface: pygame.Surface,
    ui: UiView,
    seat: int,
    rect: tuple[int, int, int, int],
    feel,
) -> None:
    """侧家牌河：槽位在桌沿轨上，牌顶朝桌心。"""
    left = rect[0] + rect[2] // 2 < T.LOGICAL_W // 2
    discards = ui.state.seat_view(seat).discards
    if not discards:
        return
    centers = side_river_centers(left, len(discards))
    angle = T.side_face_angle(left)
    along, across = _side_river_tile_size()
    skip_last = bool(feel is not None and seat in feel.flying_seats())
    last_index = len(discards) - 1
    mark_claimed = (
        seat == ui.state.last_discard_seat
        and claim_highlight_active(ui.state)
        and getattr(feel, "claim_hints_on", True)
    )
    for index, (center, face) in enumerate(zip(centers, discards)):
        if skip_last and index == last_index:
            continue
        outline = None
        if mark_claimed and index == last_index:
            if ui.state.pending:
                outline = (T.PANEL_EDGE, 1.0, 1)
            else:
                outline = (T.ACTIVE, _active_alpha(ui.now), 2)
        sprite = _rail_face_sprite(
            ui.fonts, face, along, across, angle, outline=outline,
        )
        dest = sprite.get_rect(
            center=(int(round(center[0])), int(round(center[1]))),
        )
        surface.blit(sprite, dest)


def _draw_river(
    surface: pygame.Surface,
    ui: UiView,
    seat: int,
    rect: tuple[int, int, int, int],
    *,
    center: bool,
    feel=None,
    rotate: int = 0,
    project_tiles: bool = True,
) -> None:
    discards = ui.state.seat_view(seat).discards
    if not center:
        _draw_side_river_rail(surface, ui, seat, rect, feel)
        return
    rects = center_river_rects(rect, len(discards))
    rank, suit = T.FS_TILE_RIVER_RANK, T.FS_TILE_RIVER_SUIT
    honor = T.FS_TILE_Z_RIVER_CENTER
    # F-AN-16：飞行期间该格**不画**（否则同一张牌会同时出现在牌河与飞行体里）
    skip_last = bool(feel is not None and seat in feel.flying_seats())
    last_index = len(discards) - 1
    # F-CL-01：被鸣牌（最近一次打出的那张）呼吸描边
    mark_claimed = (
        seat == ui.state.last_discard_seat
        and claim_highlight_active(ui.state)
        and getattr(feel, "claim_hints_on", True)
    )
    for index, (tile_rect, face) in enumerate(zip(rects, discards)):
        if skip_last and index == last_index:
            continue
        if project_tiles:
            tile_rect = project(tile_rect)
        if rotate:
            W.draw_tile_rotated(
                surface, ui.fonts, tile_rect, face, angle=rotate,
                rank_size=rank, suit_size=suit, honor_size=honor,
            )
        else:
            W.draw_tile(
                surface, ui.fonts, tile_rect, face, rank_size=rank, suit_size=suit, honor_size=honor
            )
        if mark_claimed and index == last_index:
            alpha = _active_alpha(ui.now)
            if ui.state.pending:
                # F-CL-07：已提交 ⇒ 降级为 PANEL_EDGE 1 px（表示「已处理」），不闪烁
                W.draw_border_alpha(surface, tile_rect, T.PANEL_EDGE, 1.0, width=1)
            else:
                W.draw_border_alpha(surface, tile_rect, T.ACTIVE, alpha, width=2)


def _draw_melds(
    surface: pygame.Surface,
    ui: UiView,
    seat: int,
    area: tuple[int, int, int, int],
    *,
    vertical: bool,
    groups=None,
    project_tiles: bool = True,
    highlight: bool = False,
) -> None:
    state = ui.state
    if groups is None:
        melds = state.melds if seat == state.my_seat else state.seat_view(seat).melds
        if not melds:
            return
        groups = meld_layout(area, melds, vertical=vertical)
    if not groups:
        return

    def place(rect: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
        if project_tiles:
            return project(rect)
        return rect

    for group_rect, tiles in groups:
        group_rect = pygame.Rect(place((group_rect.x, group_rect.y, group_rect.w, group_rect.h)))
        _draw_meld_frame(surface, group_rect)
        # v2.0（§4.8）：face=None ⇒ 暗杠空数组形态，画**4 张牌背**
        for tile_rect, face in tiles:
            tile_rect = place(tile_rect)
            if face is None:
                W.draw_tile_back(surface, tile_rect)
                continue
            W.draw_tile(
                surface, ui.fonts, tile_rect, face,
                rank_size=T.FS_TILE_MELD_RANK, suit_size=T.FS_TILE_MELD_SUIT,
                honor_size=T.FS_TILE_Z_MELD,
            )
            if highlight:
                W.draw_border_alpha(surface, tile_rect, T.SELECT, 0.9, width=2)


def _draw_meld_frame(surface: pygame.Surface, rect: pygame.Rect) -> None:
    """副露组底板：一层很淡的暗色，把一组牌框在一起（不抢牌面）。"""
    layer = pygame.Surface((rect.w, rect.h), pygame.SRCALPHA)
    pygame.draw.rect(layer, (0, 0, 0, 62), (0, 0, rect.w, rect.h), border_radius=4)
    pygame.draw.rect(layer, (255, 255, 255, 20), (0, 0, rect.w, rect.h), width=1, border_radius=4)
    surface.blit(layer, (rect.x, rect.y))


#: 一局初始牌山张数（136 口径：136 − 庄家 14 − 三家 13 = 83）
WALL_TOTAL = 83
#: 牌山的画法：每叠 2 张，从上往下看只看到最上面一张，所以「一叠 = 一张牌背」
WALL_COLUMNS = 17
WALL_ROWS = 2
WALL_PITCH = 18
_WALL_SHADOW: pygame.Surface | None = None


def _wall_shadow() -> pygame.Surface:
    global _WALL_SHADOW
    if _WALL_SHADOW is None:
        size = (WALL_COLUMNS * WALL_PITCH + 6, WALL_ROWS * WALL_PITCH + 6)
        shadow = pygame.Surface(size, pygame.SRCALPHA)
        pygame.draw.rect(shadow, (0, 0, 0, 46), (0, 0, *size), border_radius=6)
        _WALL_SHADOW = shadow
    return _WALL_SHADOW


def _draw_wall(surface: pygame.Surface, ui: UiView) -> None:
    """桌面上的牌山（v2.6）：牌背码成方阵，随剩余张数变短——不再显示数字。

    位置在两条中央牌河之间的桌心横带，正好是真实牌桌上牌山所在的位置；
    每局开牌时最满，越打越短，最后只剩几下。
    """
    remaining = ui.state.remaining
    if remaining is None:
        return
    capacity = WALL_COLUMNS * WALL_ROWS
    stacks = int(round(max(0, min(WALL_TOTAL, remaining)) / WALL_TOTAL * capacity))
    if stacks <= 0:
        return
    origin_x = T.R_CENTER[0] + (T.R_CENTER[2] - WALL_COLUMNS * WALL_PITCH) // 2
    origin_y = T.R_CENTER[1] + 2
    surface.blit(_wall_shadow(), (origin_x - 3, origin_y - 1))
    for index in range(stacks):
        row, col = divmod(index, WALL_COLUMNS)
        # 第二排错半格码放，像真的把牌叠起来（砖缝错位），而不是一块色条
        stagger = WALL_PITCH // 2 if row % 2 else 0
        W.draw_tile_back(
            surface,
            (origin_x + col * WALL_PITCH + stagger, origin_y + row * WALL_PITCH,
             T.OPP_BACK_TILE[0], T.OPP_BACK_TILE[1]),
            radius=3,
        )


def wall_stacks(ui: UiView) -> int:
    """每家面前还剩几叠牌山（按剩余张数四等分）。"""
    remaining = ui.state.remaining
    if remaining is None:
        return T.WALL_PER_SIDE
    return max(0, min(T.WALL_PER_SIDE, int(round(remaining / 4))))


def _wall_stack(surface: pygame.Surface, x: int, y: int, w: int, h: int) -> None:
    """一叠牌山：CC0 牌背 + 中间一道叠牌缝，和单张手牌背区分开。"""
    W.draw_tile_back(surface, (x, y, w, h))
    seam_y = y + int(h * 0.56)
    pygame.draw.line(surface, art.shade(T.BACK, 0.5), (x + 1, seam_y), (x + w - 2, seam_y))
    pygame.draw.line(surface, art.shade(T.BACK, 1.25), (x + 1, seam_y + 1), (x + w - 2, seam_y + 1))


def _draw_side_wall(surface: pygame.Surface, rel: int, stacks: int) -> None:
    """侧家牌山：和手牌同一套橙色牌墙，隔开一截贴在内侧。近端固定。"""
    if stacks <= 0:
        return
    left = rel != 1
    shown = min(stacks, 18)
    strip = W.side_strip_surface(shown)
    if strip is not None:
        _blit_side_strip(
            surface, strip, left, shown, T.SIDE_WALL_INSET, T.WALL_PER_SIDE, from_far=True,
        )
        return
    points = T.side_rail_points(
        left, T.SIDE_WALL_INSET, stacks, T.WALL_PER_SIDE, from_far=True,
    )
    angle = T.side_face_angle(left)
    along, across = 28, 20
    sprite = _rail_back_sprite(along, across, angle, plain=True)
    for x, y in points:
        if sprite is None:
            W.draw_tile_back(surface, (int(round(x - across / 2)), int(round(y - along / 2)), across, along))
            continue
        rect = sprite.get_rect(center=(int(round(x)), int(round(y))))
        surface.blit(sprite, rect.topleft)


def _draw_wall_row(
    surface: pygame.Surface,
    rect: tuple[int, int, int, int],
    stacks: int,
    *,
    vertical: bool = False,
) -> None:
    """对家 / 本家牌山：橙色牌墙横放。张数少时从中间变短。"""
    if stacks <= 0 or vertical:
        return
    shown = min(stacks, 18)
    _blit_flat_orange(surface, shown, rect)


def _draw_near_wall(surface: pygame.Surface, ui: UiView) -> None:
    """本家面前的牌山段（就在自己手牌正前方）。"""
    _draw_wall_row(surface, T.R_SELF_WALL, wall_stacks(ui))


def _draw_center(surface: pygame.Surface, ui: UiView) -> None:
    """桌心（v3.0）：桌心那条横带留给听牌条，临时提示压到桌沿正下方一行。"""
    for i, note in enumerate(ui.extra_notes[:1]):
        W.draw_text(
            surface, note, ui.fonts.get(T.FS_MICRO), T.WARN,
            (440, T.TABLE_FELT[1] + 4 + i * 15, 400, 14),
            W.ALIGN_CENTER,
        )


def _draw_hand_tray(surface: pygame.Surface, ui: UiView) -> None:
    """手牌托盘：跟着「副露 + 手牌」这一行的实际宽度走。"""
    state = ui.state
    rects = hand_layout(state)
    melds = meld_row_layout(state)
    if not rects and not melds:
        return
    left = min([r[0] for r in rects] + [g[0].x for g in melds]) - 16
    right = max([r[0] + r[2] for r in rects] + [g[0].right for g in melds]) + 16
    top = T.HAND_Y - 14
    surface.blit(
        art.strip(right - left, 100, (4, 14, 10, 72), edge=(255, 255, 255, 10), radius=18),
        (left, top),
    )
    glow = pygame.Surface((right - left - 24, 1), pygame.SRCALPHA)
    glow.fill((232, 192, 104, 34))
    surface.blit(glow, (left + 12, top + 1))


def _draw_self_plate(surface: pygame.Surface, ui: UiView) -> None:
    """本家那块分数牌 + 系统菜单（参考视频放在手牌右侧的桌角）。

    本家没有立绘，这块牌子就是「我是谁、我现在几张」的唯一落点。
    """
    state, fonts = ui.state, ui.fonts
    rect = T.R_SELF_PLATE
    name = state.name_of(state.my_seat) if state.my_seat is not None else "你"
    name_h = T.PLATE_NAME_H

    surface.blit(
        art.strip(rect[2], name_h, T.PLATE_NAME, edge=(146, 112, 100), radius=4),
        (rect[0], rect[1]),
    )
    badge = pygame.Rect(rect[0] + 4, rect[1] + 3, 22, name_h - 6)
    pygame.draw.rect(surface, T.ACTIVE, badge, border_radius=3)
    wind = _wind_of(state.my_seat, state) if state.my_seat is not None else "東"
    W.draw_text(
        surface, wind, fonts.get(T.FS_MICRO, bold=True), (28, 20, 24),
        (badge.x, badge.y, badge.w, badge.h), W.ALIGN_CENTER,
    )
    name_font = fonts.get(T.FS_SMALL, bold=True)
    W.draw_text(
        surface, _ellipsize(f"{name}（你）", name_font, rect[2] - 44), name_font, T.TEXT,
        (rect[0] + 30, rect[1], rect[2] - 34, name_h), W.ALIGN_LEFT,
    )

    score = (rect[0], rect[1] + name_h, rect[2], rect[3] - name_h)
    surface.blit(
        art.strip(score[2], score[3], T.PLATE_SCORE, edge=art.shade(T.PLATE_SCORE, 1.45), radius=4),
        (score[0], score[1]),
    )
    face_font = fonts.get(T.FS_MICRO, bold=True)
    W.draw_text(
        surface, f"{len(state.hand)} 张", face_font, (255, 232, 176),
        (score[0] + 8, score[1], score[2] - 60, score[3]), W.ALIGN_LEFT,
    )
    if state.my_seat is not None and state.dealer == state.my_seat:
        tag = pygame.Rect(score[0] + score[2] - 30, score[1] + 2, 26, score[3] - 4)
        pygame.draw.rect(surface, T.DEALER, tag, border_radius=3)
        W.draw_text(surface, "庄", fonts.get(T.FS_MICRO, bold=True), (48, 30, 8),
                    (tag.x, tag.y, tag.w, tag.h), W.ALIGN_CENTER)

    # 系统菜单（账号会话才有内容可看：打开战绩面板）
    if ui.has_account_session:
        btn = T.R_MENU_BTN
        hovered = T.rect_contains(btn, ui.mouse)
        W.draw_button(
            surface, fonts, btn, "系统菜单", enabled=True,
            hovered=hovered, pressed=hovered and ui.mouse_down, radius=8,
        )


def _draw_self_hand(surface: pygame.Surface, ui: UiView, feel=None) -> None:
    state, fonts = ui.state, ui.fonts
    # 本家牌河（v2.7 修正：之前 rel==0 提前 return，把自己牌河漏画了）
    if state.my_seat is not None:
        _draw_river(
            surface, ui, state.my_seat, T.R_SELF_RIVER, center=True, feel=feel, project_tiles=False,
        )
    # 副露（与手牌同一行，左侧）
    _draw_melds(
        surface, ui, state.my_seat if state.my_seat is not None else 0,
        T.R_SELF_MELD, vertical=False, groups=meld_row_layout(state),
    )

    rects = hand_layout(state)
    # F-CL-02：可吃碰关联高亮（只读描边，不改可点性）
    faces = claim_hand_faces(state, state.window) if (
        claim_highlight_active(state) and getattr(feel, "claim_hints_on", True)
    ) else {}
    consumed: dict[str, int] = {}
    if not rects and state.game_started:
        W.draw_text(
            surface, "手牌为空", fonts.get(T.FS_BODY), T.NEG, T.R_SELF_HAND, W.ALIGN_CENTER
        )
        return
    if ui.pending_flash:
        pygame.draw.rect(surface, T.NEG, T.R_SELF_HAND, width=2, border_radius=14)
    for i, (tile_rect, face) in enumerate(zip(rects, state.hand)):
        # F-AN-22：摸牌进场位移（dy 16 → 0），只动绘制、不动命中矩形（§7 R3）
        dy, draw_alpha = feel.draw_offset(i, ui.now) if feel is not None else (0.0, 255.0)
        shifted = (tile_rect[0], int(tile_rect[1] + dy), tile_rect[2], tile_rect[3])
        hovered = ui.hand_hover_index == i and (
            state.window is not None
            and not state.pending
            and state.selected_index != i
        )
        W.draw_tile(
            surface,
            fonts,
            shifted,
            face,
            radius=6,
            rank_size=T.FS_TILE_HAND_RANK,
            suit_size=T.FS_TILE_HAND_SUIT,
            honor_size=T.FS_TILE_Z_HAND,
            selected=state.selected_index == i,
            drawn=state.drawn_index == i and draw_alpha >= 255.0,
            pending=bool(state.pending and state.pending_index == i),
        )
        # F-AN-23：进场期间 DRAWN 描边 alpha 0→255（终态后交给 `draw_tile` 自己画）
        if state.drawn_index == i and draw_alpha < 255.0:
            W.draw_border_alpha(surface, shifted, T.DRAWN, draw_alpha / 255.0, width=2)
        # F-CL-02 / F-CL-07 / F-CL-09
        if faces and state.selected_index != i:
            want = faces.get(face, 0)
            if want > 0 and consumed.get(face, 0) < want:
                consumed[face] = consumed.get(face, 0) + 1
                if state.pending:
                    W.draw_border_alpha(surface, shifted, T.PANEL_EDGE, 1.0, width=1)
                else:
                    W.draw_border_alpha(surface, shifted, T.WARN, 0.95, width=2)
        if hovered:
            lift = T.HAND_DRAWN_LIFT if state.drawn_index == i else 0
            pygame.draw.rect(
                surface,
                T.BTN_HOVER,
                (tile_rect[0], tile_rect[1] - lift, tile_rect[2], tile_rect[3]),
                width=1,
                border_radius=6,
            )
    if state.selected_index is not None and 0 <= state.selected_index < len(state.hand):
        # §5.4 ② 的状态提示
        face = state.hand[state.selected_index]
        if ui.toast_alpha <= 0:
            _draw_toast_pill(surface, ui, selection_hint(face), T.SELECT)
    _draw_self_plate(surface, ui)


def _draw_toast_pill(
    surface: pygame.Surface, ui: UiView, text: str, color: tuple, alpha: int = 255
) -> None:
    """浮动胶囊提示：按文字宽度自适应、居中于 R_TOAST。"""
    font = ui.fonts.get(T.FS_SMALL)
    width = min(T.LOGICAL_W - 80, font.size(text)[0] + 32)
    height = 26
    x = 640 - width // 2
    y = T.R_TOAST[1] + T.R_TOAST[3] // 2 + 46 - height // 2   # 让开中央情报带（348..384）
    art.drop_shadow(surface, (x, y, width, height), radius=height // 2, offset=(0, 2), alpha=90, spread=2)
    surface.blit(
        art.strip(width, height, (8, 22, 16, min(230, alpha)), edge=(232, 192, 104, 70), radius=height // 2),
        (x, y),
    )
    W.draw_text(surface, text, font, color, (x, y, width, height), W.ALIGN_CENTER, alpha=alpha)


def _draw_toast(surface: pygame.Surface, ui: UiView) -> None:
    if not ui.toast_text or ui.toast_alpha <= 0:
        return
    _draw_toast_pill(surface, ui, ui.toast_text, T.TEXT, ui.toast_alpha)
