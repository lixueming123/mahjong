"""渲染原语（UI_UX_SPEC §1.3：几何绘制 + 系统字体 + CC0 牌面贴图）。

牌面贴图来自 FluffyStuff / riichi-mahjong-tiles（CC0 公有领域）：
34 种牌 + 牌背，运行时按牌位尺寸等比缩放并缓存。贴图缺失时回退到原有的
程序化牌面（万/筒/条/字），保证离线或资源损坏时仍能渲染。

本模块为纯绘制函数：无副作用、不发帧、不读网络。
"""

from __future__ import annotations

import logging
from pathlib import Path

import pygame

from .. import theme as T
from ..tiles import SUIT_INDEX, honor_glyph, is_honor, is_valid_tile
from . import art

log = logging.getLogger("mahjong.client.widgets")

_warned_tiles: set[str] = set()
_face_cache: dict[tuple, pygame.Surface] = {}
_FACE_CACHE_LIMIT = 1200
_FACE_CACHE_EVICT = 200

#: CC0 牌面贴图目录（client/assets/mahjong/faces/<face>.png）
_TILE_ASSET_DIR = Path(__file__).resolve().parents[1] / "assets" / "mahjong" / "faces"
#: Blender 预渲染的侧家牌墙（side/side_07..18.png，CC BY 4.0）
_SIDE_STRIP_DIR = Path(__file__).resolve().parents[1] / "assets" / "mahjong" / "side"
_SIDE_STRIP_CACHE: dict[int, pygame.Surface] = {}

ALIGN_LEFT = "left"
ALIGN_CENTER = "center"
ALIGN_RIGHT = "right"

#: 中文数字（万子牌面上半部分）
_CN_NUMERAL = "一二三四五六七八九"

# 图案排布取自传统麻将牌（维基「麻将牌」条目）：
# - 筒：2 一绿一蓝 / 3 斜线（绿红蓝）/ 4 四角对色 / 5 四角加红心 / 6 上二下四 /
#       7 上三斜线加下四 / 8 2×4 / 9 三行（蓝、红、绿）；
# - 条：3 上一下二 / 6 上三下三 / 7 上一下三下三 / 8 4+4 / 9 三行；
#   中间条（5 条）、最上条（7 条）、中列（9 条）为红。
_DOT_ROWS = {
    1: (1,),
    2: (1, 1),
    3: (1, 2),
    4: (2, 2),
    5: (2, 1, 2),
    6: (2, 4),
    7: (3, 4),
    8: (4, 4),
    9: (3, 3, 3),
}
_STICK_ROWS = {
    1: (1,),
    2: (1, 1),
    3: (1, 2),
    4: (2, 2),
    5: (2, 1, 2),
    6: (3, 3),
    7: (1, 3, 3),
    8: (4, 4),
    9: (3, 3, 3),
}
#: 每颗筒子的颜色（逐行逐颗）：传统牌用绿/蓝/红三色区分行，9 筒固定「蓝红绿」三行
_DOT_COLOR_ROWS = {
    1: ("P",),
    2: ("S", "P"),
    3: ("S", "MP"),
    4: ("PS", "SP"),
    5: ("PS", "M", "SP"),
    6: ("SS", "MMMM"),
    7: ("SSS", "MMMM"),
    8: ("PPPP", "PPPP"),
    9: ("PPP", "MMM", "SSS"),
}

#: 每根条子的颜色：5 条中行、7 条首行、9 条中列为红（"S" 绿 / "M" 红 / "P" 蓝）
_STICK_COLOR_ROWS = {
    5: ("SS", "M", "SS"),
    7: ("M", "SSS", "SSS"),
    9: ("SSS", "MMM", "SSS"),
}

#: 颜色代号 → 花色色（"S" 绿 / "M" 红 / "P" 蓝），大小写都收
_SUIT_TINT = {
    "m": T.SUIT_M, "p": T.SUIT_P, "s": T.SUIT_S,
    "M": T.SUIT_M, "P": T.SUIT_P, "S": T.SUIT_S,
}


# ---------------------------------------------------------------- 面板 / 文本


