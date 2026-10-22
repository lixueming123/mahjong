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


def _item_texts(item: dict) -> tuple[str, str]:
    """`(牌名, 枚数后缀)`；`remaining == 1` 追加 `·`（§2.3）。"""
    tile = str(item.get("tile", "?"))
    remaining = item.get("remaining", 0)
    suffix = f"{remaining} 枚"
    if remaining == 1:
        suffix += "·"
    return tile_label(tile), suffix


def render(surface: pygame.Surface, ui: UiView, feel) -> None:
    """画听牌条。`feel` 提供开关（`tenpai_bar_on`）与出现/消失/更新动画进度。"""
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
    _draw_bar_bg(surface, (x, y, w, h), int(alpha))

    fonts = ui.fonts
    name_font = fonts.get(T.FS_SMALL)
    count_font = fonts.get(T.FS_MICRO)
    base_y = y + h // 2

    # F-TP-04 溢出降级：多面听最多 9 面（4 面子 + 1 将），正常不触发；这里是防御
    if len(items) > _MAX_ITEMS:
        best = max((it.get("remaining", 0) for it in items), default=0)
        text = f"听 {len(items)} 面 · 最多 {best} 枚"
        W.draw_text(
            surface, text, name_font, T.TEXT,
            (x + 16, y, w - 32, h), W.ALIGN_LEFT, alpha=int(alpha),
        )
        return

    cursor = T.TENPAI_ITEM_X0
    # F-TP-05：条首固定「听」字 + 8 px 间隔
    prefix = "听"
    pw = name_font.size(prefix)[0]
    W.draw_text(
        surface, prefix, name_font, T.TEXT_DIM,
        (cursor, y, pw + 4, h), W.ALIGN_LEFT, alpha=int(alpha),
    )
    cursor += pw + T.TENPAI_PREFIX_GAP

    for item in items:
        name_text, count_text = _item_texts(item)
        remaining = item.get("remaining", 0)
        color = remaining_color(remaining)
        nw = name_font.size(name_text)[0]
        cw = count_font.size(count_text)[0]
        # F-TP-07：新增项用 WARN 底色高亮并淡出（用整条 alpha 一起衰减，不再单开计时器）
        if name_text in new_tiles or str(item.get("tile")) in new_tiles:
            hl = pygame.Surface((nw + cw + 6, h - 12), pygame.SRCALPHA)
            hl.fill((*T.WARN[:3], int(90 * (alpha / T.TENPAI_BG_ALPHA))))
            surface.blit(hl, (cursor - 3, y + 6))
        # F-TP-03：牌名（15px TEXT）+ 空格 + 「N 枚」（12px 着色）
        W.draw_text(
            surface, name_text, name_font, T.TEXT if remaining != 0 else T.NEG,
            (cursor, y, nw + 4, h), W.ALIGN_LEFT, alpha=int(alpha),
        )
        name_x_end = cursor + nw + 4
        cursor += nw + 4
        W.draw_text(
            surface, count_text, count_font, color,
            (cursor, y, cw + 8, h), W.ALIGN_LEFT, alpha=int(alpha),
        )
        # §2.3：`remaining == 0` ⇒ 牌名加删除线（理论不该出现，属服务端异常，照画不崩）
        if remaining == 0:
            pygame.draw.line(
                surface, T.NEG,
                (cursor - nw - 4, base_y), (name_x_end - 4, base_y), width=1,
            )
        cursor += cw + 8 + T.TENPAI_ITEM_GAP
        if cursor > x + w - 40:
            break


def _draw_bar_bg(surface: pygame.Surface, rect: tuple[int, int, int, int], alpha: int) -> None:
    """F-TP-02：`PANEL` 底 alpha 216 + `PANEL_EDGE` 1 px 描边 + 圆角 4。"""
    x, y, w, h = rect
    layer = pygame.Surface((w, h), pygame.SRCALPHA)
    layer.fill((*T.PANEL[:3], min(255, alpha)))
    surface.blit(layer, (x, y))
    pygame.draw.rect(surface, T.PANEL_EDGE, (x, y, w, h), width=1, border_radius=4)
