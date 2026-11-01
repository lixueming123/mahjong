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
import math

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
BACK = (214, 74, 142)          # v3.0：牌背改成参考视频的洋红（原来是蓝色）
BACK_EDGE = (136, 30, 86)
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

# 结算播报（RULINGS v1.2 §2.3）：**只新增这 4 个 token，既有配色一个都不改**
# （涨跌色仍为得分正 POS 绿 / 负 NEG 红，见 `score_color`）。
#: 播报期舞台底（盖住四卡 / 战绩块 / 手牌所在区域）
BC_ZONE = (14, 22, 20, 150)
#: 当前高亮行的底色（α40，非 30：对比度 5.5:1）
BC_ACTIVE_BG = (255, 200, 64, 40)
#: 字幕条底
BC_CAPTION_BG = (0, 0, 0, 120)
#: 封顶格的「被砍」填充
BC_CAP_FILL = (255, 200, 64, 40)

# ---------------------------------------------------------------- v2.1 视觉升级 token
# 对标腾讯麻将的质感层（`art.py`）。**都是新增 token，上表 30 个既有色值一个都不改**，
# 牌面/文字仍走 FACE / FACE_EDGE / SUIT_*，这里的色只用于光影与底板。
FELT_CORE = (30, 118, 78)         # 台面受光中心
FELT_EDGE = (11, 54, 38)          # 台面四周压暗
FELT_RAIL = (6, 24, 18)           # 台边收边
#: 牌身厚度（底沿）：比 FACE 略深，制造 3D 立牌感
TILE_SIDE = (236, 226, 216)
#: 牌面顶部高光
TILE_HI = (255, 253, 247)
#: 座位牌 / 托盘底板（半透明深色玻璃）
PLATE = (10, 28, 22, 196)
PLATE_EDGE = (74, 118, 96)
#: 中央情报面板的深色玻璃
GLASS = (9, 30, 24, 206)
#: 金色强调（台边线 / 庄家 / 选中态装饰），与 DEALER/BTN_WIN 同色系但更亮
GOLD = (232, 192, 104)
#: 木质台边（上亮下暗，配木纹线）
WOOD_HI = (120, 78, 48)
WOOD_LO = (62, 38, 24)

# ---------------------------------------------------------------- v3.0 桌布（复刻参考视频）
# 参考视频的桌面是一整块粉色绒布：上浅下深、上面压着米色斜格线、还撒着浅蓝线稿图案。
FELT_PINK_TOP = (214, 132, 146)
FELT_PINK_BOTTOM = (188, 100, 118)
#: 斜格线（米白，压在桌布上）
FELT_LATTICE = (246, 208, 200)
#: 桌面上的浅蓝线稿图案
FELT_DOODLE = (172, 200, 232)
#: 名牌上的分数条（洋红底 + 金色数字，和牌背同色系）
PLATE_SCORE = (206, 52, 108)
#: 名牌名字条（半透明深色）
PLATE_NAME = (30, 26, 30, 186)
#: 背景（人物身后那张糊掉的室内照片）
HALL_WALL = (104, 62, 70)     # 暗紫褐：和桌面同色系，整屏像同一间屋子
HALL_DARK = (52, 30, 36)

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
    "BC_ZONE": BC_ZONE,
    "BC_ACTIVE_BG": BC_ACTIVE_BG,
    "BC_CAPTION_BG": BC_CAPTION_BG,
    "BC_CAP_FILL": BC_CAP_FILL,
    "FELT_CORE": FELT_CORE,
    "FELT_EDGE": FELT_EDGE,
    "FELT_RAIL": FELT_RAIL,
    "TILE_SIDE": TILE_SIDE,
    "TILE_HI": TILE_HI,
    "PLATE": PLATE,
    "PLATE_EDGE": PLATE_EDGE,
    "GLASS": GLASS,
    "GOLD": GOLD,
    "WOOD_HI": WOOD_HI,
    "WOOD_LO": WOOD_LO,
    "FELT_PINK_TOP": FELT_PINK_TOP,
    "FELT_PINK_BOTTOM": FELT_PINK_BOTTOM,
    "FELT_LATTICE": FELT_LATTICE,
    "FELT_DOODLE": FELT_DOODLE,
    "PLATE_SCORE": PLATE_SCORE,
    "HALL_WALL": HALL_WALL,
    "HALL_DARK": HALL_DARK,
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
# v4.6：密码标签行右端的「记住密码」勾选框。只占标签行右侧空白（右边界仍是 848），
# 因此卡片高度、主按钮、注册按钮、状态行等既有坐标一个都不用动。
L_REMEMBER = (700, 390, 148, 24)
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
# v3.0：战绩/菜单入口移到桌面右下角（R_MENU_BTN 里），只在账号会话显示。
# 名字保留 `R_HUD_STATS_BTN`，因为 `app.py::_table_click` 已经按这个名字做命中测试。
R_HUD_STATS_BTN = (20, 614, 208, 22)

