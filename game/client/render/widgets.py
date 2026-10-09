"""渲染原语（UI_UX_SPEC §1.3：只有几何绘制 + 系统字体，无外部图片资源）。

本模块为纯绘制函数：无副作用、不发帧、不读网络。
"""

from __future__ import annotations

import logging

import pygame

from .. import theme as T
from ..tiles import SUIT_LABEL, SUIT_INDEX, honor_glyph, is_honor, is_valid_tile

log = logging.getLogger("mahjong.client.widgets")

_warned_tiles: set[str] = set()

ALIGN_LEFT = "left"
ALIGN_CENTER = "center"
ALIGN_RIGHT = "right"


def draw_panel(
    surface: pygame.Surface,
    rect: tuple[int, int, int, int],
    fill: tuple = T.PANEL,
    edge: tuple = T.PANEL_EDGE,
    radius: int = 6,
    width: int = 1,
) -> None:
    pygame.draw.rect(surface, fill, rect, border_radius=radius)
    if width > 0:
        pygame.draw.rect(surface, edge, rect, width=width, border_radius=radius)


def draw_border_alpha(
    surface: pygame.Surface,
    rect: tuple[int, int, int, int],
    color: tuple[int, int, int],
    alpha: float,
    width: int = 2,
    radius: int = 0,
) -> None:
    """半透明描边（用于座位高亮的「呼吸」与倒计时闪烁，§4.9/§6.1）。"""
    if alpha <= 0:
        return
    alpha = max(0.0, min(1.0, alpha))
    layer = pygame.Surface((rect[2], rect[3]), pygame.SRCALPHA)
    pygame.draw.rect(
        layer, (color[0], color[1], color[2], int(255 * alpha)), (0, 0, rect[2], rect[3]),
        width=width, border_radius=radius,
    )
    surface.blit(layer, (rect[0], rect[1]))


def draw_text(
    surface: pygame.Surface,
    text: str,
    font: pygame.font.Font,
    color: tuple,
    rect: tuple[int, int, int, int],
    align: str = ALIGN_LEFT,
    valign: str = "middle",
    alpha: int | None = None,
) -> pygame.Rect:
    """在 rect 内绘制单行文本，返回实际绘制矩形。"""
    image = font.render(text, True, color[:3])
    if alpha is not None and alpha < 255:
        image = image.convert_alpha()
        image.set_alpha(max(0, alpha))
    x, y, w, h = rect
    if align == ALIGN_CENTER:
        pos_x = x + (w - image.get_width()) // 2
    elif align == ALIGN_RIGHT:
        pos_x = x + w - image.get_width()
    else:
        pos_x = x
    if valign == "middle":
        pos_y = y + (h - image.get_height()) // 2
    elif valign == "bottom":
        pos_y = y + h - image.get_height()
    else:
        pos_y = y
    surface.blit(image, (pos_x, pos_y))
    return pygame.Rect(pos_x, pos_y, image.get_width(), image.get_height())


def suit_color(face: str) -> tuple:
    if len(face) == 2 and face[1] in SUIT_INDEX:
        # 字牌墨色（UI_UX_SPEC v1.3 §1.4 SUIT_Z）；数牌用各自花色色
        return {"m": T.SUIT_M, "p": T.SUIT_P, "s": T.SUIT_S, "z": T.SUIT_Z}[face[1]]
    return T.NEG


