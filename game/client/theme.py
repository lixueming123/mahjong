"""主题常量：颜色、字体、坐标。

全部数值逐字取自 `game/docs/UI_UX_SPEC.md`：
- §1.1 逻辑分辨率与缩放
- §1.3 美术约束（仅几何绘制 + 系统字体，无外部图片资源）
- §1.4 颜色常量
- §3.2 登录界面坐标
- §4.3 牌桌分区坐标
- §5.2 操作栏坐标
- §9.2 结算界面坐标

本模块只提供常量与纯函数，不修改任何状态、不发送任何帧。
"""

from __future__ import annotations

import logging

import pygame

log = logging.getLogger("mahjong.client.theme")

# ---------------------------------------------------------------- §1.1 分辨率

LOGICAL_W = 1280
LOGICAL_H = 720
TARGET_FPS = 60
WINDOW_TITLE = "麻将 · ex_actor"

# ---------------------------------------------------------------- §1.4 颜色

BG = (16, 68, 44)
BG_LOGIN = (18, 58, 42)
PANEL = (24, 32, 30, 235)
PANEL_EDGE = (96, 112, 104)
FACE = (246, 244, 234)
FACE_EDGE = (168, 164, 150)
BACK = (40, 96, 160)
BACK_EDGE = (24, 64, 112)
SUIT_M = (176, 32, 32)
SUIT_P = (32, 96, 176)
SUIT_S = (24, 128, 64)
#: 字牌墨色（UI_UX_SPEC v1.3 §1.4 新增）：字牌字形与其单元格文字统一使用该色
SUIT_Z = (48, 44, 40)
TEXT = (236, 236, 232)
TEXT_DIM = (176, 180, 176)
SELECT = (255, 200, 64)
DRAWN = (96, 224, 224)
ACTIVE = (255, 214, 96)
DEALER = (220, 176, 64)
POS = (96, 216, 128)
NEG = (232, 96, 96)
ZERO_SCORE = (170, 170, 170)
WARN = (240, 200, 80)
BTN = (58, 104, 80)
BTN_HOVER = (78, 134, 104)
BTN_DOWN = (44, 82, 64)
BTN_DISABLED = (78, 86, 82)
BTN_DISABLED_TEXT = (140, 146, 142)
BTN_WIN = (168, 124, 32)
BTN_WIN_HOVER = (200, 152, 48)
MASK = (0, 0, 0, 150)

COLORS = {
    "BG": BG,
    "BG_LOGIN": BG_LOGIN,
    "PANEL": PANEL,
    "PANEL_EDGE": PANEL_EDGE,
    "FACE": FACE,
    "FACE_EDGE": FACE_EDGE,
    "BACK": BACK,
    "BACK_EDGE": BACK_EDGE,
    "SUIT_M": SUIT_M,
    "SUIT_P": SUIT_P,
    "SUIT_S": SUIT_S,
    "SUIT_Z": SUIT_Z,
    "TEXT": TEXT,
    "TEXT_DIM": TEXT_DIM,
    "SELECT": SELECT,
    "DRAWN": DRAWN,
    "ACTIVE": ACTIVE,
    "DEALER": DEALER,
    "POS": POS,
    "NEG": NEG,
    "ZERO_SCORE": ZERO_SCORE,
    "WARN": WARN,
    "BTN": BTN,
    "BTN_HOVER": BTN_HOVER,
    "BTN_DOWN": BTN_DOWN,
    "BTN_DISABLED": BTN_DISABLED,
    "BTN_DISABLED_TEXT": BTN_DISABLED_TEXT,
    "BTN_WIN": BTN_WIN,
    "BTN_WIN_HOVER": BTN_WIN_HOVER,
    "MASK": MASK,
}

# ---------------------------------------------------------------- §1.3 字体

FONT_FAMILIES = [
    "PingFang SC",
    "Heiti SC",
    "Hiragino Sans GB",
    "Microsoft YaHei",
    "Noto Sans CJK SC",
    "WenQuanYi Micro Hei",
    "Arial Unicode MS",
    "DejaVu Sans",
]


