"""结算界面（UI_UX_SPEC §9）。

调用约定：`app` 先绘制牌桌快照，再调用本模块（蒙层 + 中央面板）。
"""

from __future__ import annotations

import logging

import pygame

from .. import theme as T
from ..game_state import GameState
from ..tiles import multiset_remove
from ..view import UiView
from . import stats as stats_view
from . import widgets as W

log = logging.getLogger("mahjong.client.result")

REASON_LABEL = {
    "win_tsumo": "自摸",
    "win_ron": "荣和",
    "draw_exhausted": "荒庄（牌山摸尽）",
}

TAG_LABEL = {"seven_pairs": "七对子", "pure_one_suit": "清一色"}


def _title(state: GameState) -> str:
    result = state.result or {}
    reason = result.get("reason")
    return REASON_LABEL.get(reason, str(reason or "结算"))


def _tags_text(win_diff: dict | None) -> str:
    if not isinstance(win_diff, dict):
        return "本局不计分"
    tags = [TAG_LABEL.get(t, str(t)) for t in (win_diff.get("tags") or [])]
    return " · ".join(tags) if tags else "平和"


def _results(state: GameState) -> list[dict]:
    result = state.result or {}
    raw = result.get("results")
    if not isinstance(raw, list):
        return []
    out = [r for r in raw if isinstance(r, dict) and isinstance(r.get("seat"), int)]
    out.sort(key=lambda r: r["seat"])
    return out


def group_winner_hand(state: GameState, win_seat: int) -> tuple[list, list[str]]:
    """§9.3：`results[win.seat].hand` 是扁平数组，与 `melds[]` 并存。

    返回 `(副露组列表, 门前手牌列表)`；无法匹配时退化为「不分组」并记 WARN。
    """
    result = state.result or {}
    hand = []
    for entry in _results(state):
        if entry.get("seat") == win_seat:
            raw = entry.get("hand")
            if isinstance(raw, list):
                hand = [t for t in raw if isinstance(t, str)]
            break
    melds = list(state.seat_view(win_seat).melds)
    pool = list(hand)
    groups = []
    for meld in melds:
        if multiset_remove(pool, meld.tiles):
            groups.append(meld)
        else:
            log.warning("hand 与 melds 无法匹配（数据异常），退化为不分组渲染")
            return [], hand
    return groups, pool


def render(surface: pygame.Surface, ui: UiView) -> None:
    state, fonts = ui.state, ui.fonts
    result = state.result or {}
    win = result.get("win") if isinstance(result.get("win"), dict) else None

    mask = pygame.Surface((T.LOGICAL_W, T.LOGICAL_H), pygame.SRCALPHA)
    mask.fill(T.MASK)
    surface.blit(mask, (0, 0))

    W.draw_panel(surface, T.R_RESULT_PANEL, fill=(24, 32, 30, 245), edge=T.PANEL_EDGE, radius=8, width=2)

    # 标题行
    W.draw_text(surface, _title(state), fonts.get(T.FS_H1, bold=True), T.TEXT, T.R_RESULT_TITLE)
    if win is not None:
        delta = None
        for entry in _results(state):
            if entry.get("seat") == win.get("seat"):
                delta = entry.get("delta_score")
                break
        if isinstance(delta, int):
            color = T.POS if delta > 0 else (T.NEG if delta < 0 else T.ZERO_SCORE)
            W.draw_text(
                surface, f"{delta:+d}", fonts.get(T.FS_H1, bold=True), color, T.R_RESULT_TITLE, W.ALIGN_RIGHT
            )

    # 牌型行
    W.draw_text(surface, _tags_text(win), fonts.get(T.FS_H3), T.TEXT_DIM, T.R_RESULT_TAGS)

    # 四座位结果卡片
    from_payer = win.get("from_seat") if win is not None else None
    for entry in _results(state):
        seat = entry["seat"]
        rel = state.rel_index(seat)
        if not 0 <= rel < len(T.R_RESULT_CARDS_X):
            continue
        rect = (T.R_RESULT_CARDS_X[rel], T.R_RESULT_CARDS_Y, T.R_RESULT_CARD_W, T.R_RESULT_CARDS_H)
        is_winner = win is not None and win.get("seat") == seat
        fill = (40, 56, 48, 245) if is_winner else T.PANEL
        W.draw_panel(surface, rect, fill=fill, edge=T.PANEL_EDGE, radius=6)
        if is_winner:
            pygame.draw.rect(surface, T.SELECT, rect, width=3, border_radius=6)
        view = state.seat_view(seat)
        name = f"{view.name or f'座位{seat}'}"
        if view.disconnected:
            name += "（断线托管）"
        W.draw_text(
            surface,
            f"{state.rel_label(seat)} {name}",
            fonts.get(T.FS_SMALL),
            T.TEXT_DIM,
            (rect[0] + 8, rect[1] + 6, rect[2] - 16, 18),
        )
        delta = entry.get("delta_score")
        color = T.ZERO_SCORE
        if isinstance(delta, int):
            color = T.POS if delta > 0 else (T.NEG if delta < 0 else T.ZERO_SCORE)
            text = f"{delta:+d}"
        else:
            text = "—"
        W.draw_text(
            surface,
            text,
            fonts.get(32, bold=True),
            color,
            (rect[0], rect[1] + 26, rect[2], 44),
            W.ALIGN_CENTER,
        )
        if from_payer is not None and from_payer == seat:
            tag_rect = (rect[0] + rect[2] - 46, rect[1] + 4, 42, 18)
            pygame.draw.rect(surface, T.NEG, tag_rect, border_radius=3)
            W.draw_text(surface, "放炮", fonts.get(T.FS_SMALL), (24, 12, 12), tag_rect, W.ALIGN_CENTER)

    if win is None:
        # §9.3 荒庄分支：和牌者手牌区替换为居中文案
        W.draw_text(
            surface,
            "本局不计分",
            fonts.get(T.FS_H2),
            T.ZERO_SCORE,
            T.R_RESULT_HAND,
            W.ALIGN_CENTER,
        )
    else:
        _render_win_block(surface, ui, win)

    # 两个出口（UI_UX_SPEC §9.2/§9.5）：再来一局 = 断开重连开新局；返回大厅 = 断开回登录界面
    for rect, label in ((T.R_RESULT_REMATCH_BTN, "再来一局"), (T.R_RESULT_BACK_BTN, "返回大厅")):
        hovered = T.rect_contains(rect, ui.mouse)
        W.draw_button(
            surface, fonts, rect, label, enabled=True, hovered=hovered, pressed=hovered and ui.mouse_down
        )

    # 战绩（UI_UX_SPEC §9.6）：账号会话在按钮上方内嵌「汇总 + 最近 ≤10 局」，游客会话不绘制。
    stats_view.render_result_block(surface, ui)

    remaining = "—" if state.remaining is None else str(state.remaining)
    room = state.room_id or "—"
    W.draw_text(
        surface,
        f"最终牌山剩余 {remaining} · 房间 {room} · 点「再来一局」开新局 / 「返回大厅」结束本局",
        fonts.get(T.FS_SMALL),
        T.TEXT_DIM,
        T.R_RESULT_FOOTER,
        W.ALIGN_CENTER,
    )


