"""程序化美术层：牌桌质感、玻璃面板、按钮皮肤、头像。

v2.1 视觉升级（对标腾讯麻将的观感）：**仍然只用 pygame 几何绘制**（`UI_UX_SPEC` §1.3
硬性约束不变，不引入任何图片资源），只是把「一层平涂色块」换成「投影 + 渐变 + 高光」
三层叠加。所有函数都按参数把结果 `Surface` 缓存下来，单帧只做 blit，
60 fps 下 Python 侧几乎没有额外开销。

约定：
- 返回的 `Surface` 是**只读缓存**，调用方不得就地修改（需要改就再叠一层）。
- 尺寸参数一律是**逻辑像素**（1280×720 坐标系）。
"""

from __future__ import annotations

import math
import random

import pygame

from .. import theme as T

_cache: dict[tuple, pygame.Surface] = {}
_CACHE_LIMIT = 900
_CACHE_EVICT = 150


# ---------------------------------------------------------------- 基础工具


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def lerp_color(c1: tuple, c2: tuple, t: float) -> tuple[int, int, int]:
    t = max(0.0, min(1.0, t))
    return (
        int(round(lerp(c1[0], c2[0], t))),
        int(round(lerp(c1[1], c2[1], t))),
        int(round(lerp(c1[2], c2[2], t))),
    )


def shade(color: tuple, factor: float) -> tuple[int, int, int]:
    """factor > 1 提亮、< 1 压暗。"""
    return tuple(max(0, min(255, int(round(c * factor)))) for c in color[:3])  # type: ignore[return-value]


def _remember(key: tuple, surface: pygame.Surface) -> pygame.Surface:
    if len(_cache) >= _CACHE_LIMIT:
        # 只淘汰最旧的一批：整表清空会在下一帧触发几十次重建，造成帧耗时尖刺
        for stale in list(_cache)[: _CACHE_EVICT]:
            _cache.pop(stale, None)
    _cache[key] = surface
    return surface


def _cached(key: tuple, build) -> pygame.Surface:
    surface = _cache.get(key)
    if surface is None:
        surface = _remember(key, build())
    return surface


def vertical_gradient(
    width: int,
    height: int,
    top: tuple,
    bottom: tuple,
    alpha_top: int = 255,
    alpha_bottom: int = 255,
) -> pygame.Surface:
    """1 px 宽的竖向渐变条 → smoothscale 放大（比逐像素 set_at 快一个量级）。"""
    width = max(1, int(width))
    height = max(1, int(height))
    strip = pygame.Surface((1, height), pygame.SRCALPHA)
    for y in range(height):
        t = y / max(1, height - 1)
        color = lerp_color(top, bottom, t)
        strip.set_at((0, y), (*color, int(round(lerp(alpha_top, alpha_bottom, t)))))
    if width == 1:
        return strip
    return pygame.transform.smoothscale(strip, (width, height))


def shadow_surface(width: int, height: int, radius: int, alpha: int, spread: int) -> pygame.Surface:
    """投影图层（缓存）：从外到内叠 `spread` 圈圆角矩形，越往外越淡。"""
    width = max(2, int(width))
    height = max(2, int(height))

    def build() -> pygame.Surface:
        pad = spread * 2
        surface = pygame.Surface((width + pad * 2, height + pad * 2), pygame.SRCALPHA)
        for step in range(spread, 0, -1):
            layer_alpha = int(alpha * (0.32 + 0.22 * (spread - step)))
            layer_alpha = max(0, min(255, layer_alpha))
            if layer_alpha <= 2:
                continue
            pygame.draw.rect(
                surface, (0, 0, 0, layer_alpha),
                (pad - step * 2, pad - step * 2, width + step * 4, height + step * 4),
                border_radius=radius + step * 2,
            )
        pygame.draw.rect(surface, (0, 0, 0, alpha), (pad, pad, width, height), border_radius=radius)
        return surface

    return _cached(("shadow", width, height, radius, alpha, spread), build)


def drop_shadow(
    surface: pygame.Surface,
    rect: tuple[int, int, int, int],
    radius: int = 8,
    offset: tuple[int, int] = (0, 3),
    alpha: int = 90,
    spread: int = 2,
) -> None:
    """投影：按 (w,h,radius,alpha,spread) 缓存后单次 blit（不再每帧分配画布）。"""
    x, y, w, h = rect
    if w <= 0 or h <= 0:
        return
    pad = spread * 2
    surface.blit(
        shadow_surface(w, h, radius, alpha, spread),
        (x - pad + offset[0], y - pad + offset[1]),
    )


# ---------------------------------------------------------------- 牌桌