# ---------------------------------------------------------------- v2.5 牌桌构图（重排）
# 从「整屏绿色矩形 + 贴边摆放的牌」改成「房间里的实体牌桌」：
#   房间底（art.room）→ 椭圆木边牌桌（art.table_surface）→ 四家人物立绘 + 名牌 → 桌面上的牌。
# 牌尺寸、河流容量、点击/hit-test 规则不变；元素矩形整体重排。

# v2.0（§4.6）：他家手牌背容量 14 → 18（4 组全杠时 hand_count = 18）；横排 15..18 张换行，
# 竖排（上/下家）沿列向下堆叠，不裁剪、不缩小。
OPP_BACK_MAX = 18
OPP_BACK_ROW_CAPACITY = 14

# v3.3 参考视频《明星三缺一 2004》**按测量值复原**
#
# 参考的游戏窗口内容 = 778×552。这里的每个坐标都写成「参考像素 × S + 偏移」，
# S = 720/552 = 1.3043（纵向铺满），水平偏移 = (1280 − 778×S)/2 = 133。
# 桌布另行向左右铺到画面外，所以不影响这套映射。
#
# 参考实测（窗口坐标）：
#   桌沿 y=126 │ 对家手牌背 x230..503 y120..146（13 张 pitch21 牌 20×26）
#   对家牌墙 y147..174（pitch13）│ 对家副露 y176..200 │ 对家牌河 x300..500 y205..300
#   上家手牌 x0..22（竖排 pitch16）│ 上家牌墙 x24..40 │ 上家牌河 x70..130
#   本家牌河 y310..400 │ 本家牌墙 y385..404 │ 本家手牌 x99..559 y405..446（牌 32×34 pitch35.5）
S = 1.3043
OX = 133

TABLE_FELT = (0, 110, 1280, 610)      # 去掉人物后把桌沿上提，顶部只留名牌带
TABLE_FAR_HALF = 470          # 远沿半宽：桌面是**梯形**，两侧边可见并收向远处
TABLE_NEAR_HALF = 700         # 近沿半宽：铺到画面外
TABLE_RIM = 0
TABLE_RADIUS = 0

# --- 名牌：参考里贴在画面最上沿，宽约 190 → 248 ---
R_OPP_NAME = (516, 10, 248, 46)
R_SIDE_L_NAME = (196, 10, 248, 46)
R_SIDE_R_NAME = (836, 10, 248, 46)
PLATE_NAME_H = 24

# --- 对家：手牌背 → 牌墙 → 副露 → 牌河 ---
R_OPP_BACK = (430, 124, 420, 44)
R_OPP_WALL = (398, 172, 484, 32)
R_OPP_MELD = (440, 210, 400, 32)
R_OPP_RIVER = (505, 248, 268, 126)