class FontBook:
    """系统字体缓存（§1.3 第 2 条）。

    - 按候选族顺序用 `pygame.font.match_font` 查找；
    - 全部缺失时回退 `pygame.font.Font(None, size)`（中文显示为方块但不崩溃），并记一条 WARN。
    """

    def __init__(self) -> None:
        self._cache: dict[tuple[int, bool], pygame.font.Font] = {}
        self._path: str | None = None
        self._resolved = False

    def _resolve(self) -> str | None:
        if self._resolved:
            return self._path
        self._resolved = True
        for name in FONT_FAMILIES:
            try:
                path = pygame.font.match_font(name)
            except Exception:  # pragma: no cover - 防御性
                path = None
            if path:
                self._path = path
                log.info("使用系统字体：%s (%s)", name, path)
                return self._path
        log.warning("未找到任何候选中文字体，回退默认字体：中文可能显示为方块")
        return None

    def get(self, size: int, bold: bool = False) -> pygame.font.Font:
        key = (size, bold)
        font = self._cache.get(key)
        if font is None:
            path = self._resolve()
            font = pygame.font.Font(path, size) if path else pygame.font.Font(None, size)
            font.set_bold(bold)
            self._cache[key] = font
        return font


# ---------------------------------------------------------------- §3.2 登录
# ACCOUNT_DESIGN §5：卡片长高以容纳「游客/账号」模式切换与账号模式下的密码框
# （这套坐标是 UI_UX_SPEC §3.2 冻结表的 v1.5 修订值）。

L_CARD = (400, 150, 480, 470)
L_TITLE = (400, 168, 480, 60)
L_SUBTITLE = (400, 230, 480, 28)
L_MODE_GUEST = (432, 266, 198, 36)
L_MODE_ACCOUNT = (650, 266, 198, 36)
L_LABEL = (432, 310, 416, 24)
L_INPUT = (432, 336, 416, 48)
L_LABEL2 = (432, 390, 416, 24)
L_INPUT2 = (432, 416, 416, 48)
L_MAIN_BTN = (432, 472, 198, 52)
L_REGISTER_BTN = (650, 472, 198, 52)
L_STATUS = (432, 532, 416, 26)
L_SERVER = (432, 562, 416, 20)
L_QUIT = (1080, 650, 160, 44)
L_RETRY = (432, 586, 200, 34)

# ---------------------------------------------------------------- §4.3 牌桌

R_HUD = (0, 0, 1280, 34)
R_HUD_LEFT = (8, 0, 320, 34)
# v2.0（§4.11）：局号位「X/4」（X = round_index + 1，来自 game_start；连庄 N 显示在局号右侧
# 空档，账号会话该空档为战绩按钮，故连庄文案并入中段阶段文案）
R_HUD_ROUND = (320, 0, 84, 34)
R_HUD_MID = (520, 0, 240, 34)
R_HUD_RIGHT = (960, 0, 312, 34)
# v1.7：战绩入口（HUD 左块 8..328 与中块 520..760 之间的空档，只在账号会话显示）
R_HUD_STATS_BTN = (404, 4, 88, 26)

R_OPP_NAME = (280, 36, 720, 20)
R_OPP_BACK = (514, 58, 252, 16)
# v2.0（§4.6）：他家手牌背容量 14 → 18（4 组全杠时 hand_count = 18）；15..18 张换行为
# 第二行（行距 16 px，向下扩展），不裁剪、不缩小
OPP_BACK_MAX = 18
OPP_BACK_ROW_CAPACITY = 14
R_OPP_MELD = (452, 80, 376, 36)
R_OPP_RIVER = (545, 126, 190, 158)

R_CENTER = (280, 290, 720, 60)
R_CENTER_LINE1 = (280, 292, 720, 28)
R_CENTER_LINE2 = (280, 322, 720, 26)

R_SELF_RIVER = (545, 356, 190, 158)
R_SELF_MELD = (452, 518, 376, 36)
R_SELF_HAND = (235, 564, 810, 80)

R_SIDE_L = (8, 36, 260, 518)
R_SIDE_R = (1012, 36, 260, 518)
R_SIDE_L_NAME = (8, 36, 260, 20)
R_SIDE_R_NAME = (1012, 36, 260, 20)
R_SIDE_L_BACK = (12, 60, 252, 16)
R_SIDE_R_BACK = (1016, 60, 252, 16)
R_SIDE_L_MELD = (8, 82, 260, 160)
R_SIDE_R_MELD = (1012, 82, 260, 160)
R_SIDE_L_RIVER = (8, 248, 260, 306)
R_SIDE_R_RIVER = (1012, 248, 260, 306)

