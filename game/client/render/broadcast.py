"""结算播报 · **阶段 A**（播报期整幅舞台）渲染（RULINGS v1.2 §2.3）。

两阶段的分界线是「播报是否正在跑」：

- **阶段 A**（`BroadcastView.running`）= 本模块。整幅舞台，**不画**四座位卡片 / 战绩块 /
  手牌区，但**必须画和牌张**（番种成立的物证）。所有新坐标都只活在本模块里。
- **阶段 B**（播报结束 → `game_start`）= `render/result.py` 的既有 `_render_round_end`，
  **一个既有 rect 都不改**（四卡 `y130 h96`、手牌 `(164,386,952,100)`、和牌张回 `(500,246)`）。

调用约定：`result.render()` 画完蒙层与主面板后调 `render_round_end()`；本模块不画蒙层、
不画主面板、不读状态写接口、不发帧、不 sleep。

绘制预算（RULINGS §2.3 硬约束 2）：阶段 A 最坏帧 **≤56 次 draw**。本实现的计数上限::

    舞台底 1 + 字幕条 3 + S1 8 槽×2（文本+高亮条）+ 当前行底 1 = 20
    + S2..S5 4 格×2 = 8 + S6 5（底+4 户）+ 和牌张 4 + 分隔线 2 + 提示行 1  = 40

文本缓存（硬约束 1）：`(text, color, size, bold, alpha)` → `Surface` 的字典缓存。
阶段 A 约 20 段/帧，不缓存 ⇒ 每帧 20 次 `font.render` + `convert_alpha()` 分配。
"""

from __future__ import annotations

import logging

import pygame

from .. import broadcast as BC
from .. import theme as T
from . import art
from . import widgets as W

log = logging.getLogger("mahjong.client.render.broadcast")

# ---------------------------------------------------------------- 坐标（RULINGS v1.2 §2.3 冻结）

R_CAPTION = (316, 220, 648, 28)        # 字幕条（紧凑面板内）
R_CAPTION_BAR = (316, 220, 4, 28)      # 左侧 4 px 金竖条
R_CAPTION_TEXT = (332, 220, 616, 28)   # 文本区
R_STAGE = (300, 210, 680, 330)         # 舞台底：只占桌心，不压左右摊牌
R_S1 = (316, 258, 330, 208)            # S1 番种区（8 槽 × 行高 26）
R_S2 = (660, 258, 148, 42)             # 牌型番合计
R_S3 = (818, 258, 146, 42)             # 加成 + 合计番数
R_S4 = (660, 308, 148, 42)             # 封顶
R_S5 = (818, 308, 146, 42)             # 倍率
R_S6 = (316, 470, 648, 48)             # 四户支付
R_WIN_TILE = (818, 360, 120, 96)       # 和牌张（紧凑面板右侧空档）
R_WIN_HAND = (340, 70, 600, 38)        # 赢家手牌条：名牌与桌面之间的空白带，不压四家摊牌
R_HINT = (430, 546, 420, 22)           # 播报提示行（面板下方、本家手牌上方）

#: S1 每一槽的左边界 / 宽度 / 行高（8×26 = 208，正好填满 R_S1 的高度）
S1_X = R_S1[0]
S1_W = 318
S1_ROW_H = 26
S1_SLOTS = 8
S1_PAD = 14           # 文本左缩进（给高亮条让位）
S1_FONT = T.FS_BODY   # 18 px：紧凑面板下仍能放下 8 行

#: 分隔线（2 条）：S1 与推导网格之间的竖线 + 推导网格与 S6 之间的横线
DIVIDER_X = 652
DIVIDER_Y = 462

#: 高亮条三态宽度（未播 0 / 当前 4 / 已播 2；「已讲完」只由条宽承载，不许把字调暗）
BAR_W_PENDING = 0
BAR_W_ACTIVE = 4
BAR_W_DONE = 2


# ---------------------------------------------------------------- 文本缓存


_TEXT_CACHE: dict[tuple, pygame.Surface] = {}
_TEXT_CACHE_LIMIT = 512
_PANEL_CACHE: dict[tuple, pygame.Surface] = {}


