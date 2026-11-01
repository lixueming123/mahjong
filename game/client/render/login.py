"""登录界面（UI_UX_SPEC §3；账号模式为 ACCOUNT_DESIGN §5 的增补）。

两种模式共用一块卡片：游客模式只有昵称框，账号模式多出密码框与「注册」按钮。
密码**只以 ● 掩码呈现**——明文既不进 UiView、也不进日志。
"""

from __future__ import annotations

import pygame

from .. import theme as T
from ..view import UiView
from . import art
from . import widgets as W


def _draw_watermarks(surface: pygame.Surface, fonts) -> None:
    """背景水印：两张淡到几乎看不见的大牌（東 / 發），给纯色底一点纵深。"""
    surface.blit(art.fade_tile(fonts, "1z", 150, 204, 20, 12.0), (86, 402))
    surface.blit(art.fade_tile(fonts, "6z", 150, 204, 18, -10.0), (1044, 96))
    surface.blit(art.fade_tile(fonts, "9p", 120, 164, 14, -6.0), (170, 96))


def _draw_input(
    surface: pygame.Surface,
    fonts,
    rect: tuple[int, int, int, int],
    text: str,
    focused: bool,
    error: bool,
    caret_on: bool,
    counter: str = "",
    composition: str = "",
) -> None:
    """一个输入框：聚焦时描边高亮 + 外发光，error 时红框；caret 只在聚焦时闪。"""
    edge = T.NEG if error else (T.SELECT if focused else T.PANEL_EDGE)
    surface.blit(art.strip(rect[2], rect[3], (8, 24, 18, 232), edge=None, radius=8), (rect[0], rect[1]))
    if focused and not error:
        glow = pygame.Surface((rect[2] + 8, rect[3] + 8), pygame.SRCALPHA)
        pygame.draw.rect(glow, (255, 200, 64, 46), (0, 0, rect[2] + 8, rect[3] + 8), width=4, border_radius=12)
        surface.blit(glow, (rect[0] - 4, rect[1] - 4))
    pygame.draw.rect(surface, edge, rect, width=2, border_radius=8)

    font = fonts.get(32)
    text_pos = (rect[0] + 12, rect[1] + rect[3] // 2)
    # 超长时从左侧截断，保证光标与尾部始终可见
    shown = text
    limit = rect[2] - 90
    while shown and font.size(shown)[0] > limit:
        shown = shown[1:]
    image = font.render(shown, True, T.TEXT)
    surface.blit(image, (text_pos[0], text_pos[1] - image.get_height() // 2))
    drawn_width = font.size(shown)[0]
    if focused and composition:
        comp = font.render(composition, True, T.TEXT_DIM)
        surface.blit(comp, (text_pos[0] + drawn_width, text_pos[1] - comp.get_height() // 2))
        drawn_width += comp.get_width()
    if focused and caret_on:
        caret_x = text_pos[0] + drawn_width + 2
        pygame.draw.rect(surface, T.TEXT, (caret_x, text_pos[1] - 16, 2, 32))
    if counter:
        W.draw_text(
            surface,
            counter,
            fonts.get(T.FS_SMALL),
            T.TEXT_DIM,
            (rect[0], rect[1], rect[2] - 10, rect[3]),
            W.ALIGN_RIGHT,
        )


def _draw_mode_tab(surface: pygame.Surface, fonts, rect, label: str, active: bool, ui: UiView) -> None:
    hovered = T.rect_contains(rect, ui.mouse)
    if active:
        surface.blit(art.strip(rect[2], rect[3], (232, 192, 104, 255), edge=None, radius=8), (rect[0], rect[1]))
    else:
        surface.blit(art.strip(rect[2], rect[3], (8, 24, 18, 210), edge=None, radius=8), (rect[0], rect[1]))
        pygame.draw.rect(surface, T.ACTIVE if hovered else T.PANEL_EDGE, rect, width=1, border_radius=8)
    W.draw_text(
        surface, label, fonts.get(T.FS_BODY), T.BG_LOGIN if active else T.TEXT_DIM, rect, W.ALIGN_CENTER
    )


def _draw_checkbox(
    surface: pygame.Surface, fonts, rect: tuple[int, int, int, int], label: str, checked: bool, hovered: bool
) -> None:
    """「记住密码」勾选框：16px 方框 + 勾，标签 15px；勾上后标签转亮，未勾时只在悬停提亮描边。"""
    box = pygame.Rect(rect[0], rect[1] + (rect[3] - 16) // 2, 16, 16)
    if checked:
        pygame.draw.rect(surface, T.ACTIVE, box, border_radius=4)
        pygame.draw.lines(
            surface,
            T.BG_LOGIN,
            False,
            [(box.x + 3, box.y + 8), (box.x + 7, box.y + 12), (box.x + 13, box.y + 4)],
            2,
        )
    else:
        pygame.draw.rect(surface, T.ACTIVE if hovered else T.PANEL_EDGE, box, width=2, border_radius=4)
    W.draw_text(
        surface,
        label,
        fonts.get(T.FS_SMALL),
        T.TEXT if checked else T.TEXT_DIM,
        (rect[0] + 24, rect[1], rect[2] - 24, rect[3]),
        W.ALIGN_LEFT,
    )


def render(surface: pygame.Surface, ui: UiView) -> None:
    fonts = ui.fonts
    account_mode = ui.login_mode == "account"

    # 背景：竖向渐变 + 中心柔光 + 水印牌（不再是纯色平涂）
    surface.blit(
        art.backdrop(T.LOGICAL_W, T.LOGICAL_H, (24, 78, 58), (10, 34, 25)), (0, 0)
    )
    _draw_watermarks(surface, fonts)

    # 卡片：先投影再玻璃面板，圆角/坐标仍用 §3.2 冻结值
    W.draw_panel(surface, T.L_CARD, fill=T.PANEL, edge=T.PANEL_EDGE, radius=12, shadow=True)
    # 标题下的短金线（装饰，不参与任何布局；长度只覆盖标题，不压副标题）
    line = pygame.Surface((200, 2), pygame.SRCALPHA)
    line.fill((232, 192, 104, 110))
    surface.blit(line, (640 - 100, T.L_CARD[1] + 74))

    if not account_mode:
        # 游客模式的空白区补三行「这局能玩到什么」（真实功能，不是装饰文案）
        for index, text in enumerate((
            "· 四人一桌：1 真人 + 3 AI，吃 / 碰 / 杠 / 胡全支持",
            "· 番种、封顶与结算播报：和牌后逐条念给你听",
            "· 听牌提示实时显示可胡的牌与剩余枚数",
        )):
            W.draw_text(
                surface, text, fonts.get(T.FS_SMALL), T.TEXT_DIM,
                (T.L_CARD[0] + 40, 394 + index * 24, T.L_CARD[2] - 80, 20), W.ALIGN_LEFT,
            )

    W.draw_text(surface, "麻将 · ex_actor", fonts.get(T.FS_H1, bold=True), T.TEXT, T.L_TITLE, W.ALIGN_CENTER)
    W.draw_text(
        surface, "单房间单局 · 1 真人 + 3 AI", fonts.get(T.FS_H3), T.TEXT_DIM, T.L_SUBTITLE, W.ALIGN_CENTER
    )

    _draw_mode_tab(surface, fonts, T.L_MODE_GUEST, "游客", not account_mode, ui)
    _draw_mode_tab(surface, fonts, T.L_MODE_ACCOUNT, "账号", account_mode, ui)

    W.draw_text(
        surface,
        "账号名" if account_mode else "昵称",
        fonts.get(T.FS_BODY),
        T.TEXT_DIM,
        T.L_LABEL,
        W.ALIGN_LEFT,
    )
    _draw_input(
        surface,
        fonts,
        T.L_INPUT,
        ui.login_account if account_mode else ui.nickname,
        focused=ui.login_focus == "name",
        error=ui.login_input_error,
        caret_on=ui.caret_on,
        counter="" if account_mode else f"{len(ui.nickname)}/16",
        composition=ui.ime_composition if ui.login_focus == "name" else "",
    )

    if account_mode:
        W.draw_text(surface, "密码", fonts.get(T.FS_BODY), T.TEXT_DIM, T.L_LABEL2, W.ALIGN_LEFT)
        # 掩码由 UiView 提供（只有 ● 和长度），明文留在 app 里
        _draw_input(
            surface,
            fonts,
            T.L_INPUT2,
            ui.login_password_masked,
            focused=ui.login_focus == "password",
            error=ui.login_password_error,
            caret_on=ui.caret_on,
            composition=ui.ime_composition if ui.login_focus == "password" else "",
        )
        # 勾上 = 登录成功后把账号口令存到本机（0600），下次启动直接填回（§3.5）
        _draw_checkbox(
            surface,
            fonts,
            T.L_REMEMBER,
            "记住密码",
            ui.remember_password,
            T.rect_contains(T.L_REMEMBER, ui.mouse),
        )

    main_rect = T.L_MAIN_BTN
    hovered = T.rect_contains(main_rect, ui.mouse)
    W.draw_button(
        surface,
        fonts,
        main_rect,
        ui.login_submit_label,
        enabled=ui.login_submit_enabled,
        hovered=hovered and ui.login_submit_enabled,
        pressed=hovered and ui.mouse_down and ui.login_submit_enabled,
    )

    if account_mode:
        register_rect = T.L_REGISTER_BTN
        hovered = T.rect_contains(register_rect, ui.mouse)
        W.draw_button(
            surface,
            fonts,
            register_rect,
            ui.login_register_label,
            enabled=ui.login_register_enabled,
            hovered=hovered and ui.login_register_enabled,
            pressed=hovered and ui.mouse_down and ui.login_register_enabled,
        )

    if ui.login_status:
        W.draw_text(
            surface, ui.login_status, fonts.get(T.FS_BODY), ui.login_status_color, T.L_STATUS, W.ALIGN_LEFT
        )
    if ui.login_retry_visible:
        retry = T.L_RETRY
        W.draw_button(
            surface,
            fonts,
            retry,
            "重试",
            enabled=True,
            hovered=T.rect_contains(retry, ui.mouse),
            pressed=T.rect_contains(retry, ui.mouse) and ui.mouse_down,
        )

    W.draw_text(
        surface,
        f"目标服务器 {ui.host}:{ui.port}",
        fonts.get(T.FS_SMALL),
        T.TEXT_DIM,
        T.L_SERVER,
        W.ALIGN_LEFT,
    )

    quit_rect = T.L_QUIT
    W.draw_button(
        surface,
        fonts,
        quit_rect,
        "退出",
        enabled=True,
        hovered=T.rect_contains(quit_rect, ui.mouse),
        pressed=T.rect_contains(quit_rect, ui.mouse) and ui.mouse_down,
    )
