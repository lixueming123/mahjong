"""大厅界面（`SCREEN_LOBBY`，UI_UX_SPEC §12）。

登录成功后停在这里。`新建房间` 和点击房间行只负责入座；`开始对局` 才发 `ready`。
"""

from __future__ import annotations

import pygame

from .. import theme as T
from ..view import UiView
from . import art
from . import stats as stats_view
from . import widgets as W


def _render_rooms(surface: pygame.Surface, ui: UiView) -> None:
    """房间列表：最多 4 行。本连接所在的房间用 SELECT 描边。"""
    fonts = ui.fonts
    font = fonts.get(T.FS_SMALL)
    W.draw_text(surface, "房间（点击加入）", font, T.TEXT_DIM, T.LB_ROOMS_LABEL, W.ALIGN_LEFT)
    rooms = list(ui.open_rooms)[:T.LB_ROOM_ROWS]
    if not rooms:
        W.draw_text(
            surface, "还没有可加入的房间", font, T.TEXT_DIM,
            T.lobby_room_row_rect(0), W.ALIGN_LEFT,
        )
        return
    current = ui.state.room_id
    for index, room in enumerate(rooms):
        rect = T.lobby_room_row_rect(index)
        if room.get("room_id") == current:
            surface.blit(
                art.strip(rect[2], rect[3], (232, 192, 104, 40), edge=T.SELECT, radius=6),
                (rect[0], rect[1]),
            )
        host = room.get("host") or ""
        players = room.get("players", 0)
        seats = room.get("seats", 4)
        W.draw_text(surface, f"{room.get('room_id', '')}  {host}", font, T.TEXT, rect, W.ALIGN_LEFT)
        W.draw_text(surface, f"{players}/{seats}", font, T.TEXT, rect, W.ALIGN_RIGHT)


def _identity_line(ui: UiView) -> str:
    """身份行：账号会话显示账号名，游客会话标明「游客」。"""
    name = ui.nickname or "—"
    return f"{name} · 账号" if ui.has_account_session else f"{name} · 游客"


def render(surface: pygame.Surface, ui: UiView) -> None:
    fonts = ui.fonts

    # 背景：与登录页同一套渐变 + 水印牌，切屏时观感连续
    surface.blit(art.backdrop(T.LOGICAL_W, T.LOGICAL_H, (24, 78, 58), (10, 34, 25)), (0, 0))
    surface.blit(art.fade_tile(fonts, "4z", 150, 204, 18, -12.0), (92, 430))
    surface.blit(art.fade_tile(fonts, "7z", 150, 204, 16, 10.0), (1038, 104))

    W.draw_panel(surface, T.LB_CARD, fill=T.PANEL, edge=T.PANEL_EDGE, radius=12, shadow=True)
    line = pygame.Surface((T.LB_CARD[2] - 80, 2), pygame.SRCALPHA)
    line.fill((232, 192, 104, 96))
    surface.blit(line, (T.LB_CARD[0] + 40, T.LB_CARD[1] + 78))
    W.draw_text(surface, "麻将 · ex_actor", fonts.get(T.FS_H1, bold=True), T.TEXT, T.LB_TITLE, W.ALIGN_CENTER)
    W.draw_text(surface, _identity_line(ui), fonts.get(T.FS_BODY), T.TEXT_DIM, T.LB_PLAYER, W.ALIGN_CENTER)

    # 战绩（与牌桌弹窗、结算页同一版式；游客会话写明没有可查的历史）
    if ui.has_account_session and ui.stats is not None:
        W.draw_text(
            surface, stats_view.summary_text(ui.stats), fonts.get(T.FS_SMALL), T.TEXT_DIM, T.LB_SUMMARY,
            W.ALIGN_CENTER,
        )
        entries = stats_view.recent_entries(ui.stats)[:T.LB_RECENT_ROWS]
        if entries:
            stats_view.render_list(
                surface, ui, entries, x=T.LB_ROWS_X, y=T.LB_ROWS_Y, width=T.LB_ROWS_W, row_h=T.LB_ROW_H,
                with_time=True,
            )
        else:
            W.draw_text(
                surface, stats_view.EMPTY_TEXT, fonts.get(T.FS_SMALL), T.TEXT_DIM,
                (T.LB_ROWS_X, T.LB_ROWS_Y, T.LB_ROWS_W, T.LB_ROW_H), W.ALIGN_CENTER,
            )
    elif ui.has_account_session:
        W.draw_text(
            surface, "正在读取战绩…", fonts.get(T.FS_SMALL), T.TEXT_DIM, T.LB_SUMMARY, W.ALIGN_CENTER
        )
    else:
        W.draw_text(
            surface, "游客会话：不记录战绩。用账号登录即可保存每局结果。", fonts.get(T.FS_SMALL), T.TEXT_DIM,
            (T.LB_ROWS_X, T.LB_ROWS_Y, T.LB_ROWS_W, T.LB_ROW_H), W.ALIGN_CENTER,
        )

    _render_rooms(surface, ui)

    for rect, label, enabled in (
        (T.LB_NEW_BTN, "新建房间", ui.lobby_create_enabled),
        (T.LB_START_BTN, "开始对局", ui.lobby_begin_enabled),
        (T.LB_BACK_BTN, "返回登录", True),
    ):
        hovered = T.rect_contains(rect, ui.mouse)
        W.draw_button(
            surface, fonts, rect, label, enabled=enabled, hovered=hovered and enabled,
            pressed=hovered and ui.mouse_down and enabled,
        )

    if ui.login_status:
        W.draw_text(
            surface, ui.login_status, fonts.get(T.FS_BODY), ui.login_status_color, T.LB_STATUS, W.ALIGN_LEFT
        )
    W.draw_text(
        surface, f"目标服务器 {ui.host}:{ui.port}", fonts.get(T.FS_SMALL), T.TEXT_DIM, T.LB_SERVER, W.ALIGN_LEFT
    )

    quit_rect = T.LB_QUIT
    W.draw_button(
        surface, fonts, quit_rect, "退出", enabled=True,
        hovered=T.rect_contains(quit_rect, ui.mouse),
        pressed=T.rect_contains(quit_rect, ui.mouse) and ui.mouse_down,
    )
