"""结算界面（UI_UX_SPEC v2.0 §9：局终页 + 场终页两形态）。

调用约定：`app` 先绘制牌桌快照，再调用本模块（蒙层 + 中央面板）。

- **局终页**（`game_end` 后，一场最多 8 次）：标题带局号/连庄、番种明细（`win.yaku[]`
  顺序渲染）、卡片累计分（`match_total`）、下一局倒计时提示（剩余秒数由 `interlude_ms`
  续算）；**无出口按钮**（§9.5：出口只在场终页，局终页发帧会撞服务端 BAD_REQUEST）。
  播报进行中时本页由 `render/broadcast.py`（阶段 A）接管，播完回到本模块（阶段 B）。
- **场终页**（`match_end` 后，每场 1 次）：`rankings[]` 排名表（数组序直渲、并列跳号）
  + 两出口按钮；不渲染局终页遗留状态（金框/番种明细/收付/手牌）。
"""

from __future__ import annotations

import logging

import pygame

from .. import broadcast as BC
from .. import theme as T
from ..tiles import LIMIT_LABEL, RIICHI_YAKU_LABEL
from ..game_state import GameState
from ..tiles import multiset_remove
from ..view import UiView
from . import art
from . import broadcast as broadcast_view
from . import stats as stats_view
from . import widgets as W

import sys
from pathlib import Path

_game_root = next(
    p for p in Path(__file__).resolve().parents
    if (p / "protocol" / "enums.json").is_file()
)
sys.path.insert(0, str(_game_root))
from protocol.enums import REASON

log = logging.getLogger("mahjong.client.result")

#: 纯装饰图层缓存（金色细线 / 和牌光晕）：尺寸固定，没必要每帧重建 Surface
_DECOR: dict[tuple, pygame.Surface] = {}


def _decor(key: tuple, build) -> pygame.Surface:
    surface = _DECOR.get(key)
    if surface is None:
        surface = build()
        _DECOR[key] = surface
    return surface

# 键是 reason 整数常量，值是中文显示文案（PROTOCOL v2.0）
REASON_LABEL = {
    REASON.WIN_TSUMO: "自摸",
    REASON.WIN_RON: "荣和",
    REASON.DRAW_EXHAUSTED: "荒庄（牌山摸尽）",
}

# v2.0（§9.4）：番种 id → 中文标签（id 逐字取自 PROTOCOL v1.6 / RULESET §6.4；14 项全量）。
# 「平和」只作 1 番番种名；无番和牌显示「无番（0 番）」（§9.2，v1.9 旧文案作废）。
YAKU_LABEL = {
    "menzen": "门前清",
    "tanyao": "断幺九",
    "pinfu": "平和",
    "iipeikou": "一杯口",
    "yakuhai": "箭刻",
    "rinshan": "杠上开花",
    "sanshoku_doujun": "三色同顺",
    "ittsuu": "一气通贯",
    "toitoi": "对对和",
    "sanankou": "三暗刻",
    "honitsu": "混一色",
    "shousangen": "小三元",
    "seven_pairs": "七对子",
    "pure_one_suit": "清一色",
}

# 下一局倒计时时长：**不再硬编码 5 s**，读 `game_end.interlude_ms`（缺省 12000，RULINGS C2）；
# 旧常量 `NEXT_ROUND_SECONDS = 5.0` 已删（ARCH §3.8：它本身就是「数到 0 再卡住」的 bug）。
# v2.0（§9.2）：局终页下一局提示行（占用场终页「再来一局」按钮的位置）
R_NEXT_ROUND_HINT = (430, 604, 420, 52)
# v2.0（§9.6）：场终页排名表（行高 32、行首 y=250，按 rankings[] 数组序直渲）
R_RANKINGS = (164, 246, 952, 140)
RANKING_ROW_H = 32
RANKING_ROW_Y0 = 250

NEXT_DEALER_HINT = "下一局庄家"


def _title(state: GameState) -> str:
    result = state.result or {}
    reason = result.get("reason")
    return REASON_LABEL.get(reason, str(reason or "结算"))


