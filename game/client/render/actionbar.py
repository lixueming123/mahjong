"""操作界面（UI_UX_SPEC v2.0 §5 操作栏 + §6 倒计时，与牌桌同屏叠加）。

界面映射：`SCREEN_ACTIONBAR` 在 §2.1 中被定义为「覆盖层组件」而非独立屏幕，
本实现对应 `GameState.window is not None and screen == SCREEN_TABLE and not frozen`
（`bar_visible`），即「本连接最近收到一帧 `action_request` 且该窗口未被新帧推进」。

按钮顺序冻结为 `打牌 → 胡 → 杠 → 暗杠 → 加杠 → 碰 → 吃（每条 option 一个）→ 过`
（§5.3 v2.0：对应协议 options 全局顺序 `win → kong → ankan → kakan → pong → chow → pass`）。
打牌窗口 **不提供**「过」按钮（§5.3.1 / PROTOCOL H1）；`ankan`/`kakan` 只出现在打牌窗口
（PROTOCOL v1.6 §4.6），提交 `tiles` 恰好 1 个牌面元素。
"""

from __future__ import annotations

import logging
import math
from dataclasses import dataclass, field

import pygame

from .. import theme as T
from ..game_state import SCREEN_TABLE, WINDOW_CHANKAN, WINDOW_CLAIM, WINDOW_DISCARD, GameState
from ..tiles import button_label, tile_label
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
from protocol.enums import ACTION, FRAME_TYPE

log = logging.getLogger("mahjong.client.actionbar")

MAX_CHOW_BUTTONS = 3
FLASH_PERIOD_MS = 400.0
#: 按钮文字左右内边距：可用宽度 = 按钮宽（冻结 112px）− 2×该值。
#: 取 2px（仅留出 1px 边框 + 1px 呼吸）而非更宽的值，是为了让 §5.3 冻结的**最长**文案
#: 「吃 3 筒 4 筒」（实测 102px）能在 112px 按钮内**完整显示**而无需省略号；
#: 更宽的文案仍走 `fit_label` 的省略号截断（保证不换行、不溢出按钮框）。
BUTTON_TEXT_PAD = 2
ELLIPSIS = "…"


@dataclass
class ButtonSpec:
    # kind 为整数：打牌按钮用 FRAME_TYPE.DISCARD，其余用 ACTION.* 常量。
    kind: int
    label: str
    rect: tuple[int, int, int, int]
    enabled: bool
    tiles: list[str] = field(default_factory=list)
    win: bool = False
    #: 禁用原因（`enabled=False` 时才有值）：点击它以 toast 说明「差哪一步」。
    #: 禁用按钮静默吞掉点击会让玩家以为功能坏了（立直就是典型：它是「选宣言牌 + 打牌」
    #: 两段式，没选牌时按钮是灰的，玩家只会觉得「这里不能立直吗」）。
    reason: str = ""


def bar_visible(state: GameState) -> bool:
    return state.screen == SCREEN_TABLE and state.window is not None and not state.frozen


def _base_enabled(state: GameState, remaining_ms: float | None) -> bool:
    if state.pending or state.frozen:
        return False
    if state.screen != SCREEN_TABLE:
        return False
    if remaining_ms is not None and remaining_ms <= 0:
        return False
    return True