# --- 桌心横带（听牌条）---
R_CENTER = (486, 364, 308, 18)
R_CENTER_LINE1 = (486, 364, 308, 15)
R_CENTER_LINE2 = (486, 382, 308, 15)

# --- 本家：牌河 → 牌墙 → 手牌 ---
R_SELF_RIVER = (505, 402, 268, 126)
R_SELF_WALL = (398, 532, 484, 32)   # 牌河底 528 与手牌顶 566 之间，两边留空
R_SELF_MELD = (150, 566, 560, 46)
R_SELF_HAND = (248, 566, 980, 56)   # 左边让给本家分数牌

# 侧家几何在牌河尺寸定义之后统一推导（见文件下方 SIDE_* 段）

# --- 本家分数牌 / 系统菜单 ---
R_SELF_PLATE = (16, 560, 216, 52)
R_MENU_BTN = (16, 616, 216, 28)

R_SIDE_L = (8, 120, 300, 520)
R_SIDE_R = (972, 120, 300, 520)

# 牌元素几何（参考值 × 1.3043）
RIVER_CENTER_TILE = (40, 46)
RIVER_CENTER_COL_PITCH = 42
RIVER_CENTER_ROW_PITCH = 47
RIVER_CENTER_COLS = 6
RIVER_CENTER_ROWS = 3
RIVER_CENTER_CAPACITY = RIVER_CENTER_COLS * RIVER_CENTER_ROWS
RIVER_CENTER_COMPACT_TILE = (28, 32)
RIVER_CENTER_COMPACT_COL_PITCH = 30
RIVER_CENTER_COMPACT_ROW_PITCH = 34
RIVER_CENTER_COMPACT_COLS = 7
RIVER_CENTER_COMPACT_ROWS = 3

R_TOAST = (440, 398, 400, 24)

R_TENPAI = (240, 362, 800, 32)
R_TENPAI_SHIFTED = R_TENPAI
TENPAI_BG_ALPHA = 216
TENPAI_ITEM_X0 = 256
TENPAI_ITEM_GAP = 16
TENPAI_ITEM_WIDTH_EST = 74
TENPAI_PREFIX_GAP = 8
TENPAI_TILE_W = 24
TENPAI_TILE_H = 30

# 牌元素几何（§4.4）
HAND_TILE_W = 48      # CC0 牌面 3:4，按高度 60 时宽约 45
HAND_TILE_H = 60
HAND_PITCH = 48
HAND_Y = 566
HAND_SELECT_LIFT = 8
HAND_DRAWN_LIFT = 4

# 中央/侧家牌河的**生效值**（v3.0）：桌面纵向只有 224 px，两条河各占 3 行。
RIVER_CENTER_TILE = (28, 34)
RIVER_CENTER_COL_PITCH = 30
RIVER_CENTER_ROW_PITCH = 40
RIVER_CENTER_COLS = 6
RIVER_CENTER_ROWS = 3
RIVER_CENTER_CAPACITY = RIVER_CENTER_COLS * RIVER_CENTER_ROWS  # 18 = 一局最多弃牌数
RIVER_CENTER_COMPACT_TILE = (24, 26)
RIVER_CENTER_COMPACT_COL_PITCH = 26
RIVER_CENTER_COMPACT_ROW_PITCH = 30
RIVER_CENTER_COMPACT_COLS = 7
RIVER_CENTER_COMPACT_ROWS = 3

RIVER_SIDE_TILE = (40, 30)      # 侧家弃牌：牌顶朝桌心，沿桌边横排
RIVER_SIDE_COL_PITCH = 36       # 沿轨方向槽距估算（碰撞盒用）
RIVER_SIDE_ROW_PITCH = 40       # 向桌心叠行间距
RIVER_SIDE_COLS = 6             # 横排 6 张/行
RIVER_SIDE_ROWS = 3             # 3 行 × 6 = 18