def _round_prefix(state: GameState) -> str:
    """局终页标题左段（§9.2 v2.0）：「第 X/4 局 · 连庄 N · 」。"""
    result = state.result or {}
    round_index = result.get("round_index")
    x = round_index + 1 if isinstance(round_index, int) else state.round_index + 1
    count = state.round_count
    renchan = result.get("renchan_count")
    n = renchan if isinstance(renchan, int) else state.renchan_count
    return f"第 {x}/{count} 局 · 连庄 {n} · "


def _yaku_detail(win_diff: dict | None) -> str:
    """番种明细行（§9.2 v2.0 冻结形状）：`明细：门清 1番 · 七对子 2番 ｜ 庄家 +1 ｜ 合计 4 番（16 分）已封顶`。

    - ① 按 `win.yaku[]` 顺序逐个「中文名 `N`番」；
    - ② 「庄家 +`dealer`」（为 0 不显示；**自摸 +tsumo 恒 0，恒不显示**——自摸不是番种，B1.1）；
    - ③ 结尾「合计 `total` 番（`multiplier` 分）」，`base + dealer > 6` 时追加「已封顶」；
    - 无番和牌显示「无番（0 番）」（UX-15 冻结文案；「平和」只作 1 番番种名）。
    """
    if not isinstance(win_diff, dict):
        return "本局不计分"
    # F6（RULINGS §2.6）：「`yaku` 键缺失」≠「0 番」——只有键**存在**且为空数组才是真的无番。
    if "yaku" not in win_diff:
        return "番种明细不可用"
    yaku = [y for y in (win_diff.get("yaku") or []) if isinstance(y, dict) and isinstance(y.get("id"), str)]
    if not yaku:
        return "无番（0 番）"
    # `win.fu` 键存在即日麻（役满 `fu = 0` 也算，不能用 `> 0` 判，否则退回 classic 公式）
    riichi_mode = isinstance(win_diff.get("fu"), int)
    labels = RIICHI_YAKU_LABEL if riichi_mode else YAKU_LABEL
    parts: list[str] = []
    base = 0
    for item in yaku:
        yid = item["id"]
        han = item.get("han") if isinstance(item.get("han"), int) else 0
        base += han
        label = labels.get(yid, YAKU_LABEL.get(yid, str(yid)))
        # 箭刻合并为一条（han = 组数）：han > 1 时显示「箭刻 N 组」（§9.4）
        if yid == "yakuhai" and han > 1:
            parts.append(f"{label} {han} 组")
        else:
            parts.append(f"{label} {han}番")
    text = " · ".join(parts)
    breakdown = win_diff.get("han_breakdown")
    total = base
    multiplier = None
    dealer = 0
    if isinstance(breakdown, dict):
        if isinstance(breakdown.get("total"), int):
            total = breakdown["total"]
        if isinstance(breakdown.get("base"), int):
            base = breakdown["base"]
        dealer = breakdown.get("dealer") if isinstance(breakdown.get("dealer"), int) else 0
        # 日麻：服务端把 `dealer` 按「赢家是庄家」填 1（兼容字段，PROTOCOL §12.5），
        # 但日麻庄家不加番。阶段 A 播报已按「无加成」解释，阶段 B 明细行必须同口径。
        if dealer > 0 and not riichi_mode:
            text += f" ｜ 庄家 +{dealer}"
    multiplier = win_diff.get("multiplier")
    if not isinstance(multiplier, int) or multiplier <= 0:
        multiplier = 1 << total  # = 2^total（§9.4：R = 2^番）
    fu = win_diff.get("fu")
    points = win_diff.get("points")
    if isinstance(fu, int) and isinstance(points, int):
        # 日麻：符 × 番 → 点数（不是 2^番 的倍率制）；役满 `fu = 0` 不写符
        limit = win_diff.get("limit")
        fu_text = f" {fu} 符" if fu > 0 else ""
        text += f" ｜ 合计 {total} 番{fu_text}（{points} 点）"
        if isinstance(limit, str) and limit not in ("none", ""):
            text += f" {LIMIT_LABEL.get(limit, limit)}"
        return text
    text += f" ｜ 合计 {total} 番（{multiplier} 分）"
    # F7：封顶判据是 `base + dealer > 6`，**不是** `total >= 6`——「打满 6 番」不是「被砍」。
    if base + dealer > 6:
        text += "已封顶"
    return text


