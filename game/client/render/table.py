"""牌桌界面（UI_UX_SPEC §4）。

坐标全部取自 `theme.py`（= §4.3 坐标总表）。本模块是纯读取渲染：
- 他家手牌**只**通过 `draw_tile_back` 以 `hand_count` 张牌背呈现（§4.6 / §7.2）；
- 他家的 `SeatView` 结构里没有任何牌面字段可读。
"""

from __future__ import annotations

import logging
import math

import pygame

from .. import theme as T
from ..game_state import SCREEN_TABLE, GameState
from ..tiles import tile_label
from ..view import UiView
from . import widgets as W

log = logging.getLogger("mahjong.client.table")

BREATH_PERIOD_MS = 1200.0     # §4.9 呼吸周期 1.2 s
FLASH_PERIOD_MS = 400.0       # §6.1 危险色闪烁 400 ms 相位


# ---------------------------------------------------------------- 布局计算


def selection_hint(face: str) -> str:
    """「已选中」提示文案（UI_UX_SPEC v1.4 §5.4 ②，逐字冻结）。

    牌名走 §5.3 文案公式（`tiles.tile_label`），**不再显示线上牌码**；
    调用点不得自行拼接牌名。
    """
    return f"已选中 {tile_label(face)}，再点一次或按 Enter 打出"


def hand_layout(state: GameState) -> list[tuple[int, int, int, int]]:
    """§4.5：`total_w = n*58 - 2`；`x0 = 640 - total_w//2`；`y0 = 566`。"""
    n = len(state.hand)
    if n <= 0:
        return []
    total_w = n * T.HAND_PITCH - 2
    x0 = 640 - total_w // 2
    return [(x0 + i * T.HAND_PITCH, T.HAND_Y, T.HAND_TILE_W, T.HAND_TILE_H) for i in range(n)]


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


def side_river_rects(rect: tuple[int, int, int, int], count: int) -> list[tuple[int, int, int, int]]:
    """左右列牌河：3 列 × 9 行（§4.7）。"""
    tw, th = T.RIVER_SIDE_TILE
    out = []
    for i in range(count):
        row, col = divmod(i, T.RIVER_SIDE_COLS)
        if row >= T.RIVER_SIDE_ROWS:
            break
        out.append(
            (
                rect[0] + col * T.RIVER_SIDE_COL_PITCH,
                rect[1] + row * T.RIVER_SIDE_ROW_PITCH,
                tw,
                th,
            )
        )
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