# --- 侧家轨：贴桌布左右边线，近端固定，左右镜像 ---
# 近端靠近本家手牌，远端收到对家手牌下沿；再向桌心内偏，避免画出梯形外。
SIDE_HAND_NEAR_Y = 520
SIDE_HAND_FAR_Y = 175
SIDE_HAND_INSET = 30
SIDE_WALL_INSET = 128          # 和手牌隔开一截，不再贴在橙色牌墙上
SIDE_RIVER_INSET = SIDE_HAND_INSET + 170
#: 副露在牌山内侧、牌河外侧（与手牌/牌山同一套 ``side_rail_points`` 桌边 y）
SIDE_MELD_INSET = SIDE_WALL_INSET + 58
#: 副露沿轨的桌边 y 段（``side_rail_points`` 的 far/near，不是屏幕 y）
SIDE_MELD_EDGE_FAR = 228
SIDE_MELD_EDGE_NEAR = 298
SIDE_MELD_GROUP_BANDS = 4    # 最多 4 组，沿轨分段
#: 侧家牌河「远端列」的屏幕 y（``side_point_at_screen_y``）；须高于副露投影 y
SIDE_RIVER_TOP = 392
SIDE_HAND_FULL = 13
#: 侧家手牌在槽位里占多长（<1 留出缝，不要整条贴图撑满）
SIDE_HAND_TILE_FILL = 0.86
#: 摊牌单张的兜底尺寸（沿轨长 × 朝桌心的厚）。实际长度按槽距取整。
SIDE_TILE_ALONG = 34
SIDE_TILE_ACROSS = 26
#: 每家牌山最多叠数（开局 remaining/4 ≈ 21）
WALL_PER_SIDE = 21

SIDE_MELD_W = 160
SIDE_MELD_H = 142
SIDE_RIVER_W = RIVER_SIDE_ROWS * RIVER_SIDE_ROW_PITCH + 48
SIDE_RIVER_H = int(HAND_Y - SIDE_RIVER_TOP - 16)
SIDE_RIVER_X = 0   # 由 side_river_rect 覆盖；保留名字避免旧引用
SIDE_RIVER_Y = 382


def table_edge_x(y: float, left: bool) -> float:
    """桌布左/右边线在屏幕 y 上的 x。远沿收、近沿铺出画面。"""
    top = float(TABLE_FELT[1])
    span = max(1.0, float(TABLE_FELT[3]))
    t = (y - top) / span
    cx = LOGICAL_W / 2.0
    far = float(TABLE_FAR_HALF)
    near = float(TABLE_NEAR_HALF)
    if left:
        return (cx - far) + ((cx - near) - (cx - far)) * t
    return (cx + far) + ((cx + near) - (cx + far)) * t


def side_inward(left: bool) -> tuple[float, float]:
    """从桌沿指向桌心的单位向量（屏幕坐标，y 向下）。"""
    y0 = float(SIDE_HAND_FAR_Y)
    y1 = float(SIDE_HAND_NEAR_Y)
    x0 = table_edge_x(y0, left)
    x1 = table_edge_x(y1, left)
    dx = x1 - x0
    dy = y1 - y0
    length = math.hypot(dx, dy) or 1.0
    n1 = (-dy / length, dx / length)
    n2 = (dy / length, -dx / length)
    toward = LOGICAL_W / 2.0 - (x0 + x1) / 2.0
    if toward * n1[0] >= toward * n2[0]:
        return n1
    return n2


def side_face_angle(left: bool) -> float:
    """扣着的立体牌墙：pygame 逆时针角度，牌顶转向桌心。"""
    ix, iy = side_inward(left)
    return math.degrees(math.atan2(-ix, -iy))