def felt(width: int = T.LOGICAL_W, height: int = T.LOGICAL_H) -> pygame.Surface:
    """牌桌：径向受光的绒面 + 四周压暗 + 台边金线。

    受光点略偏上（模拟顶灯），边缘压到近黑绿，让中间牌区自然成为视觉焦点。
    """

    def build() -> pygame.Surface:
        # 低分辨率算光照，再双线性放大：1280×720 的平滑渐变
        small_w, small_h = 160, 90
        small = pygame.Surface((small_w, small_h))
        cx, cy = small_w * 0.5, small_h * 0.42
        max_d = math.hypot(small_w * 0.56, small_h * 0.62)
        core = T.FELT_CORE
        edge = T.FELT_EDGE
        for y in range(small_h):
            dy = (y - cy) * 1.18
            for x in range(small_w):
                dist = math.hypot(x - cx, dy) / max_d
                t = min(1.0, dist ** 1.25)
                small.set_at((x, y), lerp_color(core, edge, t))
        # 三级缩放 = 廉价的三次模糊：96 → 24 → 96 再放大，块状感全部糊掉
        blurred = pygame.transform.smoothscale(small, (24, 14))
        blurred = pygame.transform.smoothscale(blurred, (small_w, small_h))
        blurred = pygame.transform.smoothscale(blurred, (width // 4, height // 4))
        surface = pygame.transform.smoothscale(blurred, (width, height))

        # 台边：外圈暗色收边 + 内侧一道极淡金线（只在四周，不遮牌区）
        pygame.draw.rect(surface, (6, 24, 18), (0, 0, width, height), width=6)
        rail = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(rail, (206, 168, 90, 26), (7, 7, width - 14, height - 14), width=1, border_radius=6)
        pygame.draw.rect(rail, (0, 0, 0, 40), (7, 7, width - 14, height - 14), width=2, border_radius=6)
        surface.blit(rail, (0, 0))

        # 台面缝线：一圈极淡的同心圆角矩形，暗示真实牌桌的包边
        stitch = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(
            stitch, (255, 255, 255, 14),
            (26, 42, width - 52, height - 96), width=1, border_radius=26,
        )
        surface.blit(stitch, (0, 0))
        return surface

    return _cached(("felt", width, height), build)


# ---------------------------------------------------------------- v3.0 参考视频复原
# 《明星三缺一 2004》的画面语言和「绿绒 + 木台边」完全是两回事：
#   背景     人物身后一张糊掉的室内照片（暖灰墙 + 货架）
#   牌桌     一整块粉色绒布铺满窗口下半部，桌沿就是窗口边缘，没有木边
#   桌面     米白斜线交叉成大菱形格 + 撒着浅蓝线稿小图案
# 三个函数都只在换尺寸时构建一次，单帧只 blit。


def hall(width: int = T.LOGICAL_W, height: int = T.LOGICAL_H) -> pygame.Surface:
    """人物身后的室内背景（糊掉的实景照片感）。

    做法：先在一张 96×54 的小画布上摆「竖向货架 + 一层层商品 + 零星色块」，
    再放大到全屏——缩放本身就是廉价的高斯模糊，出来的糊感正好对上 2004 年的
    实拍背景。最后叠顶灯与四角压暗。
    """

    def build() -> pygame.Surface:
        small_w, small_h = 96, 54
        small = pygame.Surface((small_w, small_h))
        for y in range(small_h):
            base = lerp_color(T.HALL_WALL, T.HALL_DARK, min(1.0, (y / max(1, small_h - 1)) ** 1.1))
            pygame.draw.line(small, base, (0, y), (small_w, y))

        rng = random.Random(20041001)          # 固定种子：每次构建的糊背景完全一样
        # 竖向隔板（货架立柱）
        for col in range(0, small_w, 7):
            board = shade(T.HALL_WALL, 0.78 + 0.34 * rng.random())
            pygame.draw.rect(small, board, (col, 0, 2, small_h))
        # 横向层板 + 层板上的商品色点
        for row in range(5, small_h, 6):
            pygame.draw.line(small, shade(T.HALL_WALL, 0.58), (0, row), (small_w, row))
            for col in range(1, small_w, 2):
                if rng.random() < 0.62:
                    tint = rng.choice(
                        ((150, 118, 92), (108, 128, 116), (168, 148, 118),
                         (96, 96, 118), (134, 106, 118))
                    )
                    pygame.draw.rect(small, tint, (col, row - 3, 2, 3))
        # 三级缩放 = 廉价的三次模糊：96 → 24 → 96 再放大，块状感全部糊掉
        blurred = pygame.transform.smoothscale(small, (24, 14))
        blurred = pygame.transform.smoothscale(blurred, (small_w, small_h))
        blurred = pygame.transform.smoothscale(blurred, (width // 4, height // 4))
        surface = pygame.transform.smoothscale(blurred, (width, height))

        # 顶灯：一条偏暖的亮带，让上半部分不那么闷
        light = pygame.Surface((width, height), pygame.SRCALPHA)
        for index, spread in enumerate((0, 46, 96)):
            pygame.draw.ellipse(
                light, (255, 226, 194, 18 - index * 5),
                (-120 - spread, -300 - spread, width + 240 + spread * 2, 460 + spread * 2),
            )
        surface.blit(light, (0, 0))

        # 四角压暗：把注意力压到中间的牌桌
        vignette = pygame.Surface((width, height), pygame.SRCALPHA)
        for step in range(70):
            alpha = min(96, int(1.3 * step))
            if alpha <= 0:
                continue
            pygame.draw.rect(
                vignette, (0, 0, 0, alpha),
                (step, step, width - step * 2, height - step * 2), width=1,
            )
        surface.blit(vignette, (0, 0))

        # 压暗 + 轻微去饱和：背景只该是「一团氛围」，不能和牌抢注意力
        dim = pygame.Surface((width, height), pygame.SRCALPHA)
        dim.fill((30, 22, 24, 24))
        surface.blit(dim, (0, 0))
        return surface

    return _cached(("hall", width, height), build)


def _doodle_gift(surface, x: int, y: int, size: int, color: tuple) -> None:
    """礼盒：方盒 + 十字缎带。"""
    box = pygame.Rect(x, y, size, int(size * 0.78))
    pygame.draw.rect(surface, color, box, width=2)
    pygame.draw.line(surface, color, (box.centerx, box.y), (box.centerx, box.bottom), 2)
    pygame.draw.line(surface, color, (box.x, box.y + box.h // 3), (box.right, box.y + box.h // 3), 2)
    pygame.draw.arc(surface, color, (box.centerx - size // 4, box.y - size // 4, size // 2, size // 2),
                    0.2, math.pi - 0.2, 2)


def _doodle_bottle(surface, x: int, y: int, size: int, color: tuple) -> None:
    """瓶子：细长颈 + 圆肩。"""
    pygame.draw.line(surface, color, (x + size // 2, y), (x + size // 2, y + size // 3), 2)
    pygame.draw.ellipse(surface, color, (x, y + size // 3, size, size - size // 3), 2)


def _doodle_flask(surface, x: int, y: int, size: int, color: tuple) -> None:
    """三角烧瓶。"""
    pygame.draw.lines(
        surface, color, False,
        [(x, y + size), (x + size // 3, y + size // 2), (x + size // 3, y),
         (x + size * 2 // 3, y), (x + size * 2 // 3, y + size // 2), (x + size, y + size)],
        2,
    )


def _doodle_hat(surface, x: int, y: int, size: int, color: tuple) -> None:
    """帽子：帽檐 + 帽顶。"""
    pygame.draw.arc(surface, color, (x, y + size // 3, size, size // 2), 0, math.pi, 2)
    pygame.draw.line(surface, color, (x, y + size // 3 + size // 4), (x + size, y + size // 3 + size // 4), 2)


def _doodle_flower(surface, x: int, y: int, size: int, color: tuple) -> None:
    """五瓣小花。"""
    radius = max(2, size // 4)
    for index in range(5):
        angle = index * 2 * math.pi / 5 - math.pi / 2
        cx = x + size // 2 + int(math.cos(angle) * radius)
        cy = y + size // 2 + int(math.sin(angle) * radius)
        pygame.draw.circle(surface, color, (cx, cy), radius, 2)


#: 桌面上的线稿图案：位置按桌面尺寸的比例给，缩放时不会跑位
_DOODLES = (
    (_doodle_gift, 0.17, 0.30),
    (_doodle_bottle, 0.09, 0.55),
    (_doodle_flask, 0.30, 0.62),
    (_doodle_hat, 0.41, 0.26),
    (_doodle_flower, 0.58, 0.24),
    (_doodle_gift, 0.71, 0.52),
    (_doodle_flask, 0.83, 0.34),
    (_doodle_bottle, 0.92, 0.62),
    (_doodle_hat, 0.50, 0.62),
    (_doodle_flower, 0.24, 0.44),
    (_doodle_flower, 0.78, 0.44),
    (_doodle_gift, 0.63, 0.72),
)


def table_cloth(
    width: int,
    height: int,
    top: int,
    far_half: int,
    near_half: int,
    center_x: int | None = None,
) -> pygame.Surface:
    """粉色桌布（参考视频的招牌桌面）：梯形 + 米白斜格 + 浅蓝线稿 + 桌沿高光。

    `top` 是桌沿在整屏里的 y；`far_half` / `near_half` 是远沿与近沿的半宽。
    长度按 `height`（整屏高）给，超出画面的部分自然被裁掉——和参考里
    「桌子铺到画面外」的感觉一致。
    """
    cx = T.LOGICAL_W // 2 if center_x is None else center_x

    def build() -> pygame.Surface:
        surface = pygame.Surface((width, height), pygame.SRCALPHA)
        # ① 底色：上浅下深
        surface.blit(vertical_gradient(width, height, T.FELT_PINK_TOP, T.FELT_PINK_BOTTOM), (0, 0))

        # ② 受光：桌心偏上一块柔光，让中间亮、四角暗
        light = pygame.Surface((width, height), pygame.SRCALPHA)
        for rx, ry, alpha in ((0.66, 0.46, 18), (0.46, 0.32, 15), (0.26, 0.18, 12)):
            glow_rect = pygame.Rect(0, 0, int(width * rx), int((height - top) * ry))
            glow_rect.center = (cx, int(top + (height - top) * 0.54))
            pygame.draw.ellipse(light, (255, 238, 228, alpha), glow_rect)
        surface.blit(light, (0, 0))

        # ③ 斜格：两组平行线分别平行于两条对角线，交叉成大菱形
        lattice = pygame.Surface((width, height), pygame.SRCALPHA)
        step = 168
        for direction in (1, -1):
            start_x = cx - far_half if direction > 0 else cx + far_half
            end_x = cx + near_half if direction > 0 else cx - near_half
            for offset in range(-width, width + step, step):
                pygame.draw.line(
                    lattice, (*T.FELT_LATTICE, 40),
                    (start_x + offset, top), (end_x + offset, height), 3,
                )
        surface.blit(lattice, (0, 0))

        # ③' 织物噪点：一眼看不出，但粉色不再是一块死板的平涂
        weave = pygame.Surface((width, height), pygame.SRCALPHA)
        noise = random.Random(20041002)
        for _ in range(width * height // 190):
            px = noise.randrange(width)
            py = noise.randrange(top, height)
            weave.set_at((px, py), (255, 255, 255, noise.choice((8, 12, 16))))
        surface.blit(weave, (0, 0))

        # ④ 浅蓝线稿图案
        doodles = pygame.Surface((width, height), pygame.SRCALPHA)
        for draw, fx, fy in _DOODLES:
            draw(
                doodles,
                int(width * fx),
                int(top + (height - top) * fy),
                max(18, int((height - top) * 0.05)),
                (*T.FELT_DOODLE, 96),
            )
        surface.blit(doodles, (0, 0))

        # ⑤ 裁剪成梯形
        mask = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.polygon(
            mask, (255, 255, 255, 255),
            [(cx - far_half, top), (cx + far_half, top),
             (cx + near_half, height), (cx - near_half, height)],
        )
        surface.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)

        # ⑥ 桌沿：桌沿**上面**一道投影（桌子压在房间地面上），桌沿本身一道亮边
        above = pygame.Surface((width, max(1, top)), pygame.SRCALPHA)
        for row in range(min(top, 26)):
            alpha = int(70 * (row / max(1, min(top, 26))) ** 1.6)
            above.fill((28, 12, 16, alpha), (0, top - 1 - row, width, 1))
        surface.blit(above, (0, 0))
        pygame.draw.line(
            surface, (252, 228, 206, 170), (cx - far_half, top + 1), (cx + far_half, top + 1), 3,
        )
        lip = pygame.Surface((width, 26), pygame.SRCALPHA)
        for index in range(26):
            lip.fill((0, 0, 0, int(46 * (1 - index / 26) ** 1.6)), (0, index, width, 1))
        surface.blit(lip, (0, top + 2))

        # ⑥' 四角暗角：参考里离桌心越远越暗，这是「一整块布」的立体感来源
        shade = pygame.Surface((width, height), pygame.SRCALPHA)
        for step in range(120):
            alpha = int(84 * (1 - step / 120) ** 1.5)
            if alpha <= 0:
                continue
            pygame.draw.rect(
                shade, (72, 18, 34, alpha),
                (step, top + step // 3, width - step * 2, height - top - step // 3), width=1,
            )
        surface.blit(shade, (0, 0))

        # ⑦ 左右两条斜边压暗，让桌布有厚度
        edge = pygame.Surface((width, height), pygame.SRCALPHA)
        for index in range(16):
            alpha = int(50 * (1 - index / 16) ** 1.4)
            pygame.draw.line(edge, (0, 0, 0, alpha), (cx - near_half + index, height), (cx - far_half + index, top), 1)
            pygame.draw.line(edge, (0, 0, 0, alpha), (cx + near_half - index, height), (cx + far_half - index, top), 1)
        surface.blit(edge, (0, 0))
        return surface

    return _cached(("cloth", width, height, top, far_half, near_half, cx), build)



# ---------------------------------------------------------------- 面板 / 卡片


def card(
    width: int,
    height: int,
    radius: int = 10,
    fill: tuple = T.PANEL,
    edge: tuple = T.PANEL_EDGE,
) -> pygame.Surface:
    """玻璃卡片：竖向渐变 + 顶部内高光 + 1 px 描边（投影由调用方用 `drop_shadow` 另画）。"""
    width = max(2, int(width))
    height = max(2, int(height))

    def build() -> pygame.Surface:
        surface = pygame.Surface((width, height), pygame.SRCALPHA)
        base = fill[:3]
        alpha = fill[3] if len(fill) > 3 else 255
        tone = vertical_gradient(width, height, shade(base, 1.55), shade(base, 0.92), alpha, alpha)
        mask = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(mask, (255, 255, 255, 255), (0, 0, width, height), border_radius=radius)
        tone.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(tone, (0, 0))

        # 顶部内高光（一条 1 px 亮线，只在圆角内）
        gloss = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(gloss, (255, 255, 255, 30), (0, 0, width, height), width=1, border_radius=radius)
        surface.blit(gloss, (0, 0))
        pygame.draw.rect(surface, edge, (0, 0, width, height), width=1, border_radius=radius)
        return surface

    return _cached(("card", width, height, radius, fill, edge), build)


def panel(
    surface: pygame.Surface,
    rect: tuple[int, int, int, int],
    radius: int = 10,
    fill: tuple = T.PANEL,
    edge: tuple = T.PANEL_EDGE,
    shadow: bool = True,
) -> None:
    """把 `card()` 贴到目标画布上，可选先铺一层投影。"""
    if rect[2] <= 0 or rect[3] <= 0:
        return
    if shadow:
        drop_shadow(surface, rect, radius=radius, offset=(0, 4), alpha=118, spread=3)
    surface.blit(card(rect[2], rect[3], radius=radius, fill=fill, edge=edge), (rect[0], rect[1]))


def backdrop(width: int, height: int, top: tuple, bottom: tuple) -> pygame.Surface:
    """非牌桌界面（登录 / 大厅）的背景：竖向渐变 + 中心提亮 + 四周压暗。"""

    def build() -> pygame.Surface:
        surface = vertical_gradient(width, height, top, bottom)
        glow = pygame.Surface((width, height), pygame.SRCALPHA)
        for index, radius in enumerate((520, 400, 300)):
            alpha = 8 + index * 4
            pygame.draw.ellipse(
                glow, (120, 220, 170, alpha),
                (width // 2 - radius, height // 2 - int(radius * 0.62), radius * 2, int(radius * 1.24)),
            )
        surface.blit(glow, (0, 0))
        pygame.draw.rect(surface, (5, 18, 13), (0, 0, width, height), width=6)
        return surface

    return _cached(("backdrop", width, height, top, bottom), build)


def fade_tile(
    fonts,
    face: str,
    width: int,
    height: int,
    alpha: int,
    angle: float = 0.0,
):
    """水印牌（登录 / 大厅背景装饰）：渲染一次、降低 alpha、可旋转，全部走缓存。"""

    def build():
        from . import widgets as widgets_module  # 延迟导入，避免与 widgets 循环依赖

        tile = widgets_module.tile_face_surface(
            fonts, face, width, height, 6,
            T.FS_TILE_HAND_RANK, T.FS_TILE_HAND_SUIT, T.FS_TILE_Z_HAND,
        ).copy()
        tile.set_alpha(alpha)
        if angle:
            tile = pygame.transform.rotozoom(tile, angle, 1.0)
            tile.set_alpha(alpha)
        return tile

    return _cached(("fade_tile", face, width, height, alpha, angle), build)


# ---------------------------------------------------------------- 房间与牌桌


def room(width: int = T.LOGICAL_W, height: int = T.LOGICAL_H) -> pygame.Surface:
    """房间背景：上部暖色墙面 / 下部木地板 / 桌面后方的柔光，外加四周压暗。

    牌桌本身由 `table_surface()` 画在这层之上，所以这里只需要「屋子」的氛围。
    """

    def build() -> pygame.Surface:
        surface = vertical_gradient(width, height, (72, 62, 50), (34, 28, 24))
        # 墙面：竖向木饰面板的接缝（极淡，避免抢戏）
        panel = pygame.Surface((width, height), pygame.SRCALPHA)
        for x in range(0, width, 96):
            pygame.draw.line(panel, (0, 0, 0, 26), (x, 0), (x, int(height * 0.62)), 2)
            pygame.draw.line(panel, (255, 236, 200, 8), (x + 2, 0), (x + 2, int(height * 0.62)), 1)
        surface.blit(panel, (0, 0))
        # 地板：更暗的一条带 + 地平线
        floor_top = int(height * 0.60)
        surface.blit(vertical_gradient(width, height - floor_top, (52, 40, 30), (24, 18, 14)),
                     (0, floor_top))
        pygame.draw.line(surface, (58, 46, 36), (0, floor_top), (width, floor_top), 2)
        # 桌面上方的吊灯光斑
        lamp = pygame.Surface((width, height), pygame.SRCALPHA)
        for index, (rx, ry, alpha) in enumerate(((560, 320, 20), (400, 230, 22), (250, 145, 26))):
            pygame.draw.ellipse(
                lamp, (255, 226, 170, alpha),
                (width // 2 - rx, int(height * 0.36) - ry, rx * 2, ry * 2),
            )
        surface.blit(lamp, (0, 0))
        # 四角压暗
        vignette = pygame.Surface((width, height), pygame.SRCALPHA)
        for step in range(6):
            alpha = 16 + step * 7
            pygame.draw.rect(
                vignette, (0, 0, 0, alpha),
                (step * 6, step * 6, width - step * 12, height - step * 12),
                width=10, border_radius=40,
            )
        surface.blit(vignette, (0, 0))
        return surface

    return _cached(("room", width, height), build)


def table_surface(width: int, height: int, rim: int, radius: int) -> pygame.Surface:
    """实体牌桌（v2.7 立体版）：木台边（带斜面）+ 下沉的绒面 + 织物纹理 + 投影。

    层次（从下到上）：投影 → 木框本体 → 木框斜面（上亮下暗）→ 内侧阴影（绒面下沉）
    → 绒面（径向受光 + 织纹 + 暗角）→ 鎏金包边。返回的画布四周留 `pad` 给投影。
    """
    pad = 30
    width = max(4, int(width))
    height = max(4, int(height))

    def build() -> pygame.Surface:
        surface = pygame.Surface((width + pad * 2, height + pad * 2), pygame.SRCALPHA)
        outer = pygame.Rect(pad, pad, width, height)

        # ① 桌面在地板上的投影（多层柔和）
        for spread, alpha in ((10, 26), (6, 34), (0, 60)):
            pygame.draw.ellipse(
                surface, (0, 0, 0, alpha),
                (pad - 8 - spread, pad + 16 + spread // 2,
                 width + 16 + spread * 2, height + 10 + spread),
            )

        # ② 木框本体（竖向渐变）
        wood = vertical_gradient(width, height, T.WOOD_HI, T.WOOD_LO)
        mask = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(mask, (255, 255, 255, 255), (0, 0, width, height), border_radius=radius)
        wood.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(wood, (outer.x, outer.y))

        # ③ 木框斜面：外沿一道暗边、内侧一圈亮面（像真实桌沿被打磨过）
        pygame.draw.rect(surface, (0, 0, 0, 110), outer, width=2, border_radius=radius)
        lip = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(
            lip, (255, 226, 170, 46), (3, 3, width - 6, height - 6), width=3,
            border_radius=max(2, radius - 3),
        )
        surface.blit(lip, (outer.x, outer.y))
        # 木纹
        grain = pygame.Surface((width, height), pygame.SRCALPHA)
        for offset in range(6, height - 6, 13):
            pygame.draw.line(grain, (0, 0, 0, 16), (10, offset), (width - 10, offset), 1)
        grain_mask = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(grain_mask, (255, 255, 255, 255), (0, 0, width, height), border_radius=radius)
        grain.blit(grain_mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(grain, (outer.x, outer.y))

        # ④ 绒面（下沉）：先铺一层比桌面小 `rim` 的绿，再在内侧压一圈阴影
        inner = pygame.Rect(pad + rim, pad + rim, width - rim * 2, height - rim * 2)
        felt_surface = felt(inner.width, inner.height).convert_alpha()
        weave = pygame.Surface((inner.width, inner.height), pygame.SRCALPHA)
        for x in range(0, inner.width, 3):
            pygame.draw.line(weave, (255, 255, 255, 7), (x, 0), (x, inner.height), 1)
        for y in range(0, inner.height, 3):
            pygame.draw.line(weave, (0, 0, 0, 9), (0, y), (inner.width, y), 1)
        felt_surface.blit(weave, (0, 0))
        # 边缘暗角：让绒面看起来是「凹」进木框里的
        for depth, alpha in ((0, 90), (2, 54), (5, 30), (9, 16)):
            pygame.draw.rect(
                felt_surface, (0, 0, 0, alpha), (depth, depth, inner.width - depth * 2, inner.height - depth * 2),
                width=max(2, 8 - depth), border_radius=max(2, radius - rim - depth),
            )
        felt_mask = pygame.Surface((inner.width, inner.height), pygame.SRCALPHA)
        pygame.draw.rect(felt_mask, (255, 255, 255, 255), (0, 0, inner.width, inner.height),
                         border_radius=max(2, radius - rim))
        felt_surface.blit(felt_mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(felt_surface, (inner.x, inner.y))

        # ⑤ 鎏金包边 + 台布内圈绣线
        pygame.draw.rect(
            surface, (232, 192, 104, 150), inner.inflate(2, 2), width=2,
            border_radius=max(2, radius - rim + 1),
        )
        ornament = pygame.Surface((inner.width, inner.height), pygame.SRCALPHA)
        band = pygame.Rect(24, 24, inner.width - 48, inner.height - 48)
        pygame.draw.rect(ornament, (232, 192, 104, 22), band, width=1,
                         border_radius=max(2, radius - rim - 20))
        for corner_x, corner_y in (
            (band.x + 30, band.y + 30), (band.right - 30, band.y + 30),
            (band.x + 30, band.bottom - 30), (band.right - 30, band.bottom - 30),
        ):
            span = 10
            pygame.draw.polygon(
                ornament, (232, 192, 104, 30),
                [(corner_x, corner_y - span), (corner_x + span, corner_y),
                 (corner_x, corner_y + span), (corner_x - span, corner_y)],
                width=1,
            )
        surface.blit(ornament, (inner.x, inner.y))
        return surface

    return _cached(("table", width, height, rim, radius), build)


# ---------------------------------------------------------------- 人物立绘


_SKIN_TONES = ((246, 220, 194), (236, 199, 168), (222, 180, 146))
_HAIR_TONES = ((38, 34, 32), (58, 42, 36), (28, 26, 28))
_ROBE_TONES = (
    (58, 88, 148), (146, 58, 58), (44, 112, 88), (118, 84, 152), (156, 118, 58), (70, 104, 116),
)


def _seed_index(seed: str, salt: int, modulo: int) -> int:
    value = salt
    for char in seed:
        value = (value * 131 + ord(char)) & 0xFFFFFFFF
    return value % modulo


def bust(height: int, seed: str, dim: bool = False) -> pygame.Surface:
    """**人物半身立绘**：坐在桌沿后面、只露出上半身的那种。

    画法（v3.0 重画）：头大、肩窄（2004 年那批休闲游戏的夸张比例），
    整体先铺一层深色剪影当描边，再把皮肤/头发/衣服画上去——这样人物从糊背景里
    跳得出来，也不会像上一版那样变成一坨深色椭圆。

    底部**不做淡出**：调用方会把桌布盖在它下面，桌沿那条线就是干净利落的切口。
    """
    height = max(60, int(height))
    width = height                       # 正方形画布：肩宽不会被裁成直边
    head_w = int(width * 0.40)
    head_h = int(height * 0.50)
    head = pygame.Rect((width - head_w) // 2, int(height * 0.02), head_w, head_h)
    cx = width // 2
    shoulder_top = head.bottom - int(head_h * 0.05)

    torso_w = int(width * 0.90)
    shoulder_h = int(height * 0.50)

    def silhouette(surface, grow: int, color: tuple) -> None:
        """一件「人形」：宽肩（椭圆的上一半）+ 到底的躯干 + 脖子 + 头。"""
        pygame.draw.ellipse(
            surface, color,
            (cx - torso_w // 2 - grow, shoulder_top - grow,
             torso_w + grow * 2, shoulder_h + grow * 2),
        )
        pygame.draw.polygon(
            surface, color,
            [
                (cx - torso_w // 2 - grow, shoulder_top + shoulder_h // 2),
                (cx + torso_w // 2 + grow, shoulder_top + shoulder_h // 2),
                (cx + int(width * 0.49), height),
                (cx - int(width * 0.49), height),
            ],
        )
        pygame.draw.rect(
            surface, color,
            (cx - int(head_w * 0.21) - grow, head.bottom - int(head_h * 0.34),
             int(head_w * 0.42) + grow * 2, int(head_h * 0.42)),
        )
        pygame.draw.ellipse(surface, color, head.inflate(grow * 2, grow * 2))

    def build() -> pygame.Surface:
        skin = _SKIN_TONES[_seed_index(seed, 7, len(_SKIN_TONES))]
        hair = _HAIR_TONES[_seed_index(seed, 31, len(_HAIR_TONES))]
        robe = _ROBE_TONES[_seed_index(seed, 97, len(_ROBE_TONES))]
        style = _seed_index(seed, 173, 3)
        if dim:
            skin, hair, robe = shade(skin, 0.66), shade(hair, 0.68), shade(robe, 0.58)

        surface = pygame.Surface((width, height), pygame.SRCALPHA)
        # ① 剪影描边
        silhouette(surface, 3, (24, 18, 18, 235))
        # ② 躯干（衣服）：左亮右暗的竖向渐变，肩膀才显得有体积
        silhouette(surface, 0, robe)
        # 体积：整张画布做一次「正片叠底」——透明像素 alpha=0，不会被染出一个方块
        surface.blit(
            vertical_gradient(width, height, (255, 255, 255), (146, 140, 138)),
            (0, 0), special_flags=pygame.BLEND_RGBA_MULT,
        )
        # 肩线高光：贴着肩头的一小段弧，把「肩膀」读出来
        pygame.draw.arc(
            surface, shade(robe, 1.45),
            (cx - torso_w // 2 + 6, shoulder_top + 4, torso_w - 12, shoulder_h - 8),
            math.radians(28), math.radians(152), max(1, height // 130),
        )
        # ③ 衣领：两侧浅色斜襟，中间留出脖子
        collar = shade(robe, 1.55)
        for side in (-1, 1):
            pygame.draw.polygon(
                surface, collar,
                [
                    (cx + side * int(width * 0.15), shoulder_top + int(height * 0.01)),
                    (cx + side * int(head_w * 0.10), shoulder_top + int(height * 0.17)),
                    (cx + side * int(head_w * 0.02), shoulder_top + int(height * 0.10)),
                    (cx + side * int(width * 0.07), shoulder_top - int(height * 0.01)),
                ],
            )
        # ④ 脖子（压在衣领上、头下面）
        pygame.draw.rect(
            surface, shade(skin, 0.80),
            (cx - int(head_w * 0.19), head.bottom - int(head_h * 0.36),
             int(head_w * 0.38), int(head_h * 0.42)),
            border_bottom_left_radius=max(2, head_w // 8),
            border_bottom_right_radius=max(2, head_w // 8),
        )
        # ⑤ 头发（头的上一层，先铺侧发再盖头顶）
        if style == 2:
            for side in (-1, 1):
                pygame.draw.ellipse(
                    surface, hair,
                    (cx + side * int(head_w * 0.40) - int(head_w * 0.16),
                     head.y + int(head_h * 0.20), int(head_w * 0.32), int(head_h * 1.02)),
                )
        # ⑥ 头 + 脸
        pygame.draw.ellipse(surface, skin, head)
        cap = pygame.Rect(head.x - 2, head.y - 4, head.w + 4, int(head.h * 0.52))
        pygame.draw.ellipse(surface, hair, cap)
        pygame.draw.ellipse(
            surface, skin,
            (head.x + int(head.w * 0.12), head.y + int(head.h * 0.30),
             int(head.w * 0.76), int(head.h * 0.64)),
        )
        for side in (-1, 1):
            # 鬓角
            pygame.draw.ellipse(
                surface, hair,
                (cx + side * int(head_w * 0.42) - max(2, int(head_w * 0.08)),
                 head.y + int(head_h * 0.20), max(4, int(head_w * 0.16)), int(head_h * 0.46)),
            )
            # 耳朵
            pygame.draw.circle(
                surface, shade(skin, 0.92),
                (cx + side * int(head_w * 0.50), head.y + int(head_h * 0.56)),
                max(2, int(head_w * 0.07)),
            )
        # ⑦ 五官
        eye_y = head.y + int(head.h * 0.56)
        eye_dx = max(4, int(head.w * 0.19))
        eye_w = max(5, int(head.w * 0.16))
        eye_h = max(3, int(head.h * 0.062))
        for side in (-1, 1):
            pygame.draw.ellipse(
                surface, (250, 249, 246),
                (cx + side * eye_dx - eye_w // 2, eye_y - eye_h // 2, eye_w, eye_h),
            )
            pygame.draw.circle(
                surface, (52, 40, 36), (cx + side * eye_dx, eye_y), max(2, eye_h // 2),
            )
        for side in (-1, 1):
            # 上眼线：2004 年那批画法，一条短线就够传神
            pygame.draw.arc(
                surface, (58, 44, 40),
                (cx + side * eye_dx - eye_w // 2, eye_y - eye_h, eye_w, eye_h * 2),
                0.15, math.pi - 0.15, max(1, height // 140),
            )
        brow_y = eye_y - int(head.h * 0.13)
        for side in (-1, 1):
            pygame.draw.line(
                surface, shade(hair, 1.05),
                (cx + side * int(eye_dx * 1.6), brow_y),
                (cx + side * int(eye_dx * 0.5), brow_y + int(head.h * 0.015)),
                max(2, height // 90),
            )
        pygame.draw.line(
            surface, shade(skin, 0.86),
            (cx, eye_y + int(head.h * 0.06)), (cx, eye_y + int(head.h * 0.13)),
            max(1, height // 120),
        )
        mouth_y = head.y + int(head.h * 0.82)
        pygame.draw.arc(
            surface, (176, 96, 90),
            (cx - int(head.w * 0.10), mouth_y - int(head.h * 0.045),
             int(head.w * 0.20), int(head.h * 0.09)),
            math.radians(200), math.radians(340), max(1, height // 100),
        )
        # ⑧ 左上轮廓光：从暗背景里把人的边缘勾出来
        rim = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.arc(
            rim, (255, 238, 214, 70),
            (head.x - 2, head.y - 2, head.w + 4, head.h + 4),
            math.radians(110), math.radians(205), max(2, head_w // 26),
        )
        surface.blit(rim, (0, 0))
        return surface

    return _cached(("bust", height, seed, dim), build)


def glow(size: int, color: tuple, steps: int = 5) -> pygame.Surface:
    """柔光（人物背后 / 和牌张背后的光晕）：同心圆逐层降 alpha，不占逐帧开销。"""

    def build() -> pygame.Surface:
        surface = pygame.Surface((size, size), pygame.SRCALPHA)
        base_alpha = color[3] if len(color) > 3 else 60
        for index in range(steps, 0, -1):
            radius = int(size / 2 * index / steps)
            alpha = int(base_alpha * (1.0 - index / (steps + 1.4)))
            if alpha <= 1:
                continue
            pygame.draw.circle(surface, (*color[:3], alpha), (size // 2, size // 2), radius)
        return surface

    return _cached(("glow", size, color, steps), build)


def portrait(
    size: int,
    seed: str,
    ring: tuple | None = None,
    dim: bool = False,
    angry: bool = False,
) -> pygame.Surface:
    """圆形人物立绘：半身像（发 / 脸 / 五官 / 肩 / 领），按昵称稳定随机。

    v2.5：牌桌上的四家不再是「首字头像」，而是**画出来的人物**（半身像），
    这样桌面才有「四个人在打牌」的画面感。全部几何绘制，无图片资源。
    """
    size = max(20, int(size))

    def build() -> pygame.Surface:
        skin = _SKIN_TONES[_seed_index(seed, 7, len(_SKIN_TONES))]
        hair = _HAIR_TONES[_seed_index(seed, 31, len(_HAIR_TONES))]
        robe = _ROBE_TONES[_seed_index(seed, 97, len(_ROBE_TONES))]
        style = _seed_index(seed, 173, 3)      # 0 短发 / 1 齐耳 / 2 长发
        if dim:
            skin = shade(skin, 0.68)
            hair = shade(hair, 0.7)
            robe = shade(robe, 0.6)

        surface = pygame.Surface((size, size), pygame.SRCALPHA)
        # 背景：偏暖的影棚光（上亮下暗），不做强饱和
        back = vertical_gradient(size, size, (86, 78, 70), (34, 30, 27))
        mask = pygame.Surface((size, size), pygame.SRCALPHA)
        pygame.draw.circle(mask, (255, 255, 255, 255), (size // 2, size // 2), size // 2)
        back.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(back, (0, 0))
        # 背景里的一点衣色环境光
        halo = pygame.Surface((size, size), pygame.SRCALPHA)
        pygame.draw.circle(halo, (*shade(robe, 1.2), 70), (size // 2, int(size * 0.72)), int(size * 0.46))
        pygame.draw.circle(mask, (255, 255, 255, 0), (0, 0), 1)
        surface.blit(halo, (0, 0))

        cx = size // 2
        head_w = int(size * 0.46)
        head_h = int(size * 0.52)
        head = pygame.Rect(cx - head_w // 2, int(size * 0.14), head_w, head_h)

        # 长发 / 齐耳：先铺发量（在头后面）
        if style == 2:
            for side in (-1, 1):
                lock = pygame.Rect(
                    cx + side * int(head.w * 0.40) - int(size * 0.075),
                    head.y + int(head.h * 0.20), int(size * 0.16), int(size * 0.62),
                )
                pygame.draw.ellipse(surface, hair, lock)
        elif style == 1:
            for side in (-1, 1):
                lock = pygame.Rect(
                    cx + side * int(head.w * 0.40) - int(size * 0.06),
                    head.y + int(head.h * 0.34), int(size * 0.13), int(size * 0.34),
                )
                pygame.draw.ellipse(surface, hair, lock)

        # 肩 / 身
        shoulder_w = int(size * 1.06)
        shoulder_h = int(size * 0.66)
        shoulder = pygame.Rect(cx - shoulder_w // 2, int(size * 0.70), shoulder_w, shoulder_h)
        pygame.draw.ellipse(surface, robe, shoulder)
        # 脖子（先画，压在肩下）
        neck_w = int(size * 0.18)
        pygame.draw.rect(
            surface, shade(skin, 0.78),
            (cx - neck_w // 2, int(size * 0.58), neck_w, int(size * 0.18)),
        )
        # 衣领：两侧浅色斜襟（留出中间的脖子）
        collar_color = shade(robe, 1.55)
        pygame.draw.polygon(
            surface, collar_color,
            [
                (cx - int(size * 0.20), shoulder.y + int(size * 0.02)),
                (cx - int(size * 0.04), shoulder.y + int(size * 0.24)),
                (cx - int(size * 0.02), shoulder.y + int(size * 0.16)),
                (cx - int(size * 0.12), shoulder.y - int(size * 0.02)),
            ],
        )
        pygame.draw.polygon(
            surface, collar_color,
            [
                (cx + int(size * 0.20), shoulder.y + int(size * 0.02)),
                (cx + int(size * 0.04), shoulder.y + int(size * 0.24)),
                (cx + int(size * 0.02), shoulder.y + int(size * 0.16)),
                (cx + int(size * 0.12), shoulder.y - int(size * 0.02)),
            ],
        )

        # 头
        pygame.draw.ellipse(surface, skin, head)
        # 头发：盖住上 42%，两侧留一点鬓角
        cap = pygame.Rect(head.x - int(size * 0.02), head.y - int(size * 0.03),
                          head.w + int(size * 0.04), int(head.h * 0.62))
        pygame.draw.ellipse(surface, hair, cap)
        # 额头：把发际线以下重新填肤色（形成一个自然的刘海弧）
        pygame.draw.ellipse(
            surface, skin,
            (head.x + int(head.w * 0.10), head.y + int(head.h * 0.30),
             int(head.w * 0.80), int(head.h * 0.62)),
        )
        # 鬓角
        for side in (-1, 1):
            pygame.draw.ellipse(
                surface, hair,
                (cx + side * int(head.w * 0.44) - int(size * 0.035),
                 head.y + int(head.h * 0.24), int(size * 0.07), int(head.h * 0.46)),
            )
        # 耳朵
        for side in (-1, 1):
            pygame.draw.circle(
                surface, shade(skin, 0.92),
                (cx + side * int(head.w * 0.50), head.y + int(head.h * 0.56)), max(1, int(size * 0.045)),
            )
        # 眼睛（眼白 + 瞳孔 + 高光）
        eye_y = head.y + int(head.h * 0.56)
        eye_dx = int(head.w * 0.20)
        eye_w = max(3, int(size * 0.11))
        eye_h = max(2, int(size * 0.055))
        for side in (-1, 1):
            pygame.draw.ellipse(
                surface, (250, 249, 246),
                (cx + side * eye_dx - eye_w // 2, eye_y - eye_h // 2, eye_w, eye_h),
            )
            pygame.draw.circle(
                surface, (52, 42, 38), (cx + side * eye_dx, eye_y), max(1, eye_h // 2),
            )
            if size >= 72:
                pygame.draw.circle(
                    surface, (255, 255, 255),
                    (cx + side * eye_dx + max(1, eye_h // 4), eye_y - max(1, eye_h // 4)),
                    max(1, eye_h // 5),
                )
        # 眉毛（当前行动的那家「皱眉」，其余平眉）
        brow_y = eye_y - int(size * 0.085)
        for side in (-1, 1):
            inner = cx + side * int(eye_dx * 0.55)
            outer = cx + side * int(eye_dx * 1.55)
            lift = -int(size * 0.012) if not angry else int(size * 0.02)
            pygame.draw.line(
                surface, shade(hair, 1.05), (outer, brow_y + lift), (inner, brow_y),
                max(1, size // 58),
            )
        # 鼻子 + 嘴
        pygame.draw.line(
            surface, shade(skin, 0.82),
            (cx, eye_y + int(size * 0.05)), (cx, eye_y + int(size * 0.10)), max(1, size // 80),
        )
        mouth_y = head.y + int(head.h * 0.86)
        pygame.draw.arc(
            surface, (168, 88, 82),
            (cx - int(size * 0.055), mouth_y - int(size * 0.045), int(size * 0.11), int(size * 0.075)),
            math.radians(200), math.radians(340), max(1, size // 66),
        )
        # 外框（深色描边 + 可选状态环）
        pygame.draw.circle(surface, (28, 24, 20), (size // 2, size // 2), size // 2, width=max(1, size // 60))
        if ring is not None:
            pygame.draw.circle(surface, ring, (size // 2, size // 2), size // 2 - 1, width=max(2, size // 26))
        return surface

    return _cached(("portrait", size, seed, ring, dim, angry), build)


def strip(
    width: int,
    height: int,
    fill: tuple,
    edge: tuple | None = None,
    radius: int | None = None,
) -> pygame.Surface:
    """条状底板（HUD / 座位牌 / 托盘）：横向渐变 + 顶亮底暗。"""
    width = max(2, int(width))
    height = max(2, int(height))
    radius = height // 2 if radius is None else radius

    def build() -> pygame.Surface:
        surface = pygame.Surface((width, height), pygame.SRCALPHA)
        base = fill[:3]
        alpha = fill[3] if len(fill) > 3 else 255
        tone = vertical_gradient(width, height, shade(base, 1.35), shade(base, 0.85), alpha, alpha)
        mask = pygame.Surface((width, height), pygame.SRCALPHA)
        pygame.draw.rect(mask, (255, 255, 255, 255), (0, 0, width, height), border_radius=radius)
        tone.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(tone, (0, 0))
        pygame.draw.rect(surface, (255, 255, 255, 26), (0, 0, width, height), width=1, border_radius=radius)
        if edge is not None:
            pygame.draw.rect(surface, edge, (0, 0, width, height), width=1, border_radius=radius)
        return surface

    return _cached(("strip", width, height, fill, edge, radius), build)


# ---------------------------------------------------------------- 按钮


_BUTTON_TONES = {
    "normal": (T.BTN, T.BTN_HOVER, T.BTN_DOWN, T.PANEL_EDGE),
    "win": (T.BTN_WIN, T.BTN_WIN_HOVER, T.BTN_WIN, (248, 214, 128)),
    "disabled": (T.BTN_DISABLED, T.BTN_DISABLED, T.BTN_DISABLED, (104, 112, 108)),
}


def button_skin(
    width: int,
    height: int,
    tone: str,
    state: str,
    radius: int = 9,
) -> pygame.Surface:
    """按钮皮肤（含投影）。`state ∈ {idle, hover, pressed}`。"""
    width = max(2, int(width))
    height = max(2, int(height))
    base, hover, pressed, edge = _BUTTON_TONES.get(tone, _BUTTON_TONES["normal"])

    def build() -> pygame.Surface:
        surface = pygame.Surface((width, height), pygame.SRCALPHA)
        body_h = max(2, height - 2)
        if state != "pressed":
            drop_shadow(surface, (1, 1, width - 2, body_h - 1), radius=radius, offset=(0, 1), alpha=100, spread=1)
        fill = {"hover": hover, "pressed": pressed}.get(state, base)
        # 上亮下暗的竖向渐变：按下时反过来（内凹感）
        if state == "pressed":
            top, bottom = shade(fill, 0.86), shade(fill, 1.12)
        else:
            top, bottom = shade(fill, 1.26), shade(fill, 0.8)
        tone_surface = vertical_gradient(width, body_h, top, bottom)
        mask = pygame.Surface((width, body_h), pygame.SRCALPHA)
        pygame.draw.rect(mask, (255, 255, 255, 255), (0, 0, width, body_h), border_radius=radius)
        tone_surface.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(tone_surface, (0, 0))
        # 顶部高光带 + 描边
        gloss = pygame.Surface((width, max(2, body_h // 2)), pygame.SRCALPHA)
        gloss.fill((255, 255, 255, 0))
        pygame.draw.rect(
            gloss, (255, 255, 255, 34),
            (1, 1, width - 2, max(2, body_h // 2)), border_top_left_radius=radius, border_top_right_radius=radius,
        )
        surface.blit(gloss, (0, 0))
        pygame.draw.rect(surface, edge, (0, 0, width, body_h), width=1, border_radius=radius)
        return surface

    return _cached(("btn", width, height, tone, state, radius), build)


# ---------------------------------------------------------------- 头像


_AVATAR_TINTS = (
    (206, 92, 68),
    (66, 128, 208),
    (74, 166, 116),
    (190, 134, 62),
    (146, 108, 198),
    (72, 164, 176),
    (198, 108, 146),
    (124, 138, 196),
)


def tint_for(seed: str) -> tuple[int, int, int]:
    value = 0
    for char in seed:
        value = (value * 131 + ord(char)) & 0xFFFFFFFF
    return _AVATAR_TINTS[value % len(_AVATAR_TINTS)]


def avatar(
    size: int,
    seed: str,
    initial: str,
    fonts,
    ring: tuple | None = None,
    dim: bool = False,
) -> pygame.Surface:
    """圆形头像：径向渐变底 + 单字 + 可选外环（庄家 / 当前行动 / 断线）。"""
    size = max(8, int(size))

    def build() -> pygame.Surface:
        surface = pygame.Surface((size, size), pygame.SRCALPHA)
        tint = tint_for(seed)
        if dim:
            tint = shade(tint, 0.55)
        # 上亮下暗的圆底
        gradient = vertical_gradient(size, size, shade(tint, 1.35), shade(tint, 0.7))
        mask = pygame.Surface((size, size), pygame.SRCALPHA)
        pygame.draw.circle(mask, (255, 255, 255, 255), (size // 2, size // 2), size // 2)
        gradient.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(gradient, (0, 0))
        # 左上角高光
        pygame.draw.circle(
            surface, (255, 255, 255, 40),
            (int(size * 0.36), int(size * 0.32)), max(2, size // 5),
        )
        font = fonts.get(max(10, int(size * 0.52)), bold=True)
        image = font.render(initial or "?", True, (250, 248, 244))
        surface.blit(image, ((size - image.get_width()) // 2, (size - image.get_height()) // 2))
        pygame.draw.circle(surface, shade(tint, 0.5), (size // 2, size // 2), size // 2, width=1)
        if ring is not None:
            pygame.draw.circle(surface, ring, (size // 2, size // 2), size // 2 - 1, width=2)
        return surface

    return _cached(("avatar", size, seed, initial, ring, dim), build)


# ---------------------------------------------------------------- 进度条


def meter(
    width: int,
    height: int,
    ratio: float,
    color: tuple,
    *,
    track: tuple = (0, 0, 0, 120),
) -> pygame.Surface:
    """倒计时条：圆头 + 头部高光；`ratio ∈ [0,1]`。"""
    width = max(2, int(width))
    height = max(2, int(height))
    surface = pygame.Surface((width, height), pygame.SRCALPHA)
    radius = max(1, height // 2)
    pygame.draw.rect(surface, track, (0, 0, width, height), border_radius=radius)
    filled = int(round(width * max(0.0, min(1.0, ratio))))
    if filled >= 1:
        body = vertical_gradient(filled, height, shade(color, 1.3), shade(color, 0.85))
        mask = pygame.Surface((filled, height), pygame.SRCALPHA)
        pygame.draw.rect(mask, (255, 255, 255, 255), (0, 0, filled, height), border_radius=radius)
        body.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(body, (0, 0))
        gloss = pygame.Surface((filled, max(2, height // 2)), pygame.SRCALPHA)
        pygame.draw.rect(
            gloss, (255, 255, 255, 46), (0, 0, filled, max(2, height // 2)),
            border_radius=radius,
        )
        surface.blit(gloss, (0, 0))
    return surface