def build_buttons(state: GameState, remaining_ms: float | None) -> list[ButtonSpec]:
    """计算按钮组（同时供渲染与命中测试使用，保证两者一致）。"""
    if not bar_visible(state) or state.window is None:
        return []
    window = state.window
    base = _base_enabled(state, remaining_ms)

    specs: list[tuple[int, str, list[str], bool]] = []  # kind,label,tiles,win
    if window.window == WINDOW_DISCARD:
        # 打牌按钮发 FRAME_TYPE.DISCARD 帧；"打牌" 是中文显示文案，不走 button_label。
        specs.append((FRAME_TYPE.DISCARD, "打牌", [], False))
    if window.has(ACTION.WIN):
        specs.append((ACTION.WIN, button_label(ACTION.WIN), [], True))
    # 日麻立直：与打牌同一条 action，需要先选中「宣言后打出的那张」。
    if window.has(ACTION.RIICHI):
        specs.append((ACTION.RIICHI, button_label(ACTION.RIICHI), [], False))
    for tiles in window.option_list(ACTION.KONG)[:1]:
        specs.append((ACTION.KONG, button_label(ACTION.KONG, tiles), tiles, False))
    # v2.0（§5.3）：暗杠 / 加杠 紧随明杠之后。暗杠可能有多条 option（每个可杠牌面一条，
    # §5.7），每条一个按钮、文案均「暗杠」（不显示牌名）；提交时带该 option 的 1 个牌面。
    for tiles in window.option_list(ACTION.ANKAN):
        specs.append((ACTION.ANKAN, button_label(ACTION.ANKAN), tiles, False))
    for tiles in window.option_list(ACTION.KAKAN):
        specs.append((ACTION.KAKAN, button_label(ACTION.KAKAN, tiles), tiles, False))
    for tiles in window.option_list(ACTION.PONG)[:1]:
        specs.append((ACTION.PONG, button_label(ACTION.PONG, tiles), tiles, False))
    chow_options = window.option_list(ACTION.CHOW)
    if len(chow_options) > MAX_CHOW_BUTTONS:
        log.warning("chow 选项超过 3 条（协议不该出现）：%d，只渲染前 3 条", len(chow_options))
        chow_options = chow_options[:MAX_CHOW_BUTTONS]
    for tiles in chow_options:
        specs.append((ACTION.CHOW, button_label(ACTION.CHOW, tiles), tiles, False))
    if window.window in (WINDOW_CLAIM, WINDOW_CHANKAN):
        specs.append((ACTION.PASS, button_label(ACTION.PASS), [], False))

    if len(specs) > T.BAR_BUTTON_MAX:
        log.warning("按钮数超过 %d，已截断", T.BAR_BUTTON_MAX)
        specs = specs[: T.BAR_BUTTON_MAX]
    if not specs:
        return []

    n = len(specs)
    width = min(T.BAR_BUTTON_W, int((T.R_BAR_BUTTONS[2] - T.BAR_BUTTON_GAP * (n - 1)) / n))
    out: list[ButtonSpec] = []
    x = T.R_BAR_BUTTONS[0]
    for kind, label, tiles, is_win in specs:
        reason = ""
        if kind == FRAME_TYPE.DISCARD:
            # 打牌要先把那张牌选出来（点手牌 → 点按钮）
            enabled = base and state.selected_index is not None
            if base and state.selected_index is None:
                reason = "先点选一张手牌再打牌"
        elif kind == ACTION.RIICHI:
            # v1.21 §5.3.2：立直按钮**不再要求先选牌** —— 点它进入「宣言模式」，
            # 手牌上会标出哪几张能作宣言牌（服务端 `riichi_tiles`），点其中一张即宣言。
            enabled = base
        else:
            enabled = base
        out.append(
            ButtonSpec(
                kind=kind,
                label=label,
                rect=(x, T.R_BAR_BUTTONS[1], width, T.BAR_BUTTON_H),
                enabled=enabled,
                tiles=list(tiles),
                win=is_win,
                reason=reason,
            )
        )
        x += width + T.BAR_BUTTON_GAP
    return out


def countdown_text(remaining_ms: float) -> str:
    if remaining_ms < 1000:
        return f"{remaining_ms / 1000.0:.1f} s"
    return f"{math.ceil(remaining_ms / 1000.0)} s"


def countdown_color(remaining_ms: float) -> tuple:
    if remaining_ms > 10000:
        return T.POS
    if remaining_ms > 3000:
        return T.WARN
    return T.NEG


def fit_label(text: str, font: pygame.font.Font, max_width: int) -> str:
    """文案过长时以**省略号截断**（UI_UX_SPEC v1.4 §5.3：不得换行、不得溢出按钮框）。

    返回值保证 `font.size(result)[0] <= max_width`（`max_width` 为正时）；
    连省略号都放不下时返回空串（调用方仍只画背景，不会溢出）。
    """
    if max_width <= 0:
        return ""
    if font.size(text)[0] <= max_width:
        return text
    for cut in range(len(text) - 1, 0, -1):
        candidate = text[:cut] + ELLIPSIS
        if font.size(candidate)[0] <= max_width:
            return candidate
    return ""