def side_rail_points(
    left: bool,
    inset: float,
    count: int,
    full: int = SIDE_HAND_FULL,
    from_far: bool = False,
    *,
    rail_far_y: float | None = None,
    rail_near_y: float | None = None,
) -> list[tuple[float, float]]:
    """侧边轨上的槽位中心。

    默认下标 0 是近端（靠近本家），张数少时远端留空。
    `from_far` 时下标 0 是远端，张数少时近端留空，短牌山不会缩到名牌上。
    ``rail_far_y`` / ``rail_near_y`` 为桌布边线上的 y（与 ``SIDE_HAND_*_Y`` 同系）。
    """
    if count <= 0:
        return []
    inward = side_inward(left)

    def at(y: float) -> tuple[float, float]:
        return (
            table_edge_x(y, left) + inward[0] * inset,
            y + inward[1] * inset,
        )

    near_y = float(SIDE_HAND_NEAR_Y if rail_near_y is None else rail_near_y)
    far_y = float(SIDE_HAND_FAR_Y if rail_far_y is None else rail_far_y)
    near = at(near_y)
    far = at(far_y)
    span = full if count <= full else count
    if span <= 1:
        return [far if from_far else near]
    full_points: list[tuple[float, float]] = []
    for index in range(span):
        t = index / (span - 1)
        full_points.append((
            near[0] + (far[0] - near[0]) * t,
            near[1] + (far[1] - near[1]) * t,
        ))
    if count >= span:
        chosen = full_points
    elif from_far:
        chosen = list(reversed(full_points[-count:]))
    else:
        chosen = full_points[:count]
    return chosen


def side_point_at_screen_y(left: bool, inset: float, screen_y: float) -> tuple[float, float]:
    """侧边轨在某个屏幕 y 上的点。"""
    inward = side_inward(left)
    y_edge = screen_y - inward[1] * inset
    x = table_edge_x(y_edge, left) + inward[0] * inset
    return (x, screen_y)


def side_meld_rect(left: bool) -> tuple[int, int, int, int]:
    """副露区外接盒（播报碰撞）；与 ``side_meld_group_centers`` 轨道段一致。"""
    pts = side_meld_group_centers(left, 0, 3, 1)
    xs = [point[0] for point in pts]
    ys = [point[1] for point in pts]
    pad = 36
    x0 = math.floor(min(xs) - pad)
    y0 = math.floor(min(ys) - pad)
    x1 = math.ceil(max(xs) + pad)
    y1 = math.ceil(max(ys) + pad)
    if left:
        return (x0, y0, x1 - x0, y1 - y0)
    return (x0, y0, x1 - x0, y1 - y0)


def side_meld_group_centers(
    left: bool,
    group_index: int,
    tile_count: int,
    group_count: int,
) -> list[tuple[float, float]]:
    """单组副露：``tile_count`` 张沿桌边横排（与手牌/牌山同轨）。"""
    if tile_count <= 0:
        return []
    bands = max(1, min(int(SIDE_MELD_GROUP_BANDS), group_count))
    edge_far = float(SIDE_MELD_EDGE_FAR)
    edge_near = float(SIDE_MELD_EDGE_NEAR)
    band = (edge_near - edge_far) / float(bands)
    seg_far = edge_far + float(group_index) * band
    seg_near = seg_far + band * 0.92
    span = max(tile_count, 3)
    return side_rail_points(
        left,
        float(SIDE_MELD_INSET),
        tile_count,
        span,
        from_far=True,
        rail_far_y=seg_far,
        rail_near_y=seg_near,
    )


