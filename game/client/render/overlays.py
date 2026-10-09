"""叠加层（UI_UX_SPEC §2.1 / §8.1）：错误条、状态横幅、断线蒙层。"""

from __future__ import annotations

import pygame

from .. import theme as T
from ..view import UiView
from . import widgets as W

DISCONNECT_BTN = (560, 392, 160, 36)


def banner_rect(bar_visible: bool) -> tuple[int, int, int, int]:
    return T.R_BANNER_WITH_BAR if bar_visible else T.R_BANNER_NO_BAR


def banner_button_rect(bar_visible: bool) -> tuple[int, int, int, int] | None:
    rect = banner_rect(bar_visible)
    return (rect[0] + rect[2] - 140, rect[1] + 4, 132, rect[3] - 8)


def render(surface: pygame.Surface, ui: UiView, bar_visible: bool = False) -> None:
    _render_error_strip(surface, ui)
    _render_banner(surface, ui, bar_visible)
    _render_disconnect(surface, ui)


def _render_error_strip(surface: pygame.Surface, ui: UiView) -> None:
    if not ui.error_visible:
        return
    rect = T.R_ERROR_STRIP
    W.draw_panel(surface, rect, fill=T.PANEL, edge=T.NEG, radius=6, width=2)
    main = ui.error_message or ""
    W.draw_text(surface, main, ui.fonts.get(T.FS_BODY), T.TEXT, (rect[0] + 12, rect[1], rect[2] - 24, 26))
    if ui.error_code:
        W.draw_text(
            surface, ui.error_code, ui.fonts.get(T.FS_SMALL), T.TEXT_DIM,
            (rect[0] + 12, rect[1] + 22, rect[2] - 24, 20),
        )


def _render_banner(surface: pygame.Surface, ui: UiView, bar_visible: bool) -> None:
    if not ui.banner_text:
        return
    rect = banner_rect(bar_visible)
    W.draw_panel(surface, rect, fill=ui.banner_color, edge=T.PANEL_EDGE, radius=4, width=1)
    text_rect = (rect[0] + 12, rect[1], rect[2] - 24, rect[3])
    if ui.banner_action:
        text_rect = (rect[0] + 12, rect[1], rect[2] - 160, rect[3])
    W.draw_text(surface, ui.banner_text, ui.fonts.get(T.FS_BODY), (16, 16, 16), text_rect)
    if ui.banner_action:
        btn = banner_button_rect(bar_visible)
        hovered = btn is not None and T.rect_contains(btn, ui.mouse)
        W.draw_button(
            surface, ui.fonts, btn, ui.banner_action,
            enabled=True, hovered=hovered, pressed=hovered and ui.mouse_down,
        )


def _render_disconnect(surface: pygame.Surface, ui: UiView) -> None:
    if not ui.disconnect_reason:
        return
    mask = pygame.Surface((T.LOGICAL_W, T.LOGICAL_H), pygame.SRCALPHA)
    mask.fill(T.MASK)
    surface.blit(mask, (0, 0))
    rect = T.R_DISCONNECT_PANEL
    W.draw_panel(surface, rect, fill=(24, 32, 30, 245), edge=T.PANEL_EDGE, radius=8, width=2)
    W.draw_text(
        surface, "连接已断开", ui.fonts.get(T.FS_H2), T.TEXT,
        (rect[0] + 16, rect[1] + 12, rect[2] - 32, 34), W.ALIGN_CENTER,
    )
    W.draw_text(
        surface, ui.disconnect_reason, ui.fonts.get(T.FS_SMALL), T.TEXT_DIM,
        (rect[0] + 16, rect[1] + 48, rect[2] - 32, 40), W.ALIGN_CENTER,
    )
    W.draw_text(
        surface, "对局进行中，服务端将托管你的座位", ui.fonts.get(T.FS_SMALL), T.WARN,
        (rect[0] + 16, rect[1] + 86, rect[2] - 32, 20), W.ALIGN_CENTER,
    )
    hovered = T.rect_contains(DISCONNECT_BTN, ui.mouse)
    W.draw_button(
        surface, ui.fonts, DISCONNECT_BTN, "返回登录",
        enabled=True, hovered=hovered, pressed=hovered and ui.mouse_down,
    )