def _kyoutaku_award_text(state: GameState) -> str:
    """场终剩棒归一位（v1.20，PROTOCOL §12.13）：`场供 2 根 → 太郎 +2000`。

    只有**场终那一局没人收供托**（末局荒庄 / 击飞）才会出现这个字段；
    其余局 `game_end.kyoutaku_award` 恒 `null`，这里返回空串、结算页不多画一行。
    """
    result = state.result or {}
    award = result.get("kyoutaku_award")
    if not isinstance(award, dict):
        return ""
    sticks = award.get("sticks")
    seat = award.get("seat")
    if not isinstance(sticks, int) or sticks <= 0 or not isinstance(seat, int):
        return ""
    points = award.get("points")
    if not isinstance(points, int):
        points = sticks * 1000
    return f"场供 {sticks} 根 → {state.name_of(seat)} +{points}"


def _leader_text(state: GameState) -> str:
    """D-3（§9.2 收付文案末行，灰色）：「本场累计：<领先者昵称> 领先」。

    按 `results[].match_total` 降序取第一名；并列时写「…、… 并列领先」（v2.0）。
    四家全为 0（首局荒庄且无人得分）也是并列，按并列渲染。
    """
    results = _results(state)
    totals = {entry["seat"]: entry.get("match_total") for entry in results
              if isinstance(entry.get("match_total"), int)}
    if not totals:
        return ""
    best = max(totals.values())
    leaders = [seat for seat, total in sorted(totals.items()) if total == best]
    names = "、".join(state.name_of(seat) for seat in leaders)
    if len(leaders) > 1:
        return f"本场累计：{names} 并列领先"
    return f"本场累计：{names} 领先"


def _match_total_of(entry: dict) -> str:
    total = entry.get("match_total")
    if isinstance(total, int):
        return f"累计 {total:+d}"
    return ""


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
        if not meld.tiles and meld.is_concealed_kan:
            # v2.0（§9.3）：暗杠空数组形态（理论上结算页应已揭示为 4 张；仍须容错）——
            # 保留该组并在渲染时画 4 张牌背 + 记 WARN
            log.warning("结算页出现 tiles 为空的暗杠组（未揭示形态），按 4 张牌背渲染")
            groups.append(meld)
            continue
        if multiset_remove(pool, meld.tiles):
            groups.append(meld)
        else:
            log.warning("hand 与 melds 无法匹配（数据异常），退化为不分组渲染")
            return [], hand
    return groups, pool


def _next_dealer(state: GameState) -> int | None:
    """§9.2 下一局提示：下一局庄家由客户端按 §2.2 第 3 条本地推算（连庄不变、轮庄 +1）。"""
    result = state.result or {}
    dealer = result.get("dealer")
    if not isinstance(dealer, int):
        dealer = state.dealer
    if not isinstance(dealer, int):
        return None
    reason = result.get("reason")
    if reason == REASON.DRAW_EXHAUSTED:
        return dealer  # 荒庄一律连庄（§3.3 消歧）
    win = result.get("win")
    winner = win.get("seat") if isinstance(win, dict) else None
    if winner == dealer:
        return dealer
    return (dealer + 1) % 4


def interlude_ms_of(state: GameState) -> int:
    """本局局间窗口（`game_end.interlude_ms`，缺省 12000 / RULINGS C2）。"""
    result = state.result or {}
    value = result.get("interlude_ms")
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return BC.BC_DEFAULT_INTERLUDE_MS
    return value


def _seconds_to_next_round(state: GameState, now: float) -> int:
    """播完之后的剩余秒数：由 `interlude_ms` 的**剩余**续算（约 3 秒），不重置成 5（RULINGS §2.5）。"""
    total_s = interlude_ms_of(state) / 1000.0
    if state.result_at is None:
        return int(total_s + 0.999)
    left = total_s - max(0.0, now - state.result_at)
    return max(0, int(left + 0.999))  # 向上取整，由 N 递减到 0


# ---------------------------------------------------------------- 蒙层（F5：每帧一张 1280×720）

_MASK_SURFACE: pygame.Surface | None = None


def mask_surface() -> pygame.Surface:
    """整屏蒙层缓存：`theme.R_RESULT_MASK` 的尺寸，一次性填充（现 60 fps 曾分配 221 MB/s）。"""
    global _MASK_SURFACE
    if _MASK_SURFACE is None:
        _MASK_SURFACE = pygame.Surface((T.R_RESULT_MASK[2], T.R_RESULT_MASK[3]), pygame.SRCALPHA)
        _MASK_SURFACE.fill(T.MASK)
    return _MASK_SURFACE


