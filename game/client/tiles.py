"""牌编码与排序（PROTOCOL.md §2 / RULESET §1.2 / UI_UX_SPEC v1.3 §1.3，H10）。

客户端与服务端必须完全一致的纯函数集合：

- 编码（PROTOCOL v1.18 §2.1）：数牌 `^[1-9][mps]$`、字牌 `^[1-7]z$`、赤 5 `^0[mps]$`；
  合取 `^((0|[1-9])[mps]|[1-7]z)$`；
  `m`=万、`p`=筒、`s`=条、`z`=字牌（`1z..7z` = 東/南/西/北/白/發/中）；
- `0m`/`0p`/`0s` 是**日麻赤宝牌**（5 的第 4 张）：牌面仍是 5，排序按 5，只是渲染成红色、
  计分时每张 +1 番。classic 房永不下发这三种牌码；
- 排序键（§2.2）：`(suit_index, rank)`，`m=0 < p=1 < s=2 < z=3`；数牌 rank 1..9 升序、
  字牌 `1z..7z` 升序 ⇒ **数牌在前、字牌在后**；
- 字牌**不可成顺**（只能构成刻子/杠），故客户端不生成也不会渲染字牌顺子；
- 客户端 **只**在「即时事件」（`draw` 插入）与「被动移除」时按本规则处理，
  对服务端下发的 `hand[]` / `melds[].tiles[]` **不重新排序**（PROTOCOL.md §2.2）。

字形口径（UI_UX_SPEC v1.3 §1.3 规则 4）：`1z..7z` 的**规范名**用简体
（东/南/西/北/白/发/中，与 PROTOCOL/RULESET 映射表逐字一致），**牌面字形冻结为繁體**
（東/南/西/北/白/發/中）。本模块只提供字形表，绘制在 `render/widgets.py`。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

_game_root = next(
    p for p in Path(__file__).resolve().parents
    if (p / "protocol" / "enums.json").is_file()
)
sys.path.insert(0, str(_game_root))
from protocol.enums import ACTION

NUMBER_SUITS = "mps"          # 数牌花色（排序序 m < p < s）；字牌见 HONOR_SUIT
HONOR_SUIT = "z"
#: 花色排序序：数牌 m<p<s，字牌 z 最大（PROTOCOL §2.2）
SUIT_INDEX = {"m": 0, "p": 1, "s": 2, "z": 3}
SUIT_LABEL = {"m": "万", "p": "筒", "s": "条"}
#: 字牌規範名（简体，用于日志/代码注释；与 PROTOCOL §2.1 映射表一致）
HONOR_NAME = {"1": "东", "2": "南", "3": "西", "4": "北", "5": "白", "6": "发", "7": "中"}
#: 字牌**牌面字形**（繁體，UI_UX_SPEC v1.3 §1.3 规则 4 / §4.4 规则 2 冻结）
HONOR_GLYPH = {"1": "東", "2": "南", "3": "西", "4": "北", "5": "白", "6": "發", "7": "中"}

#: 合取正则（PROTOCOL v1.18 §2.1）：数牌 + 字牌 + 赤 5
#: `0m` / `0p` / `0s` = 赤宝牌（5 的第 4 张），**牌面仍是 5**；只有日麻房会下发。
TILE_RE = re.compile(r"^((0|[1-9])[mps]|[1-7]z)$")
_INVALID_KEY = (len(SUIT_INDEX) + 1, 0)  # 非法牌面排在最后（不抛异常）


def is_valid_tile(tile: object) -> bool:
    return isinstance(tile, str) and TILE_RE.match(tile) is not None


def is_red_five(tile: object) -> bool:
    """赤宝牌判定（`0m` / `0p` / `0s`）。"""
    return isinstance(tile, str) and len(tile) == 2 and tile[0] == "0" and tile[1] in NUMBER_SUITS


def tile_face(tile: str) -> str:
    """把赤 5 归一到普通牌面（`0m` → `5m`），其余原样返回。

    选牌/扣牌一律按**线上牌码**匹配（赤 5 与普通 5 是两张不同的牌），
    只有「画什么图案」「排序按几」这类**牌面语义**才用它。
    """
    if is_red_five(tile):
        return "5" + tile[1]
    return tile


def is_honor(tile: object) -> bool:
    """字牌判定：`1z`..`7z`。"""
    return isinstance(tile, str) and len(tile) == 2 and tile[1] == HONOR_SUIT and tile[0] in HONOR_GLYPH


def honor_glyph(tile: object) -> str | None:
    """字牌的繁體牌面字形；非字牌返回 None。"""
    if not is_honor(tile):
        return None
    return HONOR_GLYPH[str(tile)[0]]


def honor_name(tile: object) -> str | None:
    """字牌的规范名（简体）；非字牌返回 None。"""
    if not is_honor(tile):
        return None
    return HONOR_NAME[str(tile)[0]]


def tile_key(tile: str) -> tuple:
    """排序键（PROTOCOL §2.2）：数牌在前、字牌在后，字牌内 1z..7z 升序。

    赤 5 按**牌面 5** 排序（与服务端「同面副本相邻」一致：赤牌是 5 的第 4 张，排在同面最后）。
    非法牌面返回 `_INVALID_KEY`（排到最后，保证不抛异常）。
    """
    if not is_valid_tile(tile):
        return _INVALID_KEY
    # 第三位把赤 5 排在同面最后：服务端 SortTiles 按 (face, copy)，赤牌是 copy 3。
    return (SUIT_INDEX[tile[1]], int(tile_face(tile)[0]), 1 if is_red_five(tile) else 0)


def sort_tiles(tiles: list[str]) -> list[str]:
    return sorted(tiles, key=tile_key)


def tile_rank(tile: str) -> str:
    return tile_face(tile)[0] if len(tile) == 2 else "?"


def tile_suit_label(tile: str) -> str:
    """数牌花色字；字牌返回其繁體字形（无花色角标语义）。"""
    if is_honor(tile):
        return HONOR_GLYPH[str(tile)[0]]
    if len(tile) == 2 and tile[1] in SUIT_LABEL:
        return SUIT_LABEL[tile[1]]
    return "?"


def tile_label(tile: str) -> str:
    """**中文牌名**（UI_UX_SPEC v1.4 §5.3「按钮文案公式」冻结）。

    - 数牌 `1m`..`9s` → `"<rank> <花色字>"`（阿拉伯数字 + 花色汉字），例 `2s` → `"2 条"`；
    - 字牌 `1z`..`7z` → **繁體字形**（与牌面字形一致，§1.3 规则 4），例 `1z` → `"東"`；
    - 非法牌面 → `"?"`（防御性，不抛异常；调用方只在服务端 `options` 内取值，正常不会出现）。

    注意：该函数**只用于显示文案**。出站帧的 `tiles` 字段始终是线上牌码
    （`9s`/`3p`/`1z`），显示与载荷互不影响。
    """
    if is_honor(tile):
        return HONOR_GLYPH[str(tile)[0]]
    face = tile_face(tile) if isinstance(tile, str) else tile
    if isinstance(face, str) and len(face) == 2 and face[0] in "123456789" and face[1] in SUIT_LABEL:
        label = f"{face[0]} {SUIT_LABEL[face[1]]}"
        return f"赤 {label}" if is_red_five(tile) else label
    return "?"


def button_label(action: int, tiles: list[str] | None = None) -> str:
    """操作栏按钮与「已提交：<动作>」状态提示的**统一文案**（UI_UX_SPEC v1.4 §5.3）。

    单一真相源：`render/actionbar.py`（按钮）与 `app.py`（已提交提示）都调用本函数，
    避免同一公式在两处各写一遍而漂移。`action` 为动作整数常量，返回中文显示文案。

    - `kong` → `"杠 <T>"`；`pong` → `"碰 <T>"`；
    - `chow` → `"吃 <a> <b>"`（**只列手中消耗的两张**，不含被鸣牌，见 §4.7）；
    - `win`/`pass`/`riichi` → 固定文案「胡」「过」「立直」。
    """
    tiles = list(tiles or [])
    if action == ACTION.KONG:
        return f"杠 {tile_label(tiles[0]) if tiles else '?'}"
    if action == ACTION.ANKAN:
        # v2.0（§5.3）：暗杠文案**不显示牌名**——4 张本就在本家手牌里，发牌名会泄露给旁观录屏
        return "暗杠"
    if action == ACTION.KAKAN:
        # v2.0（§5.3）：加杠显示「加杠 <牌名>」（T = 手中那一张）
        return f"加杠 {tile_label(tiles[0]) if tiles else '?'}"
    if action == ACTION.PONG:
        return f"碰 {tile_label(tiles[0]) if tiles else '?'}"
    if action == ACTION.CHOW:
        names = [tile_label(t) for t in tiles[:2]]
        return "吃 " + " ".join(names) if names else "吃"
    return {
        ACTION.WIN: "胡", ACTION.PASS: "过", ACTION.RIICHI: "立直",
    }.get(action, str(action))


# 日麻（mode=riichi）专用番种中文表。放在 tiles.py 是为了让**逻辑层**（broadcast.py）
# 与**渲染层**（render/result.py）共用同一份表，避免两处各写一遍再漂移。
# `yakuhai` 在日麻里含风牌，故覆盖成「役牌」而不是「箭刻」。
RIICHI_YAKU_LABEL = {
    "riichi": "立直",
    "ippatsu": "一发",
    "tsumo": "门前清自摸",
    "chankan": "抢杠",
    "tenhou": "天和",
    "chiihou": "地和",
    "haitei": "海底摸月",
    "houtei": "河底捞鱼",
    "chanta": "混全带幺九",
    "junchan": "纯全带幺九",
    "ryanpeikou": "二杯口",
    "sankantsu": "三杠子",
    "honroutou": "混老头",
    "chiitoitsu": "七对子",
    "chinitsu": "清一色",
    "yakuhai": "役牌",
    "dora": "宝牌",
    "ura_dora": "里宝牌",
    "aka_dora": "赤宝牌",
    "kokushi": "国士无双",
    "kokushi_13": "国士十三面",
    "suuankou": "四暗刻",
    "suuankou_tanki": "四暗刻单骑",
    "daisangen": "大三元",
    "tsuuiisou": "字一色",
    "ryuuiisou": "绿一色",
    "chinroutou": "清老头",
    "chuuren": "九莲宝灯",
    "chuuren_junsei": "纯正九莲宝灯",
    "suukantsu": "四杠子",
    "shousuushii": "小四喜",
    "daisuushii": "大四喜",
}

# 满贯以上的中文分档（日麻结算行尾）。
LIMIT_LABEL = {
    "mangan": "满贯",
    "haneman": "跳满",
    "baiman": "倍满",
    "sanbaiman": "三倍满",
    "yakuman": "役满",
}


def remove_leftmost(tiles: list[str], face: str) -> bool:
    """从 `tiles` 中移除**最左**一张与 `face` 同牌面的牌（副本不可区分）。

    返回是否成功移除；牌面不在列表时返回 False（调用方自行决定是否记 WARN）。
    """
    for i, t in enumerate(tiles):
        if t == face:
            del tiles[i]
            return True
    return False


def multiset_remove(pool: list[str], tiles: list[str]) -> bool:
    """从 `pool` 中按多重集扣掉 `tiles`（扣最左匹配项，保证确定性）。

    对字牌与数牌一视同仁（结算页 `hand` 与 `melds[]` 的还原依赖它）。
    """
    working = list(pool)
    removed_idx: list[int] = []
    for want in tiles:
        found = None
        for i, have in enumerate(working):
            if i in removed_idx:
                continue
            if have == want:
                found = i
                break
        if found is None:
            return False
        removed_idx.append(found)
    for i in sorted(removed_idx, reverse=True):
        del pool[i]
    return True
