"""战绩面板（UI_UX_SPEC §4.11 牌桌弹窗 / §9.6 结算页内嵌）。

数据全部来自一帧 `stats`（ACCOUNT_DESIGN.md §3.4）：四个汇总数字 + `recent[]`（最近 ≤10 局，
每局含房间号、结果、得失分、结束时间）。客户端不做本地累计——服务端才是权威，
所以这里只负责把最近一次收到的 `stats` 渲染出来。

两种形态共用同一份文案与配色：牌桌上是可开关的弹窗，结算页上是内嵌的一段（不新增面板）。
"""

from __future__ import annotations

import time

import pygame

from .. import theme as T
from ..view import UiView
from . import art
from . import widgets as W

import sys
from pathlib import Path

_game_root = next(
    p for p in Path(__file__).resolve().parents
    if (p / "protocol" / "enums.json").is_file()
)
sys.path.insert(0, str(_game_root))
from protocol.enums import REASON

# game_end.reason 的三个取值（PROTOCOL §5.10，reason 整数常量）→ 界面文案
REASON_TEXT = {
    REASON.WIN_RON: "荣和",
    REASON.WIN_TSUMO: "自摸",
    REASON.DRAW_EXHAUSTED: "荒庄",
}

EMPTY_TEXT = "暂无对局记录（本账号还没有打完一局）"

# `stats.recent` 的服务端上限（ACCOUNT_DESIGN.md §3.4）：牌桌面板一屏正好放下 10 行。
RECENT_LIMIT = 10


def summary_text(stats: dict) -> str:
    """汇总一行（两处共用同一公式，避免两套文案漂移）。"""
    # 日麻的 net_score 是「点数增减」，不是零和得失分 —— 标签跟着玩法走，别混着叫“得分”。
    suffix = "净得点" if stats.get("mode") == "riichi" else "净得分"
    return (
        f"累计战绩：{stats['games']} 局 · 和牌 {stats['wins']} · 荒庄 {stats['draws']} "
        f"· {suffix} {stats['net_score']:+d}"
    )


def score_color(delta: int) -> tuple:
    if delta > 0:
        return T.POS
    if delta < 0:
        return T.NEG
    return T.ZERO_SCORE


def _score_text(delta: int) -> str:
    return f"{delta:+d}" if delta != 0 else "±0"


def _ended_text(entry: dict) -> str:
    ended = entry.get("ended_at")
    if not isinstance(ended, int) or ended <= 0:
        return "—"
    return time.strftime("%m-%d %H:%M", time.localtime(ended))


def render_row(surface: pygame.Surface, ui: UiView, entry: dict, rect, with_time: bool) -> None:
    """一行：房间号 · 结果 · 得失分（· 时间）。牌桌弹窗、结算页、大厅三处共用。"""
    fonts = ui.fonts
    room = str(entry.get("room_id", "—"))
    reason = REASON_TEXT.get(entry.get("reason"), str(entry.get("reason", "—")))
    delta = entry.get("delta_score", 0)
    delta = delta if isinstance(delta, int) else 0

    W.draw_text(surface, f"{room}  {reason}", fonts.get(T.FS_SMALL), T.TEXT, rect, W.ALIGN_LEFT)
    tail = f"{_score_text(delta)}"
    if with_time:
        tail = f"{_ended_text(entry)}    {tail}"
    W.draw_text(surface, tail, fonts.get(T.FS_SMALL), score_color(delta), rect, W.ALIGN_RIGHT)


def recent_entries(stats: dict) -> list[dict]:
    recent = stats.get("recent")
    if not isinstance(recent, list):
        return []
    return [entry for entry in recent if isinstance(entry, dict)]


def render_list(surface: pygame.Surface, ui: UiView, entries: list[dict], *, x: int, y: int, width: int,
                row_h: int, with_time: bool) -> None:
    """竖排一列对局行（≤RECENT_LIMIT 行）：牌桌弹窗与大厅共用同一版式。"""
    for index, entry in enumerate(entries[:RECENT_LIMIT]):
        render_row(surface, ui, entry, (x, y + index * row_h, width, row_h), with_time)