def render(surface: pygame.Surface, ui: UiView) -> None:
    state, fonts = ui.state, ui.fonts

    # v4.5：播报期不铺整屏蒙层、不画大面板——四家摊牌必须保持可见。
    # 只画紧凑播报面板，并把四家手牌在各自桌边翻正。
    if state.match_end is None and broadcast_view.is_broadcasting(ui):
        from . import table as table_view

        table_view.draw_revealed_hands(surface, ui)
        broadcast_view.render_round_end(surface, ui)
        return

    surface.blit(mask_surface(), (T.R_RESULT_MASK[0], T.R_RESULT_MASK[1]))

    # 结算面板（v2.1）：投影 + 玻璃 + 顶部鎏金细线，和牌桌的质感语言保持一致
    W.draw_panel(surface, T.R_RESULT_PANEL, fill=(22, 32, 29, 248), edge=(120, 138, 126), radius=16, shadow=True)
    surface.blit(
        _decor(("gold", T.R_RESULT_PANEL[2] - 48), lambda: _build_gold_line(T.R_RESULT_PANEL[2] - 48)),
        (T.R_RESULT_PANEL[0] + 24, T.R_RESULT_PANEL[1] + 8),
    )

    if state.match_end is not None:
        _render_match_end(surface, ui)
    else:
        _render_round_end(surface, ui)


# ---------------------------------------------------------------- 局终页（§9.2–§9.5）


