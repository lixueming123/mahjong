"""听牌条（FEEL_SPEC v1.7 §2.2 / §2.3，常量 F-TP-01..F-TP-09）。

数据来源：**服务端 `tenpai` 帧**（PROTOCOL v1.7 §5.16），客户端不计算、不排序、不补缺省。
`waits[].remaining` 的语义是「**还没露面的最多还有 N 张**」（他家暗杠不可见 ⇒ 不扣减），
**不是**「还能摸到 N 张」——因此允许「显示 2 枚、实际可摸 0 张」。

本模块是纯渲染：只读 `state.tenpai_waits`，不改任何对局状态（§7 R1）。
"""

from __future__ import annotations

import logging
import time

import pygame

from .. import theme as T
from ..tiles import tile_label
from ..view import UiView
from . import art
from . import widgets as W

log = logging.getLogger("mahjong.client.tenpai")

# F-TP-04：项宽固定估算 74 px；`n × 74 > 784`（即 n > 10）⇒ 降级为一行摘要
_MAX_ITEMS = (T.R_TENPAI[2] - 16) // T.TENPAI_ITEM_WIDTH_EST

# 淡出动画用的**视觉快照**（纯绘制缓存，不进 game_state）
_fade_snapshot: list[dict] = []


def remaining_color(remaining: int) -> tuple[int, int, int]:
    """F-TP §2.3 枚数着色：`≥3` POS / `==2` TEXT / `==1` WARN / `==0` NEG / `>4` NEG。"""
    if remaining >= 3:
        return T.POS
    if remaining == 2:
        return T.TEXT
    if remaining == 1:
        return T.WARN
    return T.NEG


def render(surface: pygame.Surface, ui: UiView, feel) -> None:
    """画听牌条。`feel` 提供开关（`tenpai_bar_on`）与出现/消失/更新动画进度。

    v2.1：等待牌**画成真牌**（24×30 小牌）而不是牌名文字，和腾讯麻将的听牌条一致；
    「N 枚」仍按 F-TP §2.3 着色。整条先画到透明图层再统一按 `alpha` 贴回去，
    这样出现/消失动画对「牌面 + 文字」一起生效。
    """
    state = ui.state
    bar_on = getattr(feel, "tenpai_bar_on", True)
    anim = getattr(feel, "tenpai_anim", None)
    now = time.monotonic()

    items = list(state.tenpai_waits)
    if items:
        global _fade_snapshot
        _fade_snapshot = list(items)

    alpha = float(T.TENPAI_BG_ALPHA)
    dy = 0.0
    new_tiles: set[str] = set()
    if anim is not None:
        p = max(0.0, min(1.0, (now - anim.started) / anim.duration)) if anim.duration > 0 else 1.0
        if anim.kind == "appear":
            alpha = T.TENPAI_BG_ALPHA * p
            dy = -4.0 * (1.0 - p)      # F-TP-06：y −4 px 上浮
        elif anim.kind == "fade":
            alpha = T.TENPAI_BG_ALPHA * (1.0 - p)
            items = list(_fade_snapshot)
        if anim.kind == "update":
            new_tiles = set(anim.new_tiles)

    if not bar_on:
        return  # F-TP-10：关闭时不绘制（但仍消费 tenpai 帧——消费在 game_state 层）
    if not items and anim is None:
        return  # F-TP-05：未听牌时整条隐藏（不显示「未听牌」）
    if not items:
        return
    if alpha <= 1.0:
        return

    # F-TP-01 版式修正（见 theme.R_TENPAI_SHIFTED 注释）：本家有副露 ⇒ 上移 44 px，
    # 避免（240,514,800,40）与 R_SELF_MELD (452,518,376,36) 完全重叠而盖住副露组。
    rect = T.R_TENPAI_SHIFTED if state.melds else T.R_TENPAI
    x, y, w, h = rect
    y = int(y + dy)

    fonts = ui.fonts
    name_font = fonts.get(T.FS_SMALL)
    count_font = fonts.get(T.FS_MICRO)
    prefix = "听"

    # F-TP-04 溢出降级：多面听最多 9 面（4 面子 + 1 将），正常不触发；这里是防御
    overflow = len(items) > _MAX_ITEMS
    counts = [str(item.get("remaining", 0)) + " 枚" for item in items]
    pw = name_font.size(prefix)[0]
    item_w = [T.TENPAI_TILE_W + 3 + count_font.size(text)[0] for text in counts]
    total = pw + T.TENPAI_PREFIX_GAP + sum(item_w) + T.TENPAI_ITEM_GAP * max(0, len(items) - 1)

    # v2.1：底板收窄到「内容 + 内边距」并居中（原来是固定 800 px 的长条，
    # 两三个听牌面时会显得空荡）；外框矩形仍是 §4.3 冻结的 R_TENPAI。
    bar_w = w if overflow else min(w, total + 40)
    bar_x = x + (w - bar_w) // 2
    layer = pygame.Surface((bar_w, h), pygame.SRCALPHA)
    layer.blit(
        art.strip(bar_w, h, (*T.PANEL[:3], int(T.TENPAI_BG_ALPHA)), edge=T.PANEL_EDGE, radius=8),
        (0, 0),
    )

    if overflow:
        best = max((it.get("remaining", 0) for it in items), default=0)
        W.draw_text(
            layer, f"听 {len(items)} 面 · 最多 {best} 枚", name_font, T.TEXT,
            (16, 0, bar_w - 32, h), W.ALIGN_LEFT,
        )
        layer.set_alpha(int(alpha))
        surface.blit(layer, (bar_x, y))
        return

    cursor = max(12, (bar_w - total) // 2)

    W.draw_text(layer, prefix, name_font, T.TEXT_DIM, (cursor, 0, pw + 4, h), W.ALIGN_LEFT)
    cursor += pw + T.TENPAI_PREFIX_GAP

    for index, item in enumerate(items):
        remaining = item.get("remaining", 0)
        face = str(item.get("tile", "?"))
        color = remaining_color(remaining)
        width = item_w[index]
        # F-TP-07：新增项用 WARN 底色高亮并淡出（同整条一起衰减）
        if tile_label(face) in new_tiles or face in new_tiles:
            surface_hl = pygame.Surface((width + 6, h - 10), pygame.SRCALPHA)
            surface_hl.fill((*T.WARN[:3], 76))
            layer.blit(surface_hl, (cursor - 3, 5))
        W.draw_tile(
            layer, fonts,
            (cursor, (h - T.TENPAI_TILE_H) // 2, T.TENPAI_TILE_W, T.TENPAI_TILE_H),
            face,
            radius=4,
            rank_size=T.FS_TILE_RIVER_RANK,
            suit_size=T.FS_TILE_RIVER_SUIT,
            honor_size=T.FS_TILE_Z_RIVER_CENTER,
        )
        count_rect = (cursor + T.TENPAI_TILE_W + 3, 0, width - T.TENPAI_TILE_W - 3, h)
        W.draw_text(
            layer, counts[index], count_font, color, count_rect, W.ALIGN_LEFT,
        )
        # §2.3：`remaining == 0` ⇒ 牌名加删除线（理论不该出现，属服务端异常，照画不崩）
        if remaining == 0:
            pygame.draw.line(
                layer, T.NEG,
                (count_rect[0], h // 2), (count_rect[0] + count_font.size(counts[index])[0], h // 2),
                width=1,
            )
        cursor += width + T.TENPAI_ITEM_GAP

    layer.set_alpha(int(alpha))
    surface.blit(layer, (bar_x, y))