def _render_win_block(surface: pygame.Surface, ui: UiView, win: dict) -> None:
    state, fonts = ui.state, ui.fonts
    tile = win.get("tile")
    box = T.R_RESULT_WIN_TILE
    if isinstance(tile, str):
        tile_rect = (
            box[0] + (box[2] - T.RESULT_TILE[0]) // 2,
            box[1] + (box[3] - T.RESULT_TILE[1]) // 2,
            T.RESULT_TILE[0],
            T.RESULT_TILE[1],
        )
        W.draw_tile(
            surface, fonts, tile_rect, tile,
            radius=8,
            rank_size=T.FS_TILE_RESULT_RANK,
            suit_size=T.FS_TILE_RESULT_SUIT,
            honor_size=T.FS_TILE_Z_RESULT,
            win_tile=True,
        )

    winner_seat = win.get("seat")
    winner_name = state.name_of(winner_seat) if isinstance(winner_seat, int) else "—"
    tags = _tags_text(win)
    tsumo = bool(win.get("tsumo"))
    per_payer = win.get("per_payer")
    total = win.get("total")
    lines: list[str] = []
    if tsumo:
        lines.append(f"{winner_name} 自摸 {tags}，")
        lines.append(f"三家各付 {per_payer} 分，合计 {total} 分")
    else:
        payer = win.get("from_seat")
        payer_name = state.name_of(payer) if isinstance(payer, int) else "—"
        lines.append(f"{winner_name} 荣和 {tags}，")
        lines.append(f"{payer_name} 支付 {per_payer} 分")
    remaining = "—" if state.remaining is None else str(state.remaining)
    lines.append(f"牌山剩余 {remaining}")
    for i, line in enumerate(lines):
        W.draw_text(
            surface,
            line,
            fonts.get(20),
            T.TEXT,
            (T.R_RESULT_PAYMENT[0], T.R_RESULT_PAYMENT[1] + i * 28, T.R_RESULT_PAYMENT[2], 26),
        )

    if not isinstance(winner_seat, int):
        return
    groups, concealed = group_winner_hand(state, winner_seat)
    area = T.R_RESULT_HAND
    y = area[1] + (area[3] - T.RESULT_HAND_TILE[1]) // 2
    x = area[0]
    win_tile = win.get("tile")
    marked = False
    for group in groups:
        tile_pitch = T.RESULT_HAND_PITCH
        group_w = len(group.tiles) * tile_pitch - 2 + 6
        pygame.draw.rect(surface, T.PANEL, (x, area[1], group_w, area[3]), border_radius=3)
        pygame.draw.rect(surface, T.PANEL_EDGE, (x, area[1], group_w, area[3]), width=1, border_radius=3)
        for i, face in enumerate(group.tiles):
            W.draw_tile(
                surface, fonts,
                (x + 3 + i * tile_pitch, y, T.RESULT_HAND_TILE[0], T.RESULT_HAND_TILE[1]), face,
                radius=4,
                rank_size=T.FS_TILE_RESULT_HAND_RANK,
                suit_size=T.FS_TILE_RESULT_HAND_SUIT,
                honor_size=T.FS_TILE_Z_RESULT_HAND,
            )
        W.draw_text(
            surface, group.label, fonts.get(T.FS_SMALL), T.TEXT_DIM,
            (x + group_w - 18, area[1] + 2, 16, 14), W.ALIGN_RIGHT,
        )
        x += group_w + T.MELD_GROUP_GAP
    for face in concealed:
        is_win_tile = (not marked) and win_tile == face
        if is_win_tile:
            marked = True
        W.draw_tile(
            surface, fonts,
            (x, y, T.RESULT_HAND_TILE[0], T.RESULT_HAND_TILE[1]), face,
            radius=4,
            rank_size=T.FS_TILE_RESULT_HAND_RANK,
            suit_size=T.FS_TILE_RESULT_HAND_SUIT,
            honor_size=T.FS_TILE_Z_RESULT_HAND,
            win_tile=is_win_tile,
        )
        x += T.RESULT_HAND_PITCH