def _render_round_end(surface: pygame.Surface, ui: UiView) -> None:
    state, fonts = ui.state, ui.fonts
    result = state.result or {}
    win = result.get("win") if isinstance(result.get("win"), dict) else None

    # RULINGS v1.2 §2.3 两阶段：播报进行中 ⇒ 阶段 A 整幅舞台（render/broadcast.py），
    # 播报结束 / 不播报 ⇒ 阶段 B，即下面这份 v2.0 现状，**一行不改**。
    if broadcast_view.is_broadcasting(ui):
        broadcast_view.render_round_end(surface, ui)
        return

    # 标题行：局号 + 连庄 + 和牌方式
    W.draw_text(surface, f"{_round_prefix(state)}{_title(state)}", fonts.get(T.FS_H1, bold=True), T.TEXT, T.R_RESULT_TITLE)
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

    # 标题下的细线（装饰：把「局号·连庄·和牌方式」与下面四张卡片分开）
    surface.blit(
        _decor(("rule", T.R_RESULT_PANEL[2] - 48), lambda: _build_rule_line(T.R_RESULT_PANEL[2] - 48)),
        (T.R_RESULT_PANEL[0] + 24, T.R_RESULT_TAGS[1] + T.R_RESULT_TAGS[3] + 4),
    )

    # 番种明细行（v2.0：win.yaku[] 顺序渲染；无番 → 「无番（0 番）」；无自摸加成行）
    W.draw_text(surface, _yaku_detail(win), fonts.get(T.FS_H3), T.TEXT_DIM, T.R_RESULT_TAGS)

    # 四座位结果卡片（v2.0：delta + 累计分 match_total）
    from_payer = win.get("from_seat") if win is not None else None
    for entry in _results(state):
        seat = entry["seat"]
        rel = state.rel_index(seat)
        if not 0 <= rel < len(T.R_RESULT_CARDS_X):
            continue
        rect = (T.R_RESULT_CARDS_X[rel], T.R_RESULT_CARDS_Y, T.R_RESULT_CARD_W, T.R_RESULT_CARDS_H)
        is_winner = win is not None and win.get("seat") == seat
        fill = (34, 66, 52, 248) if is_winner else (24, 32, 30, 240)
        surface.blit(art.strip(rect[2], rect[3], fill, edge=None, radius=10), (rect[0], rect[1]))
        # 顶部 3 px 强调条：赢家金、放炮红、其余灰（走 alpha 图层，别在无 alpha 画布上直接画）
        accent = T.SELECT if is_winner else (T.NEG if from_payer == seat else (255, 255, 255, 30))
        surface.blit(
            art.strip(rect[2] - 2, 3, accent if len(accent) == 4 else (*accent, 255), edge=None, radius=2),
            (rect[0] + 1, rect[1] + 1),
        )
        if is_winner:
            pygame.draw.rect(surface, T.SELECT, rect, width=2, border_radius=10)
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
        # v2.0：卡片累计分（只在 game_end.match_total 出现；state_sync 永远不会有）
        total_text = _match_total_of(entry)
        if total_text:
            W.draw_text(
                surface, total_text, fonts.get(T.FS_SMALL), T.TEXT_DIM,
                (rect[0] + 4, rect[1] + rect[3] - 22, rect[2] - 8, 18), W.ALIGN_CENTER,
            )
        if from_payer is not None and from_payer == seat:
            tag_rect = (rect[0] + rect[2] - 46, rect[1] + 4, 42, 18)
            pygame.draw.rect(surface, T.NEG, tag_rect, border_radius=3)
            W.draw_text(surface, "放炮", fonts.get(T.FS_SMALL), (24, 12, 12), tag_rect, W.ALIGN_CENTER)

    if win is None:
        # §9.3 荒庄分支：和牌者手牌区替换为居中文案
        # 日麻流局有听牌罚符（§12.5 的 tenpai[4]），「本局不计分」是假话。
        tenpai = result.get("tenpai")
        draw_text = (
            f"流局 · 听牌 {sum(1 for flag in tenpai if flag is True)} 家"
            if isinstance(tenpai, list) else "本局不计分"
        )
        award_text = _kyoutaku_award_text(state)
        if award_text:
            # v1.20：场终还剩立直棒时补一行归属（两行同块，避免一行过长被裁）
            W.draw_text(
                surface,
                draw_text,
                fonts.get(T.FS_H2),
                T.ZERO_SCORE,
                (T.R_RESULT_HAND[0], T.R_RESULT_HAND[1] + 6, T.R_RESULT_HAND[2], 44),
                W.ALIGN_CENTER,
            )
            W.draw_text(
                surface,
                award_text,
                fonts.get(T.FS_H3),
                T.TEXT,
                (T.R_RESULT_HAND[0], T.R_RESULT_HAND[1] + 56, T.R_RESULT_HAND[2], 30),
                W.ALIGN_CENTER,
            )
        else:
            W.draw_text(
                surface,
                draw_text,
                fonts.get(T.FS_H2),
                T.ZERO_SCORE,
                T.R_RESULT_HAND,
                W.ALIGN_CENTER,
            )
    else:
        _render_win_block(surface, ui, win)

    # v2.0（§9.2）：下一局提示行（仅局终页；无出口按钮——出口只在场终页 §9.5）
    if result.get("match_will_continue") is False:
        W.draw_text(
            surface, "本场结束，正在统计排名…", fonts.get(T.FS_H3), T.TEXT,
            R_NEXT_ROUND_HINT, W.ALIGN_CENTER,
        )
    else:
        seconds = _seconds_to_next_round(state, ui.now)
        dealer = _next_dealer(state)
        dealer_text = f"{NEXT_DEALER_HINT}：{state.name_of(dealer)}" if dealer is not None else ""
        W.draw_text(
            surface, f"下一局即将开始（{seconds} 秒）", fonts.get(T.FS_H3), T.TEXT,
            (R_NEXT_ROUND_HINT[0], R_NEXT_ROUND_HINT[1], R_NEXT_ROUND_HINT[2], 26), W.ALIGN_CENTER,
        )
        W.draw_text(
            surface, dealer_text, fonts.get(T.FS_SMALL), T.TEXT_DIM,
            (R_NEXT_ROUND_HINT[0], R_NEXT_ROUND_HINT[1] + 28, R_NEXT_ROUND_HINT[2], 22), W.ALIGN_CENTER,
        )

    # 战绩（UI_UX_SPEC §9.6）：账号会话在按钮上方内嵌「汇总 + 最近 ≤10 局」，游客会话不绘制。
    stats_view.render_result_block(surface, ui)

    remaining = "—" if state.remaining is None else str(state.remaining)
    room = state.room_id or "—"
    round_index = result.get("round_index")
    x = round_index + 1 if isinstance(round_index, int) else state.round_index + 1
    W.draw_text(
        surface,
        f"最终牌山剩余 {remaining} · 房间 {room} · 第 {x}/{state.round_count} 局",
        fonts.get(T.FS_SMALL),
        T.TEXT_DIM,
        T.R_RESULT_FOOTER,
        W.ALIGN_CENTER,
    )