def _text_surface(fonts: T.FontBook, text: str, size: int, color: tuple, bold: bool = False) -> pygame.Surface:
    """`(text, color, size, bold, alpha)` → `Surface`（RULINGS §2.3 硬约束 1）。"""
    key = (text, tuple(color[:3]), size, bold)
    surface = _TEXT_CACHE.get(key)
    if surface is not None:
        return surface
    surface = fonts.get(size, bold=bold).render(text, True, color[:3])
    if len(_TEXT_CACHE) >= _TEXT_CACHE_LIMIT:
        _TEXT_CACHE.clear()     # 简单粗暴但够用：阶段 A 的文案在几帧内就稳定
    _TEXT_CACHE[key] = surface
    return surface


def _draw(
    surface: pygame.Surface,
    fonts: T.FontBook,
    text: str,
    rect: tuple[int, int, int, int],
    *,
    size: int,
    color: tuple,
    align: str = W.ALIGN_LEFT,
    bold: bool = False,
) -> int:
    """缓存的单行文本绘制；返回 0（未画）或 1（画了一次），便于预算对账。"""
    if not text:
        return 0
    image = _text_surface(fonts, text, size, color, bold)
    x, y, w, h = rect
    if align == W.ALIGN_CENTER:
        pos_x = x + (w - image.get_width()) // 2
    elif align == W.ALIGN_RIGHT:
        pos_x = x + w - image.get_width()
    else:
        pos_x = x
    surface.blit(image, (pos_x, y + (h - image.get_height()) // 2))
    return 1


def _alpha_panel(rect: tuple[int, int, int, int], color: tuple, radius: int = 0) -> pygame.Surface:
    """半透明底（`pygame.draw.rect` 会丢掉 RGBA 的 alpha，必须走 SRCALPHA 图层）。

    内容与尺寸恒定 ⇒ 每格只建一次，之后每帧只是一次 `blit`（零分配）。
    """
    key = (rect[2], rect[3], tuple(color), radius)
    layer = _PANEL_CACHE.get(key)
    if layer is None:
        # v2.1：平涂换成「竖向渐变 + 顶沿高光 + 极淡描边」的玻璃皮肤。
        # 皮肤在 `art` 层按尺寸缓存，**构建期一次性开销**，每帧仍只有 1 次 blit，
        # 阶段 A 的 draw 预算（≤56）不受影响。
        alpha = color[3] if len(color) > 3 else 255
        layer = art.strip(
            rect[2], rect[3], color,
            edge=(255, 255, 255, 18) if alpha >= 120 else None,
            radius=radius,
        ).copy()
        _PANEL_CACHE[key] = layer
    return layer


_RULE_CACHE: dict[int, pygame.Surface] = {}
_GLOW_CACHE: dict[tuple, pygame.Surface] = {}
_WIN_HAND_CACHE: dict[tuple, pygame.Surface] = {}


def _rule_surface(width: int) -> pygame.Surface:
    """金色细线（1 px，半透明）：缓存后单次 blit。"""
    layer = _RULE_CACHE.get(width)
    if layer is None:
        layer = pygame.Surface((width, 1), pygame.SRCALPHA)
        layer.fill((232, 192, 104, 60))
        _RULE_CACHE[width] = layer
    return layer


def _glow_surface(width: int, height: int) -> pygame.Surface:
    """和牌张光晕：三层同心圆角矩形，越往外越淡。"""
    key = (width, height)
    layer = _GLOW_CACHE.get(key)
    if layer is None:
        layer = pygame.Surface((width, height), pygame.SRCALPHA)
        for index, inset in enumerate((0, 10, 20)):
            pygame.draw.rect(
                layer, (232, 192, 104, 30 - index * 8),
                (inset, inset, width - inset * 2, height - inset * 2),
                border_radius=max(2, 20 - inset),
            )
        _GLOW_CACHE[key] = layer
    return layer


def _blit_panel(surface: pygame.Surface, rect: tuple[int, int, int, int], color: tuple, radius: int = 0) -> None:
    surface.blit(_alpha_panel(rect, color, radius), (rect[0], rect[1]))


# ---------------------------------------------------------------- 入口


def is_broadcasting(ui: object) -> bool:
    """阶段 A 的唯一判据（`render/result.py` 用）：**播报正在跑**才走本模块。

    注意不能用 `BroadcastView.active`（= plan 存在）——不播报的档（budget < 5100）
    与 `--no-feel` 都必须原样退 v2.0 静态页。
    """
    view = getattr(ui, "bc", None)
    return view is not None and view.running


def render_round_end(surface: pygame.Surface, ui: object) -> None:
    """阶段 A 整幅舞台。蒙层与主面板已由 `result.render()` 画好。"""
    view = getattr(ui, "bc", None)
    plan = getattr(view, "plan", None)
    if view is None or plan is None:      # pragma: no cover - 调用方已用 is_broadcasting 把关
        return
    fonts = ui.fonts
    state = ui.state

    # ① 舞台底（盖住四卡 / 战绩块 / 手牌，但不盖主面板边缘与页脚）
    _blit_panel(surface, R_STAGE, T.BC_ZONE, radius=6)
    # ①' 舞台上下各一道鎏金细线（纯 blit，不占 draw 预算）
    for edge_y in (R_STAGE[1] + 2, R_STAGE[1] + R_STAGE[3] - 3):
        surface.blit(_rule_surface(R_STAGE[2] - 32), (R_STAGE[0] + 16, edge_y))
    # ② 字幕条
    _draw_caption(surface, ui, view, plan)
    # ②' 赢家手牌条（面板上方；整条缓存后单次 blit）
    _draw_winner_hand(surface, ui, state)
    # ③ S1 番种区（8 槽）
    _draw_yaku_slots(surface, ui, view, plan)
    # ④ S2–S5 推导网格
    _draw_chain_cell(surface, ui, view, plan, R_S2, (BC.FOCUS_CHAIN, "base"), _s2_text(plan))
    _draw_chain_cell(surface, ui, view, plan, R_S3, (BC.FOCUS_CHAIN, "dealer"), _s3_text(plan))
    _draw_chain_cell(surface, ui, view, plan, R_S4, (BC.FOCUS_CHAIN, "cap"), _s4_text(plan),
                     fill=T.BC_CAP_FILL if plan.cap_state == BC.CAP_CAPPED else T.PANEL)
    _draw_chain_cell(surface, ui, view, plan, R_S5, (BC.FOCUS_CHAIN, "total"), _s5_text(plan))
    # ⑤ S6 四户
    _draw_payments(surface, ui, view, plan)
    # ⑥ 和牌张（必须保留：番种成立的物证）
    _draw_win_tile(surface, ui, state)
    # ⑦ 分隔线（2 条）
    _draw_dividers(surface)
    # ⑧ 倒计时行：播报期只显示「播报中…（空格跳过）」，**不显示秒数**
    _draw(surface, fonts, "播报中…（空格跳过）", R_HINT, size=T.FS_H3, color=T.TEXT, align=W.ALIGN_CENTER)


# ---------------------------------------------------------------- 分块


def _draw_caption(surface: pygame.Surface, ui: object, view, plan) -> None:
    text = view.caption or plan.static_caption
    _blit_panel(surface, R_CAPTION, T.BC_CAPTION_BG, radius=4)
    pygame.draw.rect(surface, T.SELECT, R_CAPTION_BAR)
    _draw(surface, ui.fonts, text, R_CAPTION_TEXT, size=T.FS_H3, color=T.TEXT, align=W.ALIGN_CENTER)


def _draw_yaku_slots(surface: pygame.Surface, ui: object, view, plan) -> None:
    fonts = ui.fonts
    rows = plan.yaku_rows
    for index in range(S1_SLOTS):
        y = R_S1[1] + index * S1_ROW_H
        focus = (BC.FOCUS_YAKU, index)
        row_state = view.row_state(focus)
        # 三态（RULINGS v1.2）：未播 条宽0 / 当前 条宽4 + 底 / 已播 条宽2；**无插值**
        bar_w = BAR_W_ACTIVE if row_state == "active" else (BAR_W_DONE if row_state == "done" else BAR_W_PENDING)
        if row_state == "active":
            _blit_panel(surface, (S1_X, y, S1_W, S1_ROW_H), T.BC_ACTIVE_BG, radius=3)
        if bar_w:
            pygame.draw.rect(surface, T.SELECT, (S1_X, y, bar_w, S1_ROW_H))
        if index >= len(rows):
            continue
        # 「已讲完」只由条宽承载 —— 已播/当前同色（TEXT），只有未播用 TEXT_DIM
        color = T.TEXT_DIM if row_state == "pending" else T.TEXT
        _draw(surface, fonts, _slot_text(rows[index]), (S1_X + S1_PAD, y, S1_W - S1_PAD, S1_ROW_H),
               size=S1_FONT, color=color)


def _slot_text(row) -> str:
    if row.merged_count:
        return f"另 {row.merged_count} 项 · {row.han} 番"
    if row.show_han:
        return f"{row.name} {row.han} 番"
    return row.name


def _draw_chain_cell(surface: pygame.Surface, ui: object, view, plan, rect, focus, text: str,
                     fill: tuple = T.PANEL) -> None:
    """S2–S5 的通用格子：半透明底 + 描边（当前步用 `SELECT` 描边）+ 居中文本。"""
    active = view.row_state(focus) == "active"
    _blit_panel(surface, rect, fill, radius=6)
    pygame.draw.rect(surface, T.SELECT if active else T.PANEL_EDGE, rect,
                     width=2 if active else 1, border_radius=6)
    _draw(surface, ui.fonts, text, rect, size=T.FS_H3, color=T.TEXT, align=W.ALIGN_CENTER)


def _draw_payments(surface: pygame.Surface, ui: object, view, plan) -> None:
    state = ui.state
    fonts = ui.fonts
    if view.row_state((BC.FOCUS_PAY, "P1")) == "active":
        _blit_panel(surface, R_S6, T.BC_ACTIVE_BG, radius=4)
    cell_w = R_S6[2] // 4
    for index in range(4):
        seat = index
        x = R_S6[0] + index * cell_w
        name = state.rel_label(seat) if hasattr(state, "rel_label") else f"座位{seat}"
        _draw(surface, fonts, name, (x, R_S6[1], cell_w, 20), size=T.FS_SMALL, color=T.TEXT_DIM)
        delta = plan.delta.get(seat)
        # 屏上不得出现「自摸 +0」：0 分一律写 "0"，不写 "+0"（RULINGS V6 的同款口径）
        text = "0" if delta == 0 else (f"{delta:+d}" if isinstance(delta, int) else "—")
        _draw(surface, fonts, text, (x, R_S6[1] + 20, cell_w, 28), size=T.FS_H2,
              color=T.score_color(delta), align=W.ALIGN_CENTER)


def _draw_winner_hand(surface: pygame.Surface, ui: object, state) -> None:
    """播报面板上方的赢家手牌条：整条预渲染缓存，每帧只 blit 一次。"""
    result = state.result if isinstance(getattr(state, "result", None), dict) else {}
    win = result.get("win") if isinstance(result.get("win"), dict) else None
    if win is None:
        return
    seat = win.get("seat")
    hand: list[str] = []
    for entry in result.get("results") or []:
        if isinstance(entry, dict) and entry.get("seat") == seat:
            raw = entry.get("hand")
            if isinstance(raw, list):
                hand = [t for t in raw if isinstance(t, str)]
            break
    if not hand:
        return
    tw, th, pitch = 24, 32, 26
    key = (tuple(hand), tw, th, pitch, win.get("tile"))
    strip = _WIN_HAND_CACHE.get(key)
    if strip is None:
        width = max(1, len(hand) * pitch)
        strip = pygame.Surface((width, th), pygame.SRCALPHA)
        for index, face in enumerate(hand):
            rect = (index * pitch, 0, tw, th)
            W.draw_tile(
                strip, ui.fonts, rect, face,
                rank_size=T.FS_TILE_RESULT_HAND_RANK,
                suit_size=T.FS_TILE_RESULT_HAND_SUIT,
                honor_size=T.FS_TILE_Z_RESULT_HAND,
            )
        if isinstance(win.get("tile"), str):
            last = max(i for i, t in enumerate(hand) if t == win["tile"]) if win["tile"] in hand else -1
            if last >= 0:
                W.draw_border_alpha(strip, (last * pitch, 0, tw, th), T.SELECT, 1.0, width=2, radius=4)
        _WIN_HAND_CACHE[key] = strip
    _blit_panel(surface, R_WIN_HAND, T.BC_CAPTION_BG, radius=4)
    _draw(surface, ui.fonts, "胡牌手牌", (R_WIN_HAND[0] + 10, R_WIN_HAND[1], 84, R_WIN_HAND[3]),
          size=T.FS_SMALL, color=T.SELECT, align=W.ALIGN_LEFT)
    x = R_WIN_HAND[0] + 96 + max(0, (R_WIN_HAND[2] - 96 - strip.get_width()) // 2)
    surface.blit(strip, (x, R_WIN_HAND[1] + (R_WIN_HAND[3] - th) // 2))


def _draw_win_tile(surface: pygame.Surface, ui: object, state) -> None:
    result = state.result if isinstance(getattr(state, "result", None), dict) else {}
    win = result.get("win") if isinstance(result.get("win"), dict) else None
    tile = win.get("tile") if win is not None else None
    if not isinstance(tile, str):
        return
    rect = (
        R_WIN_TILE[0] + (R_WIN_TILE[2] - T.RESULT_TILE[0]) // 2,
        R_WIN_TILE[1] + (R_WIN_TILE[3] - T.RESULT_TILE[1]) // 2,
        T.RESULT_TILE[0],
        T.RESULT_TILE[1],
    )
    # 和牌张垫一层金色光晕：整幅舞台上视觉焦点必须是「胡的那张牌」
    surface.blit(_glow_surface(R_WIN_TILE[2] + 32, R_WIN_TILE[3] + 32), (R_WIN_TILE[0] - 16, R_WIN_TILE[1] - 16))
    W.draw_tile(
        surface, ui.fonts, rect, tile,
        radius=8,
        rank_size=T.FS_TILE_RESULT_RANK,
        suit_size=T.FS_TILE_RESULT_SUIT,
        honor_size=T.FS_TILE_Z_RESULT,
        win_tile=True,
    )


def _draw_dividers(surface: pygame.Surface) -> None:
    pygame.draw.line(surface, T.PANEL_EDGE,
                     (DIVIDER_X, R_S1[1]), (DIVIDER_X, R_S1[1] + S1_SLOTS * S1_ROW_H), 2)
    pygame.draw.line(surface, T.PANEL_EDGE,
                     (R_STAGE[0] + 16, DIVIDER_Y), (R_STAGE[0] + R_STAGE[2] - 16, DIVIDER_Y), 2)


# ---------------------------------------------------------------- 文案


def _s2_text(plan) -> str:
    """段 2 牌型番合计；键缺失（L2/L3）时显式说「明细不可用」，不假装 0 番。"""
    if plan.level in (BC.LEVEL_NO_YAKU, BC.LEVEL_UNUSABLE):
        return "明细不可用"
    return f"牌型 {plan.base} 番" if plan.base is not None else "牌型 —"


def _s3_text(plan) -> str:
    """段 3：**禁止只显示「+1」不显示「= N 番」**（RULINGS §2.2 必现行）。"""
    total = plan.total_han
    total_text = f"{total} 番" if total is not None else "— 番"
    dealer = plan.dealer or 0
    if plan.level in (BC.LEVEL_NO_YAKU, BC.LEVEL_UNUSABLE):
        return "明细不可用"
    if dealer > 0:
        return f"庄家 +{dealer} = {total_text}"
    # dealer == 0：该格**不画数字 0**（RULINGS V6，「自摸 +0」不出现在屏上）
    return f"合计 {total_text}"


def _s4_text(plan) -> str:
    """段 4 封顶三子态。判据是 `base + dealer > 6`，**不是** `total >= 6`（打满 6 番不是被砍）。"""
    if plan.cap_state == BC.CAP_CAPPED:
        return f"{plan.capped_input} 番 → 封顶 6 番"
    if plan.cap_state == BC.CAP_EXACT:
        return "打满 6 番"
    return "未封顶"


def _s5_text(plan) -> str:
    return f"× {plan.mult} 倍" if plan.mult is not None else "× — 倍"


def clear_caches() -> None:
    """测试/切场用：丢弃文本与图层缓存（正常主循环**不**调用）。"""
    _TEXT_CACHE.clear()
    _PANEL_CACHE.clear()