def render_overlay(surface: pygame.Surface, ui: UiView) -> None:
    """牌桌弹窗（§4.11）：全屏蒙层 + 中央面板；ESC / 点击面板外关闭。"""
    mask = pygame.Surface((T.LOGICAL_W, T.LOGICAL_H), pygame.SRCALPHA)
    mask.fill(T.MASK)
    surface.blit(mask, (0, 0))
    panel = T.R_STATS_PANEL
    W.draw_panel(surface, panel, fill=(22, 32, 29, 248), edge=(120, 138, 126), radius=14, shadow=True)
    gold = pygame.Surface((panel[2] - 48, 2), pygame.SRCALPHA)
    gold.fill((232, 192, 104, 110))
    surface.blit(gold, (panel[0] + 24, panel[1] + 8))
    font_body = ui.fonts.get(T.FS_BODY)
    W.draw_text(surface, "战绩", ui.fonts.get(T.FS_H3, bold=True), T.TEXT, T.R_STATS_TITLE, W.ALIGN_LEFT)
    W.draw_text(surface, "ESC 关闭", ui.fonts.get(T.FS_SMALL), T.TEXT_DIM, T.R_STATS_TITLE, W.ALIGN_RIGHT)

    stats = ui.stats
    if stats is None:
        # 面板已开但 `stats` 还没到（请求在途）：给一行占位，不阻塞牌局。
        W.draw_text(surface, "正在读取战绩…", font_body, T.TEXT_DIM, T.R_STATS_SUMMARY, W.ALIGN_LEFT)
        return

    W.draw_text(surface, summary_text(stats), font_body, T.TEXT, T.R_STATS_SUMMARY, W.ALIGN_LEFT)
    W.draw_text(
        surface, "最近对局（新的在前）", ui.fonts.get(T.FS_SMALL), T.TEXT_DIM, T.R_STATS_HEADER, W.ALIGN_LEFT
    )
    rule = pygame.Surface((panel[2] - 48, 1), pygame.SRCALPHA)
    rule.fill((255, 255, 255, 24))
    surface.blit(rule, (panel[0] + 24, T.R_STATS_HEADER[1] + T.R_STATS_HEADER[3] + 2))

    entries = recent_entries(stats)
    if not entries:
        W.draw_text(surface, EMPTY_TEXT, ui.fonts.get(T.FS_SMALL), T.TEXT_DIM, T.R_STATS_ROWS_Y, W.ALIGN_LEFT)
        return
    _draw_zebra(surface, panel[0] + 16, T.R_STATS_ROWS_Y, panel[2] - 32, T.R_STATS_ROW_H, len(entries[:RECENT_LIMIT]))
    render_list(surface, ui, entries, x=panel[0] + 24, y=T.R_STATS_ROWS_Y, width=panel[2] - 48,
                row_h=T.R_STATS_ROW_H, with_time=True)


def _draw_zebra(surface: pygame.Surface, x: int, y: int, width: int, row_h: int, count: int) -> None:
    """隔行淡底纹：10 行数字排下来不容易串行。"""
    layer = pygame.Surface((width, row_h * max(0, count)), pygame.SRCALPHA)
    for index in range(count):
        if index % 2 == 1:
            pygame.draw.rect(layer, (255, 255, 255, 14), (0, index * row_h, width, row_h))
    surface.blit(layer, (x, y))


def render_result_block(surface: pygame.Surface, ui: UiView) -> None:
    """结算页内嵌（§9.6）：汇总一行 + 最近 ≤10 局，排成 2 列 × 5 行（不新增面板）。"""
    stats = ui.stats
    if stats is None:
        return  # 游客会话，或 stats 还没回来：结算页保持既有版面
    W.draw_text(
        surface,
        summary_text(stats),
        ui.fonts.get(T.FS_SMALL),
        T.TEXT_DIM,
        T.R_RESULT_STATS_SUMMARY,
        W.ALIGN_CENTER,
    )
    entries = recent_entries(stats)
    if not entries:
        W.draw_text(
            surface, EMPTY_TEXT, ui.fonts.get(T.FS_SMALL), T.TEXT_DIM,
            (T.R_RESULT_STATS_COL_X[0], T.R_RESULT_STATS_ROWS_Y, T.R_RESULT_STATS_COL_W, T.R_RESULT_STATS_ROW_H),
            W.ALIGN_LEFT,
        )
        return
    # 倒序先填满左列（5 条）再填右列，正好对应服务端的 ≤10 条上限。
    for index, entry in enumerate(entries[:RECENT_LIMIT]):
        column, offset = divmod(index, 5)
        if column >= len(T.R_RESULT_STATS_COL_X):
            break
        row = (
            T.R_RESULT_STATS_COL_X[column],
            T.R_RESULT_STATS_ROWS_Y + offset * T.R_RESULT_STATS_ROW_H,
            T.R_RESULT_STATS_COL_W,
            T.R_RESULT_STATS_ROW_H,
        )
        render_row(surface, ui, entry, row, with_time=False)