def _mirror_x(rect: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    return (LOGICAL_W - rect[0] - rect[2], rect[1], rect[2], rect[3])


def _rail_bounds(left: bool, inset: float, count: int, pad: int) -> tuple[int, int, int, int]:
    points = side_rail_points(left, inset, count, count)
    xs = [point[0] for point in points]
    ys = [point[1] for point in points]
    x0 = math.floor(min(xs) - pad)
    y0 = math.floor(min(ys) - pad)
    x1 = math.ceil(max(xs) + pad)
    y1 = math.ceil(max(ys) + pad)
    return (x0, y0, x1 - x0, y1 - y0)


# 手牌 / 牌山 / 牌河外接盒（播报碰撞）；副露 ``R_SIDE_*_MELD`` 与对家横排同款
R_SIDE_L_BACK = _rail_bounds(True, SIDE_HAND_INSET, SIDE_HAND_FULL, 32)
R_SIDE_L_WALL = _rail_bounds(True, SIDE_WALL_INSET, WALL_PER_SIDE, 20)


def side_river_rect(left: bool) -> tuple[int, int, int, int]:
    """牌河在侧边中段，比副露更靠桌心。"""
    top = SIDE_RIVER_TOP
    x, _ = side_point_at_screen_y(left, SIDE_RIVER_INSET, float(top))
    if left:
        return (int(round(x)), top, SIDE_RIVER_W, SIDE_RIVER_H)
    return (int(round(x - SIDE_RIVER_W)), top, SIDE_RIVER_W, SIDE_RIVER_H)


#: 侧家平牌副露与牌山外沿的间距（与 ``side_flat_meld_rect``）
SIDE_MELD_WALL_GAP = 12
SIDE_MELD_Y = 218


def side_flat_meld_rect(left: bool) -> tuple[int, int, int, int]:
    """侧家副露：对家同款横排，宽与牌河列一致，整体在牌山外侧。"""
    wall = _rail_bounds(left, SIDE_WALL_INSET, WALL_PER_SIDE, 28)
    river = side_river_rect(left)
    height = R_OPP_MELD[3]
    width = river[2]
    gap = SIDE_MELD_WALL_GAP
    if left:
        x0 = wall[0] + wall[2] + gap
        return (int(x0), SIDE_MELD_Y, width, height)
    x0 = wall[0] - gap - width
    return (int(x0), SIDE_MELD_Y, width, height)


R_SIDE_L_RIVER = side_river_rect(True)
R_SIDE_L_MELD = side_flat_meld_rect(True)

R_SIDE_R_BACK = _mirror_x(R_SIDE_L_BACK)
R_SIDE_R_WALL = _mirror_x(R_SIDE_L_WALL)
R_SIDE_R_RIVER = side_river_rect(False)
R_SIDE_R_MELD = side_flat_meld_rect(False)

MELD_TILE = (38, 46)
MELD_TILE_PITCH = 30
MELD_GROUP_GAP = 8
MELD_ROW_HEIGHT = 40

OPP_BACK_TILE = (30, 42)      # 参考 20×26：牌背是**窄而高**的立牌
OPP_BACK_PITCH = 31
#: 牌山（v3.5）：**一叠 = 2 张牌上下叠在一起**，横着码成一条。
#: 俯视只看得见上面那张的牌顶（一条亮面）+ 两张牌背之间的一道缝，所以画成
#: 「亮顶 + 上张牌背 + 缝 + 下张牌背」，高度约等于 2 张牌背。
WALL_STACK = (22, 32)
WALL_STACK_PITCH = 23
WALL_STACK_PITCH_V = 24        # 竖排（上/下家）时沿列方向的间距
#: 亮顶 / 上张牌背 / 下张牌背的分界（相对叠顶，按高度比例）
WALL_TOP_RATIO = 0.17
WALL_SEAM_RATIO = 0.58

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
FS_MICRO = 12        # B 批（FEEL_SPEC §2.2 F-TP-03）：听牌条「N 枚」的枚数字号
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


def score_color(delta: int | None) -> tuple:
    """涨跌色（RULINGS C10 v1.1 撤回整页改色）：**取值就是页面现状**。

    得分正 = `POS` 绿、负 = `NEG` 红、0（或缺失）= `ZERO_SCORE` 灰。
    收敛成一个函数只为日后一处切换（U3 待用户拍板），当前行为与 v2.0 逐像素一致。
    """
    if not isinstance(delta, int) or delta == 0:
        return ZERO_SCORE
    return POS if delta > 0 else NEG


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