R_TOAST = (440, 344, 400, 24)

# 牌元素几何（§4.4）
HAND_TILE_W = 56
HAND_TILE_H = 76
HAND_PITCH = 58
HAND_Y = 566
HAND_SELECT_LIFT = 8
HAND_DRAWN_LIFT = 4

RIVER_CENTER_TILE = (30, 38)
RIVER_CENTER_COL_PITCH = 32
RIVER_CENTER_ROW_PITCH = 40
RIVER_CENTER_COLS = 6
RIVER_CENTER_ROWS = 4
RIVER_CENTER_CAPACITY = RIVER_CENTER_COLS * RIVER_CENTER_ROWS  # 24
RIVER_CENTER_COMPACT_TILE = (24, 30)
RIVER_CENTER_COMPACT_COL_PITCH = 26
RIVER_CENTER_COMPACT_ROW_PITCH = 36
RIVER_CENTER_COMPACT_COLS = 7
RIVER_CENTER_COMPACT_ROWS = 5

RIVER_SIDE_TILE = (26, 32)
RIVER_SIDE_COL_PITCH = 28
RIVER_SIDE_ROW_PITCH = 34
RIVER_SIDE_COLS = 3
RIVER_SIDE_ROWS = 9

MELD_TILE = (28, 36)
MELD_TILE_PITCH = 30
MELD_GROUP_GAP = 8
MELD_ROW_HEIGHT = 40

OPP_BACK_TILE = (16, 16)
OPP_BACK_PITCH = 18

# ---------------------------------------------------------------- §5.2 操作栏

R_BAR = (8, 650, 1264, 66)
R_BAR_TIMER = (8, 650, 224, 66)
R_BAR_TIMER_TEXT = (16, 656, 208, 30)
R_BAR_TIMER_BAR = (16, 690, 208, 12)
R_BAR_BUTTONS = (240, 656, 1032, 54)
BAR_BUTTON_H = 46
BAR_BUTTON_W = 112
BAR_BUTTON_GAP = 8
BAR_BUTTON_MAX = 8

# ---------------------------------------------------------------- §8 错误态

R_ERROR_STRIP = (240, 36, 800, 44)
R_BANNER_WITH_BAR = (240, 610, 800, 32)
R_BANNER_NO_BAR = (240, 668, 800, 32)
R_DISCONNECT_PANEL = (440, 280, 400, 160)

# ---------------------------------------------------------------- §9.2 结算

R_RESULT_MASK = (0, 0, 1280, 720)
R_RESULT_PANEL = (140, 40, 1000, 640)
R_RESULT_TITLE = (164, 60, 952, 44)
R_RESULT_TAGS = (164, 106, 952, 24)
R_RESULT_CARDS_Y = 130
R_RESULT_CARDS_H = 96
R_RESULT_CARDS_X = (164, 404, 644, 884)
R_RESULT_CARD_W = 220
R_RESULT_WIN_TILE = (500, 246, 120, 120)
R_RESULT_PAYMENT = (660, 246, 440, 120)
R_RESULT_HAND = (164, 386, 952, 100)
# v1.6：结算页两个出口——「再来一局」（断开重连开新局）与「返回大厅」
R_RESULT_REMATCH_BTN = (430, 604, 200, 52)
R_RESULT_BACK_BTN = (650, 604, 200, 52)
R_RESULT_FOOTER = (164, 660, 952, 20)

# ---------------------------------------------------------------- v1.7 战绩
# 牌桌弹窗（UI_UX_SPEC §4.11）：汇总 + 最近 ≤10 局
R_STATS_PANEL = (340, 96, 600, 440)
R_STATS_TITLE = (364, 116, 552, 32)
R_STATS_SUMMARY = (364, 158, 552, 26)
R_STATS_HEADER = (364, 196, 552, 22)
R_STATS_ROWS_Y = 224
R_STATS_ROW_H = 26
# 结算页内嵌（UI_UX_SPEC §9.6）：汇总一行 + 最近 ≤10 局排成 2 列 × 5 行
R_RESULT_STATS_SUMMARY = (164, 486, 952, 22)
R_RESULT_STATS_COL_X = (164, 644)
R_RESULT_STATS_COL_W = 440
R_RESULT_STATS_ROWS_Y = 512
R_RESULT_STATS_ROW_H = 18