def draw_panel(
    surface: pygame.Surface,
    rect: tuple[int, int, int, int],
    fill: tuple = T.PANEL,
    edge: tuple = T.PANEL_EDGE,
    radius: int = 6,
    width: int = 1,
    shadow: bool = False,
) -> None:
    """玻璃面板（v2.1：竖向渐变 + 顶沿高光 + 1 px 描边，替代原来的平涂色块）。

    `shadow=True` 会先在下面铺一层投影（给卡片类大面板用）。
    """
    if rect[2] <= 0 or rect[3] <= 0:
        return
    art.panel(surface, rect, radius=radius, fill=fill, edge=edge, shadow=shadow)


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


# ---------------------------------------------------------------- 牌身与牌面


def _tile_body(width: int, height: int, radius: int, fill: tuple) -> pygame.Surface:
    """牌身（v3.0 **立牌**版）：牌是**竖着立在桌面上**的。

    摄像机在斜上方，所以一张立着的牌看到的是：最上面一条**牌顶面**（亮），
    它下面是整块**牌面**（正对着你），最底下落在桌面上的投影。
    上一版把厚度画在下面，读起来像一张平放的纸牌，这次翻上来。
    """
    band = max(4, int(height * 0.13))          # 牌顶面（从上往下看到的那一窄条）
    shadow = 3
    surface = pygame.Surface((width, height + band + shadow), pygame.SRCALPHA)

    # ① 落在桌布上的投影（牌底沿往下压一点）
    pygame.draw.rect(
        surface, (0, 0, 0, 52),
        (1, band + 1, width, height + shadow - 1),
        border_radius=radius,
    )
    # ② 牌顶面：立着的时候，顶面是朝上的一块亮面
    top = pygame.Rect(0, 0, width, band)
    pygame.draw.rect(surface, art.shade(T.TILE_SIDE, 1.06), top,
                     border_top_left_radius=radius, border_top_right_radius=radius)
    pygame.draw.line(
        surface, art.shade(T.TILE_SIDE, 1.22), (1, 1), (width - 2, 1),
    )
    # ③ 牌面：顶面下面那一整块正面
    face = pygame.Rect(0, band, width, height)
    pygame.draw.rect(surface, fill, face,
                     border_top_left_radius=max(1, radius // 2),
                     border_top_right_radius=max(1, radius // 2))
    tone = art.vertical_gradient(
        width, height, art.shade(fill, 1.03), art.shade(fill, 0.95)
    )
    mask = pygame.Surface((width, height), pygame.SRCALPHA)
    pygame.draw.rect(mask, (255, 255, 255, 255), (0, 0, width, height),
                     border_top_left_radius=max(1, radius // 2),
                     border_top_right_radius=max(1, radius // 2))
    tone.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
    surface.blit(tone, (0, band))
    # ④ 顶面与牌面的交界线 + 牌面轮廓
    pygame.draw.line(
        surface, art.shade(T.FACE_EDGE, 0.82), (1, band), (width - 2, band),
    )
    pygame.draw.rect(surface, T.FACE_EDGE, face, width=1,
                     border_top_left_radius=max(1, radius // 2),
                     border_top_right_radius=max(1, radius // 2))
    pygame.draw.rect(surface, T.FACE_EDGE, top, width=1,
                     border_top_left_radius=radius, border_top_right_radius=radius)
    return surface


def _pick_sizes(box: pygame.Rect, first: str, second: str, fonts, base: float) -> tuple:
    """二分式收敛，保证「两行字」在牌面内完整放下（不裁剪、不溢出）。"""
    size_first = max(8, int(box.h * base))
    size_second = max(8, int(box.h * base * 1.28))
    gap = max(1, int(box.h * 0.05))
    for _ in range(40):
        image_first = fonts.get(size_first, bold=True).render(first, True, (0, 0, 0))
        image_second = fonts.get(size_second, bold=True).render(second, True, (0, 0, 0))
        total = image_first.get_height() + image_second.get_height() + gap
        widest = max(image_first.get_width(), image_second.get_width())
        if total <= box.h and widest <= box.w:
            break
        if size_first <= 9 and size_second <= 9:
            break
        size_first = max(9, size_first - 1)
        size_second = max(9, size_second - 1)
    return size_first, size_second, gap


def _draw_wan(surface: pygame.Surface, fonts, box: pygame.Rect, rank: int) -> None:
    """万子：上「一…九」（墨蓝）下「萬」（红）——传统牌的上蓝下红。"""
    numeral = _CN_NUMERAL[rank - 1]
    size_first, size_second, gap = _pick_sizes(box, numeral, "萬", fonts, 0.24)
    image_first = fonts.get(size_first, bold=True).render(numeral, True, (44, 62, 120))
    image_second = fonts.get(size_second, bold=True).render("萬", True, T.SUIT_M)
    total = image_first.get_height() + image_second.get_height() + gap
    y = box.y + (box.h - total) // 2
    surface.blit(image_first, (box.x + (box.w - image_first.get_width()) // 2, y))
    surface.blit(
        image_second,
        (box.x + (box.w - image_second.get_width()) // 2, y + image_first.get_height() + gap),
    )


def _draw_wan_flat(surface: pygame.Surface, fonts, box: pygame.Rect, rank: int) -> None:
    """横放牌位上的万子：左「一…九」（墨蓝）右「萬」（红），保持正向可读。"""
    numeral = _CN_NUMERAL[rank - 1]
    size = max(9, int(box.h * 0.52))
    while size > 9:
        first = fonts.get(size, bold=True).render(numeral, True, (44, 62, 120))
        second = fonts.get(size, bold=True).render("萬", True, T.SUIT_M)
        if first.get_width() + second.get_width() + 2 <= box.w:
            break
        size -= 1
    first = fonts.get(size, bold=True).render(numeral, True, (44, 62, 120))
    second = fonts.get(size, bold=True).render("萬", True, T.SUIT_M)
    gap = 2
    total_w = first.get_width() + second.get_width() + gap
    x = box.x + max(0, (box.w - total_w) // 2)
    y = box.y + (box.h - max(first.get_height(), second.get_height())) // 2
    surface.blit(first, (x, y))
    surface.blit(second, (x + first.get_width() + gap, y))


def _dot(surface: pygame.Surface, center: tuple[int, int], radius: int, color: tuple) -> None:
    """一颗筒子：外环实心 + 白心 + 同色中心点（半径过小则退化为实心圆）。"""
    if radius <= 2:
        pygame.draw.circle(surface, color, center, max(1, radius))
        return
    pygame.draw.circle(surface, color, center, radius)
    pygame.draw.circle(surface, T.FACE, center, max(1, int(radius * 0.60)))
    pygame.draw.circle(surface, color, center, max(1, int(radius * 0.30)))


def _big_dot(surface: pygame.Surface, center: tuple[int, int], radius: int) -> None:
    """1 筒的大饼：蓝环 + 绿环 + 红心（传统牌里唯一的多色大圆）。"""
    pygame.draw.circle(surface, T.SUIT_P, center, radius)
    pygame.draw.circle(surface, T.FACE, center, max(1, int(radius * 0.74)))
    pygame.draw.circle(surface, T.SUIT_S, center, max(1, int(radius * 0.56)), width=max(1, radius // 6))
    pygame.draw.circle(surface, T.SUIT_M, center, max(1, int(radius * 0.26)))


def _stick(surface: pygame.Surface, rect: pygame.Rect, color: tuple) -> None:
    """一根条子：竹节短棒（上下各一道节线）。"""
    radius = max(1, min(rect.w, rect.h) // 4)
    pygame.draw.rect(surface, color, rect, border_radius=radius)
    if rect.h >= 11:
        node = art.shade(color, 0.68)
        for ratio in (0.3, 0.7):
            y = rect.y + int(rect.h * ratio)
            pygame.draw.line(surface, node, (rect.x + 1, y), (rect.right - 2, y))
    if rect.h >= 14:
        pygame.draw.line(surface, art.shade(color, 1.35), (rect.x + 1, rect.y + 1), (rect.right - 2, rect.y + 1))


def _draw_sparrow(surface: pygame.Surface, box: pygame.Rect) -> None:
    """1 条（幺鸡）：站枝麻雀——绿身、红冠红喙、蓝尾。仅在手牌/结算等大牌面启用。"""
    w, h = box.w, box.h
    branch_y = box.y + int(h * 0.80)
    body = pygame.Rect(box.x + int(w * 0.20), box.y + int(h * 0.32), int(w * 0.48), int(h * 0.38))

    # 枝（先画，压在鸟爪之下）
    pygame.draw.line(surface, T.SUIT_Z, (box.x + 2, branch_y), (box.right - 3, branch_y), max(1, h // 48))
    # 尾羽：一块朝左下的三角，别用三条线（小尺寸下会糊成一团）
    pygame.draw.polygon(
        surface, T.SUIT_P,
        [
            (body.left + 3, body.centery - 2),
            (box.x + 2, branch_y - max(2, h // 30)),
            (body.left + 3, body.bottom - 2),
        ],
    )
    pygame.draw.ellipse(surface, T.SUIT_S, body)
    # 翅：比身体深一档的弧
    pygame.draw.ellipse(surface, art.shade(T.SUIT_S, 0.78), body.inflate(-int(w * 0.16), -int(h * 0.10)))
    head_center = (body.x + int(body.w * 0.74), body.y - max(2, h // 26))
    head_radius = max(3, int(w * 0.15))
    pygame.draw.circle(surface, T.SUIT_S, head_center, head_radius)
    # 冠 + 喙 + 眼
    pygame.draw.circle(
        surface, T.SUIT_M, (head_center[0], head_center[1] - head_radius), max(1, int(w * 0.055))
    )
    pygame.draw.polygon(
        surface, T.SUIT_M,
        [
            (head_center[0] + head_radius - 1, head_center[1] - 1),
            (head_center[0] + head_radius + max(3, int(w * 0.13)), head_center[1] + max(1, h // 60)),
            (head_center[0] + head_radius - 1, head_center[1] + max(2, h // 34)),
        ],
    )
    pygame.draw.circle(
        surface, (26, 22, 18),
        (head_center[0] + max(1, head_radius // 3), head_center[1] - max(1, head_radius // 3)),
        max(1, head_radius // 3),
    )
    # 爪：两条短线落在枝上
    for offset in (-int(w * 0.06), int(w * 0.06)):
        pygame.draw.line(
            surface, T.SUIT_Z,
            (body.centerx + offset, body.bottom - 2),
            (body.centerx + offset, branch_y),
            max(1, h // 50),
        )


def _draw_pips(surface: pygame.Surface, box: pygame.Rect, count: int, kind: str, tall: bool) -> None:
    """筒（dot）/ 条（stick）按传统排布绘制；`tall` 为真时 1 条画成麻雀。"""
    if kind == "dot" and count == 1:
        _big_dot(surface, (box.centerx, box.centery), max(3, int(min(box.w, box.h) * 0.40)))
        return
    if kind == "stick" and count == 1:
        if tall and box.h >= 46:
            _draw_sparrow(surface, box)
            return
        _stick(
            surface,
            pygame.Rect(box.centerx - max(2, box.w // 8), box.y + box.h // 10,
                        max(4, box.w // 4), box.h - box.h // 5),
            T.SUIT_S,
        )
        return

    if kind == "dot":
        rows = _DOT_ROWS[count]
        colors = _DOT_COLOR_ROWS[count]
    else:
        rows = _STICK_ROWS[count]
        colors = _STICK_COLOR_ROWS.get(count, tuple("S" * per_row for per_row in rows))

    row_h = box.h / len(rows)
    for row_index, per_row in enumerate(rows):
        cell_w = box.w / per_row
        row_colors = colors[row_index] if row_index < len(colors) else "S" * per_row
        for col in range(per_row):
            cx = box.x + cell_w * (col + 0.5)
            cy = box.y + row_h * (row_index + 0.5)
            tint = _SUIT_TINT.get(row_colors[col] if col < len(row_colors) else "S", T.SUIT_S)
            if kind == "dot":
                radius = max(1, int(min(cell_w, row_h) * 0.40))
                _dot(surface, (int(cx), int(cy)), radius, tint)
            else:
                # 竹节细长（约 1:3），过粗会像色块而不像「条」
                width = max(2, int(cell_w * 0.30))
                height = max(3, int(row_h * 0.84))
                _stick(surface, pygame.Rect(int(cx - width / 2), int(cy - height / 2), width, height), tint)


def _draw_honor(surface: pygame.Surface, fonts, box: pygame.Rect, face: str, honor_size: int) -> None:
    """字牌：東南西北墨色 / 中红 / 發绿 / 白为空心方框（传统白板）。"""
    glyph = honor_glyph(face) or "?"
    if face == "5z":
        # 白板：不放字形，画一个内嵌方框（比全空白更像一张「白」，也不易被误读为空牌）
        inset = max(3, box.w // 5)
        frame = pygame.Rect(
            box.x + inset, box.y + inset, max(2, box.w - inset * 2), max(2, box.h - inset * 2)
        )
        line = max(1, box.w // 22)
        pygame.draw.rect(surface, art.shade(T.SUIT_P, 0.86), frame, width=line, border_radius=max(1, box.w // 8))
        pygame.draw.rect(
            surface, art.shade(T.SUIT_P, 1.25),
            frame.inflate(-max(2, box.w // 4), -max(2, box.w // 4)),
            width=max(1, line - 1), border_radius=max(1, box.w // 10),
        )
        return
    color = T.SUIT_M if face == "7z" else (T.SUIT_S if face == "6z" else T.SUIT_Z)
    image = fonts.get(honor_size, bold=True).render(glyph, True, color)
    # 单字居中；极窄牌位时按宽度再收一档，保证不越界
    while image.get_width() > box.w and honor_size > 8:
        honor_size -= 1
        image = fonts.get(honor_size, bold=True).render(glyph, True, color)
    surface.blit(image, (box.x + (box.w - image.get_width()) // 2, box.y + (box.h - image.get_height()) // 2))


def _face_art(
    fonts,
    face: str,
    width: int,
    height: int,
    radius: int,
    rank_size: int,
    suit_size: int,
    honor_size: int,
    pending: bool,
) -> pygame.Surface:
    fill = (196, 194, 184) if pending else T.FACE
    surface = _tile_body(width, height, radius, fill)
    if not is_valid_tile(face):
        if face not in _warned_tiles:
            _warned_tiles.add(face)
            log.warning("未知牌面字符串，渲染为「?」：%r", face)
        draw_text(surface, "?", fonts.get(max(10, rank_size), bold=True), T.NEG, (0, 0, width, height), ALIGN_CENTER)
        return surface

    if is_honor(face):
        # 字牌（§4.4 规则 2）：单字居中，无数字、无角标、不旋转
        _draw_honor(surface, fonts, pygame.Rect(0, 0, width, height), face, honor_size)
        return surface

    rank = int(face[0])
    suit = face[1]
    if suit == "m":
        # 万：竖牌两行字（一…九 + 萬）；横放牌位改左右横排，保持正向可读。
        if height >= 30 and width >= 22:
            pad_y = max(2, int(height * 0.09))
            pad_x = max(2, int(width * 0.08))
            _draw_wan(surface, fonts, pygame.Rect(pad_x, pad_y, width - pad_x * 2, height - pad_y * 2), rank)
        elif width >= 26:
            pad = max(2, int(width * 0.08))
            _draw_wan_flat(
                surface, fonts, pygame.Rect(pad, 1, width - pad * 2, height - 2), rank
            )
        else:
            draw_text(
                surface, str(rank), fonts.get(rank_size, bold=True), T.SUIT_M,
                (0, 0, width, height), ALIGN_CENTER,
            )
        return surface

    # 筒 / 条：图案化
    pad = max(2, int(width * 0.13))
    vertical_pad = max(2, int(height * 0.10))
    box = pygame.Rect(pad, vertical_pad, width - pad * 2, height - vertical_pad * 2)
    if box.w >= 8 and box.h >= 8:
        _draw_pips(surface, box, rank, "dot" if suit == "p" else "stick", tall=height >= 46)
    else:
        draw_text(
            surface, str(rank), fonts.get(rank_size, bold=True), suit_color(face),
            (0, 0, width, height), ALIGN_CENTER,
        )
    return surface


def _tint_back(image: pygame.Surface) -> pygame.Surface:
    """``back.png`` 自带大红，统一染成 ``T.BACK`` 橙金（不改 CC0 原始素材）。

    侧家牌墙是浅粉贴图乘 ``SIDE_STRIP_TINT``；牌背这张是纯红，红通道已饱和，
    同样的乘法只会得到橙红。所以先灰度化、抬亮到接近白，再乘目标色。
    所有牌背（暗杠占位 / 3D 牌墙顶 / 旋转牌背）都走这里，颜色一次对齐。
    """
    gray = pygame.transform.grayscale(image).copy()
    gray.fill(T.BACK_LIFT, special_flags=pygame.BLEND_RGB_ADD)
    gray.fill(T.BACK, special_flags=pygame.BLEND_RGB_MULT)
    return gray


def _image_tile_surface(face: str, width: int, height: int, pending: bool) -> pygame.Surface | None:
    """按牌位尺寸等比缩放 CC0 牌面贴图；缺图返回 None（调用方回退程序化）。"""
    if width <= 0 or height <= 0:
        return None
    path = _TILE_ASSET_DIR / f"{face}.png"
    if not path.exists():
        return None
    try:
        source = pygame.image.load(str(path)).convert_alpha()
    except (pygame.error, OSError) as exc:  # pragma: no cover - 防御性
        log.warning("牌面贴图加载失败：%s（%s），回退程序化绘制", path, exc)
        return None
    src_w, src_h = source.get_size()
    scale = min(width / src_w, height / src_h)
    draw_w = max(1, int(round(src_w * scale)))
    draw_h = max(1, int(round(src_h * scale)))
    image = pygame.transform.smoothscale(source, (draw_w, draw_h))
    if pending:
        image = pygame.transform.grayscale(image)
    if face == "back":
        image = _tint_back(image)
    out = pygame.Surface((width, height), pygame.SRCALPHA)
    out.blit(image, ((width - draw_w) // 2, (height - draw_h) // 2))
    return out


def tile_face_surface(
    fonts,
    face: str,
    width: int,
    height: int,
    radius: int,
    rank_size: int,
    suit_size: int,
    honor_size: int,
    pending: bool = False,
) -> pygame.Surface:
    """带缓存的牌面（CC0 贴图优先，缺图回退程序化图案）。返回的 Surface 只读。"""
    key = (face, width, height, radius, rank_size, suit_size, honor_size, pending)
    surface = _face_cache.get(key)
    if surface is None:
        if len(_face_cache) >= _FACE_CACHE_LIMIT:
            # 淘汰最旧的一批（而不是 clear 全表）：飞行动画每帧都会产生新尺寸，
            # 整表清空会让下一帧把几十张牌重新画一遍，帧耗时出现 20 ms 级尖刺。
            for stale in list(_face_cache)[: _FACE_CACHE_EVICT]:
                _face_cache.pop(stale, None)
        surface = _image_tile_surface(face, width, height, pending)
        if surface is None:
            surface = _face_art(
                fonts, face, width, height, radius, rank_size, suit_size, honor_size, pending
            )
        _face_cache[key] = surface
    return surface


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
    """绘制一张麻将牌（§4.4 规则 1/2/4/5，v2.1 图案化）。

    - **万**：上「一…九」下「萬」（红）；**筒**：圆点按传统排布（蓝，1/5/7 带红心）；
      **条**：竹节短棒（绿，1 条红头）；**字**：東南西北墨色 / 中红 / 發绿 / 白空心框。
    - 牌身统一「底沿厚度 + 顶沿高光」，形成立牌感（v2.1 新增，不改任何几何冻结值）。

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
    if w <= 0 or h <= 0:
        return

    art_surface = tile_face_surface(
        fonts, face, w, h, radius, rank_size, suit_size, honor_size, pending
    )
    surface.blit(art_surface, (target.x, target.y))

    if selected or win_tile:
        draw_border_alpha(surface, (target.x, target.y, w, h), T.SELECT, 1.0, width=3, radius=radius)
    elif drawn:
        draw_border_alpha(surface, (target.x, target.y, w, h), T.DRAWN, 1.0, width=2, radius=radius)
        marker = min(14, max(6, w // 4))
        badge = pygame.Rect(target.right - marker - 1, target.y + 1, marker, marker)
        pygame.draw.rect(surface, T.DRAWN, badge, border_top_right_radius=radius)


def draw_tile_rotated(
    surface: pygame.Surface,
    fonts,
    rect: tuple[int, int, int, int],
    face: str,
    *,
    angle: int = 90,
    radius: int = 3,
    rank_size: int = T.FS_TILE_SIDE_RANK,
    suit_size: int = T.FS_TILE_SIDE_SUIT,
    honor_size: int = T.FS_TILE_Z_RIVER_SIDE,
) -> None:
    """旋转 90° 的牌面（v2.5：上/下家的牌河整块横放，和真麻将一致）。

    `rect` 给的是**旋转后**的落位（宽高已交换）；底层牌面按 `(h, w)` 渲染后旋转。
    """
    w, h = rect[2], rect[3]
    if w <= 0 or h <= 0:
        return
    base = tile_face_surface(fonts, face, h, w, radius, rank_size, suit_size, honor_size, False)
    key = ("rot", face, h, w, radius, rank_size, suit_size, honor_size, angle)
    rotated = _face_cache.get(key)
    if rotated is None:
        rotated = pygame.transform.rotate(base, angle)
        if len(_face_cache) < _FACE_CACHE_LIMIT:
            _face_cache[key] = rotated
    rw, rh = rotated.get_width(), rotated.get_height()
    surface.blit(
        rotated,
        (rect[0] + (w - rw) // 2, rect[1] + (h - rh) // 2),
    )


def tile_back_surface(width: int, height: int, face: str = "back", *, plain: bool = False) -> pygame.Surface | None:
    """CC0 牌背贴图 + 亮顶/侧厚（v4.8）；`plain=True` 只用原始贴图，不补任何几何。

    牌墙不能是平贴的红卡片：顶部留一条更亮的“牌顶”，主体沿用外部牌背贴图，
    底沿加一层暗边，形成牌块厚度。侧家牌墙与普通牌背共用同一套。
    """
    if plain:
        return _image_tile_surface(face, width, height, False)
    if face != "back":
        return _image_tile_surface(face, width, height, False)
    if width <= 0 or height <= 0:
        return None
    key = ("back3d", width, height)
    cached = _face_cache.get(key)
    if cached is not None:
        return cached
    band = max(2, int(round(height * 0.14)))
    body_h = max(1, height - band)
    source = _image_tile_surface("back", width, body_h, False)
    if source is None:
        return None
    out = pygame.Surface((width, height), pygame.SRCALPHA)
    # 主体牌背（外部贴图），底沿略暗
    out.blit(source, (0, band))
    pygame.draw.rect(out, (0, 0, 0, 46), (1, band + 1, width - 2, body_h - 1), border_radius=2)
    # 顶部牌顶：更亮的一条
    top = pygame.Surface((width, band), pygame.SRCALPHA)
    pygame.draw.rect(
        top, art.shade(T.BACK, 1.55), (0, 0, width, band),
        border_top_left_radius=3, border_top_right_radius=3,
    )
    pygame.draw.line(top, art.shade(T.BACK, 1.9), (1, 0), (width - 2, 0))
    out.blit(top, (0, 0))
    pygame.draw.line(out, art.shade(T.BACK, 0.55), (1, band), (width - 2, band))
    if len(_face_cache) < _FACE_CACHE_LIMIT:
        _face_cache[key] = out
    return out


def side_strip_surface(count: int) -> pygame.Surface | None:
    """侧家整条牌墙原图（1..18 张）。不足 7 张时从 side_07 按比例裁，素材与长条一致。"""
    n = max(1, min(18, int(count)))
    cached = _SIDE_STRIP_CACHE.get(n)
    if cached is not None:
        return cached
    src_n = n if n >= 7 else 7
    path = _SIDE_STRIP_DIR / f"side_{src_n:02d}.png"
    if not path.exists():
        return None
    try:
        image = pygame.image.load(str(path)).convert_alpha()
    except (pygame.error, OSError) as exc:  # pragma: no cover
        log.warning("侧墙贴图加载失败：%s（%s）", path, exc)
        return None
    # v4.0：雀魂参考帧的牌山是橙金，Blender 素材是浅粉红。一次 RGB 乘法
    # 整体调色，保留渐变 / 高光 / alpha，也不改 CC BY 原始素材。
    image = image.copy()
    image.fill(T.SIDE_STRIP_TINT, special_flags=pygame.BLEND_RGB_MULT)
    if n < src_n:
        cut = max(1, int(round(image.get_width() * n / src_n)))
        image = image.subsurface((0, 0, cut, image.get_height())).copy()
    _SIDE_STRIP_CACHE[n] = image
    return image


def draw_tile_back(surface: pygame.Surface, rect: tuple[int, int, int, int], radius: int = 3) -> None:
    """牌背（§4.4 规则 3）：他家手牌**只**允许以此形式出现。

    v4.2 起使用 CC0 牌背贴图；缺图时回退到程序化牌背。
    """
    x, y, w, h = rect
    if w <= 0 or h <= 0:
        return
    key = ("back", w, h, radius)
    cached = _face_cache.get(key)
    if cached is None:
        cached = tile_back_surface(w, h, plain=True)
        if cached is None:
            band = max(3, int(h * 0.16))
            cached = pygame.Surface((w, h + band + 3), pygame.SRCALPHA)
            body = pygame.Rect(0, band, w, h)
            pygame.draw.rect(cached, (0, 0, 0, 52), (1, band + 1, w, h + 2), border_radius=radius)
            pygame.draw.rect(
                cached, art.shade(T.BACK, 1.55), (0, 0, w, band),
                border_top_left_radius=radius, border_top_right_radius=radius,
            )
            pygame.draw.line(cached, art.shade(T.BACK, 1.85), (1, 1), (w - 2, 1))
            tone = art.vertical_gradient(w, h, art.shade(T.BACK, 1.18), art.shade(T.BACK, 0.9))
            mask = pygame.Surface((w, h), pygame.SRCALPHA)
            pygame.draw.rect(mask, (255, 255, 255, 255), body,
                             border_top_left_radius=max(1, radius // 2),
                             border_top_right_radius=max(1, radius // 2))
            tone.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
            cached.blit(tone, (0, band))
            pygame.draw.rect(cached, T.BACK_EDGE, body, width=1, border_radius=radius)
            pygame.draw.rect(
                cached, T.BACK_EDGE, (0, 0, w, band), width=1,
                border_top_left_radius=radius, border_top_right_radius=radius,
            )
        if len(_face_cache) < _FACE_CACHE_LIMIT:
            _face_cache[key] = cached
    surface.blit(cached, (x, y))


# ---------------------------------------------------------------- 按钮 / 头像


def draw_tile_back_rotated(
    surface: pygame.Surface, center: tuple[int, int], size: int, angle: int = 45
) -> None:
    """旋转的牌背（v2.5：侧家的手牌墙斜着码，像参考图里的透视牌墙）。"""
    size = max(4, int(size))
    key = ("back_rot", size, angle)
    rotated = _face_cache.get(key)
    if rotated is None:
        base = pygame.Surface((size, size), pygame.SRCALPHA)
        draw_tile_back(base, (0, 0, size, size), radius=max(1, size // 5))
        rotated = pygame.transform.rotate(base, angle)
        if len(_face_cache) < _FACE_CACHE_LIMIT:
            _face_cache[key] = rotated
    surface.blit(rotated, (center[0] - rotated.get_width() // 2, center[1] - rotated.get_height() // 2))


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
    radius: int = 9,
) -> None:
    """按钮（§5.2/§5.3 的禁用态与「胡」按钮金色底；v2.1 加投影与渐变）。"""
    if not enabled:
        tone, state = "disabled", "idle"
        text_color = T.BTN_DISABLED_TEXT
    elif win:
        tone = "win"
        state = "pressed" if pressed else ("hover" if hovered else "idle")
        text_color = (46, 32, 10)
    else:
        tone = "normal"
        state = "pressed" if pressed else ("hover" if hovered else "idle")
        text_color = T.TEXT
    skin = art.button_skin(rect[2], rect[3], tone, state, radius=radius)
    surface.blit(skin, (rect[0], rect[1]))
    text_rect = (rect[0], rect[1] + (1 if pressed and enabled else 0), rect[2], rect[3] - 2)
    draw_text(surface, label, fonts.get(T.FS_BODY, bold=True), text_color, text_rect, ALIGN_CENTER)


def draw_avatar(
    surface: pygame.Surface,
    fonts: T.FontBook,
    center: tuple[int, int],
    size: int,
    name: str,
    *,
    ring: tuple | None = None,
    dim: bool = False,
) -> None:
    """座位头像（圆形 + 首字）。`ring` 用于庄家金环 / 当前行动高亮 / 断线红环。"""
    initial = (name.strip()[:1] or "?").upper()
    skin = art.avatar(size, name, initial, fonts, ring=ring, dim=dim)
    surface.blit(skin, (center[0] - size // 2, center[1] - size // 2))


def draw_chip(
    surface: pygame.Surface,
    fonts: T.FontBook,
    rect: tuple[int, int, int, int],
    text: str,
    *,
    fill: tuple = (0, 0, 0, 120),
    color: tuple = T.TEXT,
    size: int = T.FS_MICRO,
    bold: bool = False,
) -> None:
    """小胶囊标签（「13 张」「庄」「本家」）。"""
    skin = art.strip(rect[2], rect[3], fill, edge=None)
    surface.blit(skin, (rect[0], rect[1]))
    draw_text(surface, text, fonts.get(size, bold=bold), color, rect, ALIGN_CENTER)