def render(surface: pygame.Surface, ui: UiView, remaining_ms: float | None) -> None:
    state, fonts = ui.state, ui.fonts
    if state.window is None or state.screen != SCREEN_TABLE:
        return
    window = state.window

    # 底部操作带（v3.2）：参考里没有整条深色底栏，这里只留一层「越往下越暗」的渐变，
    # 让按钮浮在桌布上，而不是压在一条黑边里
    band = (0, T.R_BAR[1] - 6, T.LOGICAL_W, T.R_BAR[3] + 12)
    fade = pygame.Surface((band[2], band[3]), pygame.SRCALPHA)
    for row in range(band[3]):
        fade.fill((6, 4, 8, int(96 * (row / band[3]) ** 0.8)), (0, row, band[2], 1))
    surface.blit(fade, (band[0], band[1]))

    # 倒计时区
    surface.blit(
        art.strip(T.R_BAR_TIMER[2], T.R_BAR_TIMER[3], (8, 6, 12, 84), edge=(150, 116, 140, 70), radius=12),
        (T.R_BAR_TIMER[0], T.R_BAR_TIMER[1]),
    )
    if remaining_ms is None:
        remaining_ms = 0.0
    color = countdown_color(remaining_ms)
    alpha = 255
    if 0 < remaining_ms < 3000:
        phase = (ui.now * 1000.0 % FLASH_PERIOD_MS) / FLASH_PERIOD_MS
        alpha = 255 if phase < 0.5 else int(255 * 0.6)
    text = countdown_text(remaining_ms)
    if remaining_ms <= 0:
        text = "0 s"
    W.draw_text(
        surface, text, fonts.get(T.FS_H2, bold=True), color, T.R_BAR_TIMER_TEXT, W.ALIGN_CENTER, alpha=alpha
    )
    ratio = 0.0 if window.window_ms <= 0 else max(0.0, min(1.0, remaining_ms / window.window_ms))
    surface.blit(
        art.meter(T.R_BAR_TIMER_BAR[2], T.R_BAR_TIMER_BAR[3], ratio, color),
        (T.R_BAR_TIMER_BAR[0], T.R_BAR_TIMER_BAR[1]),
    )
    if remaining_ms <= 0:
        W.draw_text(
            surface, "已超时，等待服务端行动", fonts.get(T.FS_SMALL), T.NEG,
            (T.R_BAR_TIMER[0], T.R_BAR_TIMER[1] + T.R_BAR_TIMER[3] - 16, T.R_BAR_TIMER[2], 16),
            W.ALIGN_CENTER,
        )

    # 按钮区：不再套一层面板，按钮自己带投影浮在操作带上
    buttons = build_buttons(state, remaining_ms)
    button_font = fonts.get(T.FS_BODY)
    right_edge = T.R_BAR_BUTTONS[0]
    for spec in buttons:
        hovered = T.rect_contains(spec.rect, ui.mouse)
        # §5.3：文案过长以省略号截断（不换行、不溢出按钮框）
        label = fit_label(spec.label, button_font, spec.rect[2] - 2 * BUTTON_TEXT_PAD)
        W.draw_button(
            surface,
            fonts,
            spec.rect,
            label,
            enabled=spec.enabled,
            hovered=hovered,
            pressed=hovered and ui.mouse_down,
            win=spec.win,
        )
        right_edge = spec.rect[0] + spec.rect[2]
        if remaining_ms <= 0 and hovered:
            W.draw_text(
                surface, "已超时", fonts.get(T.FS_SMALL), T.NEG,
                (spec.rect[0], spec.rect[1] - 18, spec.rect[2], 16), W.ALIGN_CENTER,
            )

    # 状态提示（按钮区右侧空白）：PENDING > 非法计数 > 中性提示
    status_x = right_edge + 12
    status_w = T.R_BAR_BUTTONS[0] + T.R_BAR_BUTTONS[2] - status_x
    if status_w > 60:
        text, fallback, color = _status_text(state, ui)
        if text:
            font = fonts.get(T.FS_SMALL)
            if fallback and font.size(text)[0] > status_w:
                text = fallback
            if font.size(text)[0] > status_w:
                # 仍然过长 → 与按钮同一口径：省略号截断（保证不溢出、不换行）
                text = fit_label(text, font, status_w)
            if text:
                W.draw_text(
                    surface, text, font, color,
                    (status_x, T.R_BAR_BUTTONS[1], status_w, T.BAR_BUTTON_H), W.ALIGN_RIGHT,
                )


def _status_text(state: GameState, ui: UiView) -> tuple[str, str, tuple]:
    """返回 (主文案, 等宽不足时的短文案, 颜色)。"""
    if state.pending:
        return f"已提交：{state.pending_label or ''}", "", T.TEXT_DIM
    if ui.illegal_count > 0:
        return f"非法操作 {ui.illegal_count}/10", "", T.WARN
    if state.window is None:
        return "", "", T.TEXT_DIM
    if state.window.window == WINDOW_DISCARD:
        if state.window.has(ACTION.WIN):
            # §5.3.1：打牌窗口不提供「过」按钮，用中性提示帮助理解
            return "也可直接点手牌打牌（本局不胡）", "也可直接点手牌打牌", T.TEXT_DIM
        if state.window.has(ACTION.RIICHI):
            # 立直 = 宣言牌 + 打牌 的合成动作（PROTOCOL §12.5）。v1.21 起按钮先可点、
            # 点开后在手牌上高亮「能作宣言牌」的那几张，所以提示改成「点立直 → 点高亮的牌」。
            if state.riichi_arming:
                return "立直：只能打高亮的牌", "只能打高亮的牌", T.WARN
            if state.selected_index is not None:
                return "点立直：宣言并打出选中的牌", "再点立直", T.TEXT_DIM
            return "点立直，再点一张高亮的牌", "立直→点高亮的牌", T.TEXT_DIM
        return "请打牌", "", T.TEXT_DIM
    if state.window.window == WINDOW_CHANKAN:
        # 抢杠的那张牌不在牌河里，玩家只能靠这行知道自己在抢什么
        tile = getattr(state.window, "tile", None)
        if tile:
            return f"抢杠 {tile_label(tile)} · 可胡", f"抢杠 {tile_label(tile)}", T.WARN
        return "抢杠 · 可胡", "抢杠", T.WARN
    return "请选择动作", "", T.TEXT_DIM