def draw_tile(
    surface: pygame.Surface,
    fonts: T.FontBook,
    rect: tuple[int, int, int, int],
    face: str,
    *,
    radius: int = 3,
    rank_size: int = T.FS_TILE_RIVER_RANK,
    suit_size: int = T.FS_TILE_RIVER_SUIT,
    honor_size: int = T.FS_TILE_Z_RIVER_CENTER,
    selected: bool = False,
    drawn: bool = False,
    win_tile: bool = False,
    pending: bool = False,
) -> None:
    """绘制一张麻将牌（§4.4 规则 1/2/4/5，v1.3 按数牌/字牌二分）。

    - **数牌**：`rank` 大号居中偏上 + 花色字小号右下角，颜色 = `SUIT_M`/`SUIT_P`/`SUIT_S`；
    - **字牌**（`1z`..`7z`）：**单个繁體字形居中**、无数字、无花色角标、不旋转，颜色 = `SUIT_Z`，
      字号由调用方按牌位档位传入（§4.4「字牌字体」列，实现方不得自行取值）。

    标记优先级：`selected`（3px 描边 + 上移 8px）> `drawn`（2px + 上移 4px + 右上角方块）
    > `win_tile`（3px SELECT 描边 + 1.08× 放大，调用方负责放大 rect）；
    `pending` 用于本地「待确认」灰显 + 下沉 6px（§5.3 第 1 行）。
    """
    lift = 0
    if selected:
        lift = T.HAND_SELECT_LIFT
    elif drawn:
        lift = T.HAND_DRAWN_LIFT
    offset = 0
    if pending:
        offset = 6
    x, y, w, h = rect
    target = pygame.Rect(x, y - lift + offset, w, h)

    fill = T.FACE
    if pending:
        fill = (196, 194, 184)
    pygame.draw.rect(surface, fill, target, border_radius=radius)

    if not is_valid_tile(face):
        if face not in _warned_tiles:
            _warned_tiles.add(face)
            log.warning("未知牌面字符串，渲染为「?」：%r", face)
        draw_text(surface, "?", fonts.get(rank_size, bold=True), T.NEG, target, ALIGN_CENTER)
        pygame.draw.rect(surface, T.FACE_EDGE, target, width=1, border_radius=radius)
        return

    color = suit_color(face)
    if is_honor(face):
        # 字牌（§1.3 规则 4 / §4.4 规则 2）：单字居中，无数字、无角标、不旋转
        draw_text(
            surface,
            honor_glyph(face) or "?",
            fonts.get(honor_size, bold=True),
            color,
            target,
            ALIGN_CENTER,
        )
    else:
        rank = face[0]
        suit = SUIT_LABEL[face[1]]
        draw_text(
            surface,
            rank,
            fonts.get(rank_size, bold=True),
            color,
            (target.x, target.y + int(h * 0.12), w, int(h * 0.6)),
            ALIGN_CENTER,
        )
        draw_text(
            surface,
            suit,
            fonts.get(suit_size),
            color,
            (target.x, target.y + h - suit_size - 2, w - 3, suit_size + 2),
            ALIGN_RIGHT,
            valign="middle",
        )
    pygame.draw.rect(surface, T.FACE_EDGE, target, width=1, border_radius=radius)

    if selected or win_tile:
        pygame.draw.rect(surface, T.SELECT, target, width=3, border_radius=radius)
    elif drawn:
        pygame.draw.rect(surface, T.DRAWN, target, width=2, border_radius=radius)
        marker = pygame.Rect(target.right - 14, target.y + 1, 14, 14)
        pygame.draw.rect(surface, T.DRAWN, marker)


def draw_tile_back(surface: pygame.Surface, rect: tuple[int, int, int, int], radius: int = 3) -> None:
    """牌背（§4.4 规则 3）：他家手牌**只**允许以此形式出现。"""
    pygame.draw.rect(surface, T.BACK, rect, border_radius=radius)
    pygame.draw.rect(surface, T.BACK_EDGE, rect, width=1, border_radius=radius)
    x, y, w, h = rect
    if w >= 24 and h >= 24:
        pygame.draw.line(surface, T.BACK_EDGE, (x + 3, y + 3), (x + w - 4, y + h - 4))


def draw_button(
    surface: pygame.Surface,
    fonts: T.FontBook,
    rect: tuple[int, int, int, int],
    label: str,
    *,
    enabled: bool = True,
    hovered: bool = False,
    pressed: bool = False,
    win: bool = False,
) -> None:
    """按钮（§5.2/§5.3 的禁用态与「胡」按钮金色底）。"""
    if not enabled:
        fill = T.BTN_DISABLED
        text_color = T.BTN_DISABLED_TEXT
    elif win:
        fill = T.BTN_WIN_HOVER if (hovered or pressed) else T.BTN_WIN
        text_color = T.TEXT
    else:
        if pressed:
            fill = T.BTN_DOWN
        elif hovered:
            fill = T.BTN_HOVER
        else:
            fill = T.BTN
        text_color = T.TEXT
    pygame.draw.rect(surface, fill, rect, border_radius=4)
    pygame.draw.rect(surface, T.PANEL_EDGE, rect, width=1, border_radius=4)
    draw_text(surface, label, fonts.get(T.FS_BODY), text_color, rect, ALIGN_CENTER)