# ---------------------------------------------------------------- v1.8 大厅（UI_UX_SPEC §12）
# 登录成功后先停在这里：玩家卡片 + 战绩 + 「开始对局」/「返回登录」。
LB_CARD = (360, 80, 560, 560)
LB_TITLE = (384, 100, 512, 52)
LB_PLAYER = (384, 164, 512, 26)
LB_SUMMARY = (384, 200, 512, 24)
# 战绩列表只画最近 4 行（牌桌弹窗仍是 ≤10）。4×24 结束于 y=328，必须停在房间标题之上。
LB_ROWS_X = 384
LB_ROWS_Y = 232
LB_ROWS_W = 512
LB_ROW_H = 24
LB_RECENT_ROWS = 4
LB_ROOMS_LABEL = (384, 348, 512, 22)
LB_ROOMS_X = 384
LB_ROOMS_Y = 372
LB_ROOMS_W = 512
LB_ROOM_ROW_H = 24
LB_ROOM_ROWS = 4
LB_NEW_BTN = (384, 492, 160, 48)
LB_START_BTN = (552, 492, 168, 48)
LB_BACK_BTN = (728, 492, 168, 48)
LB_STATUS = (384, 556, 512, 26)
LB_SERVER = (384, 588, 512, 20)
LB_QUIT = (1080, 650, 160, 44)

RESULT_TILE = (100, 120)
RESULT_HAND_TILE = (42, 56)
RESULT_HAND_PITCH = 44

# ---------------------------------------------------------------- 字体字号

FS_H1 = 40
FS_H2 = 28
FS_H3 = 22
FS_BODY = 18
FS_SMALL = 15
FS_TILE_HAND_RANK = 26
FS_TILE_HAND_SUIT = 15
FS_TILE_RIVER_RANK = 16
FS_TILE_RIVER_SUIT = 11
FS_TILE_SIDE_RANK = 14
FS_TILE_SIDE_SUIT = 10
FS_TILE_MELD_RANK = 15
FS_TILE_MELD_SUIT = 10
FS_TILE_RESULT_RANK = 44
FS_TILE_RESULT_SUIT = 22
FS_TILE_RESULT_HAND_RANK = 22
FS_TILE_RESULT_HAND_SUIT = 13

# 字牌字号（UI_UX_SPEC v1.3 §4.4「字牌字体」列 + 附录 A：單字 N px **粗體** 居中、无角标）——
# 实现方不得自行取值（§4.4 D3 注 / 附录 D S9）。
FS_TILE_Z_HAND = 34
FS_TILE_Z_RIVER_CENTER = 20
FS_TILE_Z_RIVER_SIDE = 17
FS_TILE_Z_MELD = 19
FS_TILE_Z_RESULT = 56
FS_TILE_Z_RESULT_HAND = 28


def lobby_room_row_rect(index: int) -> tuple[int, int, int, int]:
    """大厅第 index 间房间行。绘制与点击命中用同一个矩形。"""
    return (LB_ROOMS_X, LB_ROOMS_Y + index * LB_ROOM_ROW_H, LB_ROOMS_W, LB_ROOM_ROW_H)


def rect_contains(rect: tuple[int, int, int, int], pos: tuple[float, float]) -> bool:
    x, y, w, h = rect
    return x <= pos[0] < x + w and y <= pos[1] < y + h


def scaled_size(scale: float) -> tuple[int, int]:
    return max(1, int(round(LOGICAL_W * scale))), max(1, int(round(LOGICAL_H * scale)))


def compute_scale(win_w: int, win_h: int) -> float:
    """§1.1 小屏策略：`scale = min((sw-80)/1280, (sh-80)/720, 1.0)`。"""
    return min((win_w - 80) / LOGICAL_W, (win_h - 80) / LOGICAL_H, 1.0)