# ---------------------------------------------------------------- 场终页（§9.6）


def _render_match_end(surface: pygame.Surface, ui: UiView) -> None:
    state, fonts = ui.state, ui.fonts
    match_end = state.match_end or {}
    hands_played = match_end.get("hands_played")
    renchan = match_end.get("renchan_count")
    n = hands_played if isinstance(hands_played, int) else "?"
    m = renchan if isinstance(renchan, int) else 0

    # 标题行：本场结束 · 共 N 局（含连庄 M 次）
    W.draw_text(
        surface, f"本场结束 · 共 {n} 局（含连庄 {m} 次）",
        fonts.get(T.FS_H1, bold=True), T.TEXT, T.R_RESULT_TITLE,
    )
    W.draw_text(surface, "最终排名如下", fonts.get(T.FS_H3), T.TEXT_DIM, T.R_RESULT_TAGS)

    # 排名表：按 rankings[] **数组顺序**直渲（协议已排好：final_score 降序、同分 seat 升序）；
    # 第 1 名金色描边；并列名次照协议 rank 显示（并列跳号）。
    rankings = [r for r in (match_end.get("rankings") or []) if isinstance(r, dict)]
    for i, entry in enumerate(rankings):
        seat = entry.get("seat")
        if not isinstance(seat, int):
            continue
        y = RANKING_ROW_Y0 + i * RANKING_ROW_H
        rect = (R_RANKINGS[0], y, R_RANKINGS[2], RANKING_ROW_H - 4)
        rank = entry.get("rank") if isinstance(entry.get("rank"), int) else i + 1
        score = entry.get("final_score")
        score_text = f"{score:+d}" if isinstance(score, int) else "—"
        color = T.POS if (isinstance(score, int) and score > 0) else (
            T.NEG if (isinstance(score, int) and score < 0) else T.ZERO_SCORE
        )
        fill = (40, 56, 48, 245) if rank == 1 else T.PANEL
        W.draw_panel(surface, rect, fill=fill, edge=T.PANEL_EDGE, radius=4)
        if rank == 1:
            pygame.draw.rect(surface, T.SELECT, rect, width=2, border_radius=4)
        view = state.seat_view(seat)
        name = view.name or f"座位{seat}"
        if view.disconnected:
            name += "（断线托管）"
        W.draw_text(
            surface,
            f"第 {rank} 名 · {state.rel_label(seat)} {name}",
            fonts.get(T.FS_BODY),
            T.TEXT,
            (rect[0] + 12, rect[1], rect[2] // 2, rect[3]),
            W.ALIGN_LEFT,
        )
        W.draw_text(
            surface, score_text, fonts.get(T.FS_BODY, bold=True), color,
            (rect[0] + rect[2] - 200, rect[1], 188, rect[3]), W.ALIGN_RIGHT,
        )

    # 两出口按钮（§9.5/§9.6：仅场终页渲染）
    for rect, label in ((T.R_RESULT_REMATCH_BTN, "再来一局"), (T.R_RESULT_BACK_BTN, "返回大厅")):
        hovered = T.rect_contains(rect, ui.mouse)
        W.draw_button(
            surface, fonts, rect, label, enabled=True, hovered=hovered, pressed=hovered and ui.mouse_down
        )

    # 战绩块照常（§9.5 第 6 条：账号模式）
    stats_view.render_result_block(surface, ui)

    room = state.room_id or "—"
    W.draw_text(
        surface,
        f"房间 {room} · 本场战绩已存档",
        fonts.get(T.FS_SMALL),
        T.TEXT_DIM,
        T.R_RESULT_FOOTER,
        W.ALIGN_CENTER,
    )


def _build_gold_line(width: int) -> pygame.Surface:
    line = pygame.Surface((width, 2), pygame.SRCALPHA)
    line.fill((232, 192, 104, 120))
    return line


def _build_rule_line(width: int) -> pygame.Surface:
    line = pygame.Surface((width, 1), pygame.SRCALPHA)
    line.fill((255, 255, 255, 26))
    return line


def _build_win_glow(width: int, height: int) -> pygame.Surface:
    glow = pygame.Surface((width + 40, height + 40), pygame.SRCALPHA)
    for index, spread in enumerate((0, 8, 16)):
        pygame.draw.rect(
            glow, (232, 192, 104, 26 - index * 7),
            (20 - spread, 20 - spread, width + spread * 2, height + spread * 2),
            border_radius=18,
        )
    return glow


def _render_win_block(surface: pygame.Surface, ui: UiView, win: dict) -> None:
    state, fonts = ui.state, ui.fonts
    tile = win.get("tile")
    box = T.R_RESULT_WIN_TILE
    if isinstance(tile, str):
        # 和牌张后面垫一层金色光晕，让「胡的是哪张」一眼可见
        surface.blit(
            _decor(("glow", box[2], box[3]), lambda: _build_win_glow(box[2], box[3])),
            (box[0] - 20, box[1] - 20),
        )
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
    # §9.2（v2.0）收付文案：荣和 =「<和牌者> 荣和，合计 <total> 番，<放炮者> 支付 <per_payer> 分」；
    # 自摸 =「<和牌者> 自摸，合计 <total> 番，三家各付 <per_payer> 分，合计 <total> 分」；
    # 末行灰色「本场累计：<领先者昵称> 领先」（D-3：并列时「并列领先」）。
    # 自摸**不渲染加成行**（B1.1：自摸不是番种，tsumo 恒 0 自然不显示）。
    # 日麻（有 `win.fu`）：单位是「点」不是「分」，句首写「N 番 M 符」——与阶段 A 的
    # 结论句 / S5 格子同口径，避免播报页与静态结算页各说一套。
    han_total = None
    breakdown = win.get("han_breakdown")
    if isinstance(breakdown, dict) and isinstance(breakdown.get("total"), int):
        han_total = breakdown["total"]
    tsumo = bool(win.get("tsumo"))
    per_payer = win.get("per_payer")
    total = win.get("total")
    riichi_mode = isinstance(win.get("fu"), int)
    unit = "点" if riichi_mode else "分"
    if riichi_mode:
        fu = win.get("fu")
        fu_text = f" {fu} 符" if isinstance(fu, int) and fu > 0 else ""
        han_text = f"{han_total} 番{fu_text}" if han_total is not None else ""
    else:
        han_text = f"合计 {han_total} 番" if han_total is not None else ""
    lines: list[tuple[str, tuple]] = []
    if tsumo:
        lines.append((f"{winner_name} 自摸，{han_text}，", T.TEXT))
        lines.append((f"三家各付 {per_payer} {unit}，合计 {total} {unit}", T.TEXT))
    else:
        payer = win.get("from_seat")
        payer_name = state.name_of(payer) if isinstance(payer, int) else "—"
        lines.append((f"{winner_name} 荣和，{han_text}，", T.TEXT))
        lines.append((f"{payer_name} 支付 {per_payer} {unit}", T.TEXT))
    leader = _leader_text(state)
    if leader:
        lines.append((leader, T.TEXT_DIM))
    for i, (line, color) in enumerate(lines):
        W.draw_text(
            surface,
            line,
            fonts.get(20),
            color,
            (T.R_RESULT_PAYMENT[0], T.R_RESULT_PAYMENT[1] + i * 28, T.R_RESULT_PAYMENT[2], 26),
        )

    remaining = "—" if state.remaining is None else str(state.remaining)
    W.draw_text(
        surface,
        f"牌山剩余 {remaining}",
        fonts.get(20),
        T.TEXT,
        (T.R_RESULT_PAYMENT[0], T.R_RESULT_PAYMENT[1] + len(lines) * 28, T.R_RESULT_PAYMENT[2], 26),
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
        faces: list[str | None] = list(group.tiles) or [None] * 4  # 暗杠空数组 ⇒ 4 张牌背（§4.8）
        group_w = len(faces) * tile_pitch - 2 + 6
        pygame.draw.rect(surface, T.PANEL, (x, area[1], group_w, area[3]), border_radius=3)
        pygame.draw.rect(surface, T.PANEL_EDGE, (x, area[1], group_w, area[3]), width=1, border_radius=3)
        for i, face in enumerate(faces):
            rect = (x + 3 + i * tile_pitch, y, T.RESULT_HAND_TILE[0], T.RESULT_HAND_TILE[1])
            if face is None:
                W.draw_tile_back(surface, rect)
                continue
            W.draw_tile(
                surface, fonts, rect, face,
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