def render(surface: pygame.Surface, ui: UiView, feel=None) -> None:
    state = ui.state
    surface.fill(T.BG)
    _draw_hud(surface, ui)
    for seat in range(4):
        _draw_seat(surface, ui, seat, feel)
    _draw_center(surface, ui)
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
    surface: pygame.Surface, ui: UiView, rect: tuple[float, float, float, float], face: str
) -> None:
    """F-AN-14/15：飞行体牌面（尺寸随进度插值、旋转 0 度），画在牌桌之上。"""
    W.draw_tile(
        surface,
        ui.fonts,
        (int(rect[0]), int(rect[1]), max(1, int(rect[2])), max(1, int(rect[3]))),
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


def _draw_hud(surface: pygame.Surface, ui: UiView) -> None:
    state, fonts = ui.state, ui.fonts
    pygame.draw.rect(surface, T.PANEL, T.R_HUD)
    pygame.draw.line(surface, T.PANEL_EDGE, (0, T.R_HUD[3]), (T.LOGICAL_W, T.R_HUD[3]))

    room = state.room_id or "—"
    me = state.name_of(state.my_seat) if state.my_seat is not None else "—"
    W.draw_text(
        surface, f"{room}  {me}（本家）", fonts.get(T.FS_SMALL), T.TEXT, T.R_HUD_LEFT, W.ALIGN_LEFT
    )
    # v2.0（§4.11）：局号位「X/4」（X = round_index + 1，来自 game_start；局间不显示累计分）
    if state.game_started:
        W.draw_text(
            surface,
            f"{state.round_index + 1}/{state.round_count}",
            fonts.get(T.FS_BODY, bold=True),
            T.TEXT,
            T.R_HUD_ROUND,
            W.ALIGN_CENTER,
        )
    # v2.0：连庄提示并入中段阶段文案（账号会话局号右侧空档是战绩按钮，不能占用）
    mid = state.phase if state.renchan_count <= 0 else f"{state.phase} · 连庄 {state.renchan_count}"
    W.draw_text(
        surface, mid, fonts.get(T.FS_BODY), T.TEXT_DIM, T.R_HUD_MID, W.ALIGN_CENTER
    )

    dot_color = T.POS if ui.connected else T.NEG
    dot_y = T.R_HUD[1] + T.R_HUD[3] // 2
    pygame.draw.circle(surface, dot_color, (T.R_HUD_RIGHT[0] + 8, dot_y), 5)
    label = "已连接" if ui.connected else "未连接"
    if ui.rtt_ms is not None and ui.rtt_ms > 1000:
        label += "（延迟高）"
    x = T.R_HUD_RIGHT[0] + 20
    W.draw_text(surface, label, fonts.get(T.FS_SMALL), T.TEXT_DIM, (x, T.R_HUD[1], 120, T.R_HUD[3]))
    if ui.illegal_count > 0:
        color = T.NEG if ui.illegal_count >= 7 else T.WARN
        W.draw_text(
            surface,
            f"非法操作 {ui.illegal_count}/10",
            fonts.get(T.FS_SMALL, bold=ui.illegal_count >= 7),
            color,
            (x + 120, T.R_HUD[1], 180, T.R_HUD[3]),
        )
    if state.hand_warning:
        W.draw_text(
            surface, "张数异常", fonts.get(T.FS_SMALL), T.WARN,
            (T.R_HUD_RIGHT[0] - 60, T.R_HUD[1], 60, T.R_HUD[3]), W.ALIGN_RIGHT,
        )

    # 战绩入口（UI_UX_SPEC §4.11）：只有账号会话才有历史可看，游客会话不画这个按钮。
    if ui.has_account_session:
        rect = T.R_HUD_STATS_BTN
        hovered = T.rect_contains(rect, ui.mouse)
        W.draw_button(
            surface,
            fonts,
            rect,
            "战绩",
            enabled=True,
            hovered=hovered,
            pressed=hovered and ui.mouse_down,
        )


def _active_alpha(now: float) -> float:
    phase = (now * 1000.0 % BREATH_PERIOD_MS) / BREATH_PERIOD_MS
    return 0.6 + 0.4 * (0.5 + 0.5 * math.sin(2 * math.pi * phase))


def _draw_name_strip(
    surface: pygame.Surface,
    ui: UiView,
    rect: tuple[int, int, int, int],
    seat: int,
    *,
    align: str = W.ALIGN_LEFT,
    active: bool = False,
    extra: str = "",
) -> None:
    state, fonts = ui.state, ui.fonts
    view = state.seat_view(seat)
    label = f"{view.name or f'座位{seat}'} · {state.rel_label(seat)}"
    if extra:
        label += f"  {extra}"
    if view.disconnected:
        label += "（断线托管）"
    color = T.TEXT if not view.disconnected else T.WARN
    W.draw_text(surface, label, fonts.get(T.FS_BODY), color, rect, align)
    if active:
        W.draw_border_alpha(surface, rect, T.ACTIVE, _active_alpha(ui.now), width=2)
    elif view.disconnected:
        W.draw_border_alpha(surface, rect, T.WARN, 0.85, width=2)


def _draw_dealer_badge(surface: pygame.Surface, ui: UiView, rect: tuple[int, int, int, int]) -> None:
    if ui.state.dealer is None:
        return
    pygame.draw.rect(surface, T.DEALER, rect, border_radius=3)
    W.draw_text(surface, "庄", ui.fonts.get(T.FS_SMALL, bold=True), (24, 20, 12), rect, W.ALIGN_CENTER)


def _draw_back_row(
    surface: pygame.Surface, rect: tuple[int, int, int, int], hand_count: int
) -> None:
    """他家手牌：**只**画牌背（§4.6）。v2.0：容量 14 → 18（一般式 14 + kans，kans ≤ 4）。

    15..18 张**换行**绘制第二行（行距 16 px，向下扩展），不裁剪、不缩小；
    超过 18（协议上限）记 WARN 不绘制超出部分。
    """
    max_tiles = max(0, rect[2] // T.OPP_BACK_PITCH)
    per_row = min(T.OPP_BACK_ROW_CAPACITY, max_tiles)
    shown = min(hand_count, T.OPP_BACK_MAX, per_row + 4)  # 第二行最多再放 4 张（14 + 4 = 18）
    if hand_count > T.OPP_BACK_MAX:
        log.warning("他家 hand_count=%d 超过 %d，超出部分不绘制", hand_count, T.OPP_BACK_MAX)
    for i in range(shown):
        row, col = divmod(i, per_row)
        W.draw_tile_back(
            surface,
            (
                rect[0] + col * T.OPP_BACK_PITCH,
                rect[1] + row * rect[3],  # 行距 16 px（背条高度），向下扩展
                T.OPP_BACK_TILE[0],
                T.OPP_BACK_TILE[1],
            ),
        )


def _draw_seat(surface: pygame.Surface, ui: UiView, seat: int, feel=None) -> None:
    state, fonts = ui.state, ui.fonts
    rel = state.rel_index(seat)
    view = state.seat_view(seat)
    active = state.current_seat == seat and state.screen == SCREEN_TABLE
    if rel == 0:
        # 本家：昵称在 HUD 左区；此处只补「当前行动」高亮（§4.9）与本家副露/牌河
        if active:
            W.draw_border_alpha(surface, T.R_HUD_LEFT, T.ACTIVE, _active_alpha(ui.now), width=2)
        _draw_melds(surface, ui, seat, T.R_SELF_MELD, vertical=False)
        _draw_river(surface, ui, seat, T.R_SELF_RIVER, center=True, feel=feel)
    elif rel == 2:
        _draw_name_strip(
            surface, ui, T.R_OPP_NAME, seat, align=W.ALIGN_CENTER, active=active,
            extra=f"{view.hand_count} 张",
        )
        if state.dealer == seat:
            _draw_dealer_badge(surface, ui, (T.R_OPP_NAME[0] - 22, T.R_OPP_NAME[1], 20, 20))
        _draw_back_row(surface, T.R_OPP_BACK, view.hand_count)
        _draw_melds(surface, ui, seat, T.R_OPP_MELD, vertical=False)
        _draw_river(surface, ui, seat, T.R_OPP_RIVER, center=True, feel=feel)
    elif rel == 1:
        strip = T.R_SIDE_R_NAME
        _draw_name_strip(
            surface, ui, strip, seat, align=W.ALIGN_LEFT, active=active,
            extra=f"{view.hand_count} 张",
        )
        if state.dealer == seat:
            _draw_dealer_badge(surface, ui, (strip[0] + strip[2] - 20, strip[1], 20, 20))
        _draw_back_row(surface, T.R_SIDE_R_BACK, view.hand_count)
        _draw_melds(surface, ui, seat, T.R_SIDE_R_MELD, vertical=True)
        _draw_river(surface, ui, seat, T.R_SIDE_R_RIVER, center=False, feel=feel)
    else:
        strip = T.R_SIDE_L_NAME
        _draw_name_strip(
            surface, ui, strip, seat, align=W.ALIGN_LEFT, active=active,
            extra=f"{view.hand_count} 张",
        )
        if state.dealer == seat:
            _draw_dealer_badge(surface, ui, (strip[0] + strip[2] - 20, strip[1], 20, 20))
        _draw_back_row(surface, T.R_SIDE_L_BACK, view.hand_count)
        _draw_melds(surface, ui, seat, T.R_SIDE_L_MELD, vertical=True)
        _draw_river(surface, ui, seat, T.R_SIDE_L_RIVER, center=False, feel=feel)


def _draw_river(
    surface: pygame.Surface,
    ui: UiView,
    seat: int,
    rect: tuple[int, int, int, int],
    *,
    center: bool,
    feel=None,
) -> None:
    discards = ui.state.seat_view(seat).discards
    if center:
        rects = center_river_rects(rect, len(discards))
        rank, suit = T.FS_TILE_RIVER_RANK, T.FS_TILE_RIVER_SUIT
        honor = T.FS_TILE_Z_RIVER_CENTER
    else:
        rects = side_river_rects(rect, len(discards))
        rank, suit = T.FS_TILE_SIDE_RANK, T.FS_TILE_SIDE_SUIT
        honor = T.FS_TILE_Z_RIVER_SIDE
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
) -> None:
    state = ui.state
    if seat == state.my_seat:
        melds = state.melds
    else:
        melds = state.seat_view(seat).melds
    if not melds:
        return
    for group_rect, tiles in meld_layout(area, melds, vertical=vertical):
        W.draw_panel(surface, (group_rect.x, group_rect.y, group_rect.w, group_rect.h), radius=3)
        # v2.0（§4.8）：face=None ⇒ 暗杠空数组形态，画**4 张牌背**（他家暗杠揭示前的恒常形态）
        for tile_rect, face in tiles:
            if face is None:
                W.draw_tile_back(surface, tile_rect)
                continue
            W.draw_tile(
                surface, ui.fonts, tile_rect, face,
                rank_size=T.FS_TILE_MELD_RANK, suit_size=T.FS_TILE_MELD_SUIT,
                honor_size=T.FS_TILE_Z_MELD,
            )


def _draw_center(surface: pygame.Surface, ui: UiView) -> None:
    state, fonts = ui.state, ui.fonts
    if state.current_seat is None:
        text = "当前行动：—"
        color = T.TEXT_DIM
    elif state.current_seat == state.my_seat:
        text = "当前行动：你（本家）"
        color = T.ACTIVE
    else:
        text = f"当前行动：{state.rel_label(state.current_seat)} {state.name_of(state.current_seat)}"
        color = T.TEXT
    W.draw_text(surface, text, fonts.get(T.FS_H3), color, T.R_CENTER_LINE1, W.ALIGN_CENTER)

    remaining = "—" if state.remaining is None else str(state.remaining)
    W.draw_text(
        surface, f"牌山剩余 {remaining}", fonts.get(T.FS_H2), T.TEXT, T.R_CENTER_LINE2, W.ALIGN_CENTER
    )
    for i, note in enumerate(ui.extra_notes[:2]):
        W.draw_text(
            surface, note, fonts.get(T.FS_SMALL), T.WARN,
            (T.R_CENTER[0], T.R_CENTER[1] + T.R_CENTER[3] + i * 18, T.R_CENTER[2], 18),
            W.ALIGN_CENTER,
        )


def _draw_self_hand(surface: pygame.Surface, ui: UiView, feel=None) -> None:
    state, fonts = ui.state, ui.fonts
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
        pygame.draw.rect(surface, T.NEG, T.R_SELF_HAND, width=2)
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
        # §5.4 ② 的状态提示：画在 R_TOAST（唯一允许与其他区块重叠、且不会被手牌/副露遮挡的位置）
        face = state.hand[state.selected_index]
        if ui.toast_alpha <= 0:
            layer = pygame.Surface((T.R_TOAST[2], T.R_TOAST[3]), pygame.SRCALPHA)
            layer.fill((24, 32, 30, 220))
            surface.blit(layer, (T.R_TOAST[0], T.R_TOAST[1]))
            W.draw_text(
                surface,
                selection_hint(face),
                fonts.get(T.FS_SMALL),
                T.SELECT,
                T.R_TOAST,
                W.ALIGN_CENTER,
            )


def _draw_toast(surface: pygame.Surface, ui: UiView) -> None:
    if not ui.toast_text or ui.toast_alpha <= 0:
        return
    layer = pygame.Surface((T.R_TOAST[2], T.R_TOAST[3]), pygame.SRCALPHA)
    layer.fill((24, 32, 30, min(235, ui.toast_alpha)))
    surface.blit(layer, (T.R_TOAST[0], T.R_TOAST[1]))
    W.draw_text(
        surface,
        ui.toast_text,
        ui.fonts.get(T.FS_SMALL),
        T.TEXT,
        T.R_TOAST,
        W.ALIGN_CENTER,
        alpha=ui.toast_alpha,
    )
