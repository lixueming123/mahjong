#!/usr/bin/env python3
"""天凤牌谱（mjlog XML）解析器：把一局半庄拆成「每局的配牌 + 摸牌序列 + 动作 + 结算」。

只做解析与**自校验**：不做抓取（`tenhou_fetch.py`）、不做判番（`tenhou_convert.py` + C++ 侧
`tenhou_rules_check`）。

## mjlog 的关键编码（本文件即真值来源，全部经真实牌谱核对）

- **牌 ID = 牌面 × 4 + 副本**；牌面 0..8 万 / 9..17 筒 / 18..26 索 / 27..33 东南西北白发中。
  赤 5 = 5m/5p/5s 的 `副本 == 0`，即 **16 / 52 / 88**（`AGARI.hai` 里这三个 ID 出现 ⇒ 赤牌）。
- 摸牌 `<T><U><V><W>`（座位 0..3）、弃牌 `<D><E><F><G>`，**ElementTree 把 `<T93/>` 的 tag 解成
  `"T93"`**，所以数值从 tag 尾部取（`_split_tag`）。弃牌值与摸牌同是裸 ID ⇒ 摸切/手切在编码上
  无区别，只能跟「这家上一次摸到的牌」比对。
- 副露 `<N who="s" m="v"/>` 的位布局见 `decode_meld`。
- `<AGARI hai=... machi=... yaku=...>`：`hai` = **门前手牌（不含副露）+ 和牌张**，`machi` = 和牌张，
  `yaku` = `[役ID, 番]` 平铺（含 52 宝牌 / 53 里宝 / 54 赤宝牌 三条「非役」条目），
  `doraHai` / `doraHaiUra` = **指示牌**（不是宝牌本身：46 手真实和牌按指示牌换算 46/46 一致，
  按宝牌读只有 40/46）。

## 自校验（错了就抛 `MjlogError`，绝不静默）

| 断言 | 抓住什么错误 |
| --- | --- |
| 弃牌在重构手牌里 | 摸打追踪错位 |
| 副露消耗的牌在重构手牌里 | 副露位解码错（吃/碰/杠的张数、from） |
| `sorted(重构手牌 + 和牌张) == sorted(AGARI.hai)` | 牌 ID 解释、赤牌、扣牌顺序 |
| `赤牌总数（门前 + 副露）== yaku 中 54 的番` | 赤 5 判定（16/52/88） |
| 副露的 `claimed` 等于该家最近一次弃牌 | `from_seat` 相对座位解码 |
"""

from __future__ import annotations

import gzip
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field

SUITS = "mps"

#: 赤 5 的牌 ID（5m/5p/5s 的副本 0）
AKA_IDS = frozenset({16, 52, 88})

#: 天凤 `yaku` 的役 ID（0..54）。52 宝牌 / 53 里宝 / 54 赤宝牌 不是役，列在这里只为了让
#: 映射表完整、并让「赤牌张数」这类校验有名字可用。
TENHOU_YAKU: dict[int, str] = {
    0: "tsumo", 1: "riichi", 2: "ippatsu", 3: "chankan", 4: "rinshan", 5: "haitei",
    6: "houtei", 7: "pinfu", 8: "tanyao", 9: "iipeikou",
    10: "seat_east", 11: "seat_south", 12: "seat_west", 13: "seat_north",
    14: "round_east", 15: "round_south", 16: "round_west", 17: "round_north",
    18: "haku", 19: "hatsu", 20: "chun", 21: "daburu_riichi", 22: "chiitoitsu",
    23: "chanta", 24: "ittsuu", 25: "sanshoku", 26: "sanshoku_doukou",
    27: "sankantsu", 28: "toitoi", 29: "sanankou", 30: "shousangen",
    31: "honroutou", 32: "ryanpeikou", 33: "junchan", 34: "honitsu", 35: "chinitsu",
    36: "renhou", 37: "tenhou", 38: "chiihou", 39: "daisangen", 40: "suuankou",
    41: "suuankou_tanki", 42: "tsuuiisou", 43: "ryuuiisou", 44: "chinroutou",
    45: "chuuren", 46: "chuuren_junsei", 47: "kokushi", 48: "kokushi_13",
    49: "daisuushii", 50: "shousuushii", 51: "suukantsu",
    52: "dora", 53: "ura_dora", 54: "aka_dora",
}

#: 天凤 `ten` 的第三个值：0 = 无（未满贯）/ 1 满贯 / 2 跳满 / 3 倍满 / 4 三倍满 / 5 役满
TENHOU_LIMITS = ("none", "mangan", "haneman", "baiman", "sanbaiman", "yakuman")


class MjlogError(Exception):
    """牌谱内容与已知编码不符（自校验失败）。"""


# ---------------------------------------------------------------------------
# 牌 ID ↔ 协议牌码
# ---------------------------------------------------------------------------
def face_of(tile_id: int) -> int:
    return tile_id // 4


def copy_of(tile_id: int) -> int:
    return tile_id % 4


def is_aka(tile_id: int) -> bool:
    return tile_id in AKA_IDS


def to_protocol_tile(tile_id: int, red_fives: bool = True) -> str:
    """天凤牌 ID → PROTOCOL §2.1 牌码。`red_fives=False` 时赤 5 也写 `5m`（classic 口径）。"""
    face = face_of(tile_id)
    if face >= 27:
        return f"{face - 26}z"
    suit = SUITS[face // 9]
    rank = face % 9 + 1
    if red_fives and is_aka(tile_id):
        return f"0{suit}"
    return f"{rank}{suit}"


def to_plain_tile(tile_id: int) -> str:
    """同 `to_protocol_tile`，但赤 5 也写成普通 `5m` / `5p` / `5s`（判「同面」时用）。"""
    return to_protocol_tile(tile_id, red_fives=False)


#: 指示牌 → 宝牌牌面（数牌 9→1、风 东南西北→南西北东、三元 白发中→发中白）
_NEXT_FACE: dict[int, int] = {}
for _base in (0, 9, 18):
    for _rank in range(8):
        _NEXT_FACE[_base + _rank] = _base + _rank + 1
    _NEXT_FACE[_base + 8] = _base
for _wind in range(27, 31):
    _NEXT_FACE[_wind] = 27 + (_wind - 27 + 1) % 4
for _dragon in range(31, 34):
    _NEXT_FACE[_dragon] = 31 + (_dragon - 31 + 1) % 3


def dora_face_of_indicator(tile_id: int) -> int:
    """指示牌 ID → 宝牌**牌面**（与 `riichi::DoraFaceFromIndicator` 同口径）。"""
    return _NEXT_FACE[face_of(tile_id)]


# ---------------------------------------------------------------------------
# 副露
# ---------------------------------------------------------------------------
CHI, PON, KAKAN, ANKAN, MINKAN, NUKI = "chi", "pon", "kakan", "ankan", "minkan", "nuki"
KAN_KINDS = (ANKAN, KAKAN, MINKAN)


@dataclass
class Meld:
    kind: str
    seat: int
    from_seat: int          # 被鸣者的绝对座位；暗杠 = 自己
    tiles: list[int]        # 4 张（杠）或 3 张，已排序
    from_hand: list[int]    # 这次副露从**手里**拿走的牌（被鸣的那张不在此列）
    claimed: int | None     # 被鸣的那张牌（暗杠为 None）

    @property
    def is_kan(self) -> bool:
        return self.kind in KAN_KINDS

    def to_json(self) -> dict:
        return {
            "kind": self.kind,
            "seat": self.seat,
            "from_seat": self.from_seat,
            "tiles": [to_protocol_tile(tile) for tile in self.tiles],
            "from_hand": [to_protocol_tile(tile) for tile in self.from_hand],
            "claimed": None if self.claimed is None else to_protocol_tile(self.claimed),
            "kan": self.is_kan,
        }


def _copies_missing(packed: int, shift: int) -> list[int]:
    """刻子类：`packed` 的 bit 里标出「三张牌用了哪些副本」，补出缺的那一张。

    天凤的编码是「4 张同面牌里挑 3 张」，`unused` 是被跳过的那张的副本号。
    """
    unused = (packed >> shift) & 0x3
    offsets = ((1, 2, 3), (0, 2, 3), (0, 1, 3), (0, 1, 2))[unused]
    return list(offsets)


def decode_meld(seat: int, value: int) -> Meld:
    """`<N who=seat m=value/>` → `Meld`。

    位布局（低 → 高，取自天凤 `tehai.js`，已用真实牌谱核对）：

        0..1  被鸣者的**相对**座位（暗杠 = 0 = 自己）
        2     吃
        3     碰
        4     加杠
        5     拔北（仅三麻）
        6..7  副本选择（吃/碰/杠：被跳过或刚加的那张）
        8..   牌面 / 顺子组号
    """
    callee_rel = value & 0x3
    from_seat = (seat + callee_rel) % 4

    if value & (1 << 2):  # 吃
        packed = (value & 0xFC00) >> 10
        rotation, group = packed % 3, packed // 3
        base = (9 * (group // 7) + (group % 7)) * 4
        tiles = [
            base + 0 + ((value & 0x0018) >> 3),
            base + 4 + ((value & 0x0060) >> 5),
            base + 8 + ((value & 0x0180) >> 7),
        ]
        if rotation == 1:
            tiles = [tiles[1], tiles[0], tiles[2]]
        elif rotation == 2:
            tiles = [tiles[2], tiles[0], tiles[1]]
        claimed = tiles[0]  # rotation 已把被鸣的那张换到首位
        return Meld(CHI, seat, from_seat, sorted(tiles), sorted(tiles[1:]), claimed)

    if value & (1 << 3):  # 碰
        packed = (value & 0xFE00) >> 9
        rotation, face = packed % 3, packed // 3
        offsets = _copies_missing(value, 5)
        tiles = [face * 4 + offset for offset in offsets]
        if rotation == 1:
            tiles = [tiles[1], tiles[0], tiles[2]]
        elif rotation == 2:
            tiles = [tiles[2], tiles[0], tiles[1]]
        claimed = tiles[0]
        return Meld(PON, seat, from_seat, sorted(tiles), sorted(tiles[1:]), claimed)

    if value & (1 << 4):  # 加杠：已有碰的那 3 张 + 手里第 4 张
        added = (value & 0x0060) >> 5
        packed = (value & 0xFE00) >> 9
        rotation, face = packed % 3, packed // 3
        base = face * 4
        others = [base + offset for offset in ((1, 2, 3), (0, 2, 3), (0, 1, 3), (0, 1, 2))[added]]
        added_tile = base + added
        # 前 3 张是已碰出的组（此刻已不在手里），第 4 张才是本次从手里取走的
        ordered = others
        if rotation == 1:
            ordered = [others[1], others[0], others[2]]
        elif rotation == 2:
            ordered = [others[2], others[0], others[1]]
        claimed = ordered[0]  # 原来被碰的那张（用于核对 from_seat）
        return Meld(KAKAN, seat, from_seat, sorted([added_tile] + others), [added_tile], claimed)

    if value & (1 << 5):  # 拔北（三麻）
        tile = (value & 0xFF00) >> 8
        return Meld(NUKI, seat, seat, [tile], [tile], None)

    # 暗杠 / 大明杠：4 张同面，代表牌在 8..15 位
    tile = (value & 0xFF00) >> 8
    face = face_of(tile)
    tiles = [face * 4 + copy for copy in range(4)]
    if callee_rel == 0:
        return Meld(ANKAN, seat, seat, tiles, tiles, None)
    return Meld(MINKAN, seat, from_seat, tiles, [t for t in tiles if t != tile], tile)


# ---------------------------------------------------------------------------
# 动作 / 结算 / 局
# ---------------------------------------------------------------------------
@dataclass
class Action:
    kind: str                  # draw / discard / meld / riichi / agari / ryuukyoku
    seat: int
    tile: str = ""
    tsumogiri: bool = False
    meld: Meld | None = None

    def to_json(self) -> dict:
        body: dict = {"kind": self.kind, "seat": self.seat}
        if self.tile:
            body["tile"] = self.tile
        if self.kind == "discard":
            body["tsumogiri"] = self.tsumogiri
        if self.meld is not None:
            body["meld"] = self.meld.to_json()
        return body


@dataclass
class WinResult:
    """`<AGARI>` 的结算载荷 —— 判番回归的**期望值**。"""

    seat: int
    from_seat: int
    winning_tile: str
    concealed: list[str]        # 门前手牌（含和牌张），赤 5 写作 0m/0p/0s
    melds: list[Meld]
    fu: int
    points: int                 # 和牌者总收入（荣和 = 放炮者付出；自摸 = 三家之和）
    limit: int                  # 见 TENHOU_LIMITS
    yaku: list[tuple[int, int]]
    yakuman: list[int]
    dora_indicators: list[int]  # 裸 ID，`dora_count()` 用
    ura_indicators: list[int]
    delta: list[int]            # 四家得失分（天凤 sc 的差分，单位点）

    @property
    def tsumo(self) -> bool:
        return self.from_seat == self.seat

    @property
    def han(self) -> int:
        """天凤给出的总番数（含宝牌里宝赤宝）。"""
        return sum(han for _, han in self.yaku)

    def yaku_han(self, tenhou_id: int) -> int:
        return sum(han for yaku_id, han in self.yaku if yaku_id == tenhou_id)

    def yaku_counts(self) -> dict[int, int]:
        """`{役ID: 番}`；`yaku` 里同一 ID 只出现一次，这里再合并一次以防万一。"""
        counts: dict[int, int] = {}
        for yaku_id, han in self.yaku:
            counts[yaku_id] = counts.get(yaku_id, 0) + han
        return counts

    def dora_count(self) -> int:
        """按指示牌换算的宝牌张数（校验天凤 52 番是否与我们口径一致）。"""
        return sum(self.concealed.count(_dora_code(ind)) for ind in self.dora_indicators)

    def to_json(self) -> dict:
        return {
            "seat": self.seat,
            "from_seat": self.from_seat,
            "tsumo": self.tsumo,
            "winning_tile": self.winning_tile,
            "concealed": self.concealed,
            "melds": [meld.to_json() for meld in self.melds],
            "fu": self.fu,
            "han": self.han,
            "points": self.points,
            "limit": TENHOU_LIMITS[self.limit] if 0 <= self.limit < len(TENHOU_LIMITS) else "none",
            "limit_code": self.limit,
            "yaku": [
                {"id": yaku_id, "name": TENHOU_YAKU.get(yaku_id, f"unknown_{yaku_id}"), "han": han}
                for yaku_id, han in self.yaku
            ],
            "yakuman": [TENHOU_YAKU.get(yaku_id, f"unknown_{yaku_id}") for yaku_id in self.yakuman],
            "dora_indicators": [to_protocol_tile(tile) for tile in self.dora_indicators],
            "ura_indicators": [to_protocol_tile(tile) for tile in self.ura_indicators],
            "delta": self.delta,
        }


def _dora_code(indicator_id: int) -> str:
    face = dora_face_of_indicator(indicator_id)
    return to_protocol_tile(face * 4)


@dataclass
class Round:
    index: int
    round_wind: int
    round_number: int
    honba: int
    kyoutaku: int
    dealer: int
    dora_indicators: list[str]
    points_before: list[int]
    hands: list[list[str]]          # 配牌（14/13 张，已排序）
    actions: list[Action] = field(default_factory=list)
    win: WinResult | None = None
    draw_tenpai: list[bool] = field(default_factory=list)
    draw_reason: str = ""

    def to_json(self) -> dict:
        return {
            "index": self.index,
            "round_wind": self.round_wind,
            "round_number": self.round_number,
            "honba": self.honba,
            "kyoutaku": self.kyoutaku,
            "dealer": self.dealer,
            "dora_indicators": self.dora_indicators,
            "points_before": self.points_before,
            "hands": self.hands,
            "actions": [action.to_json() for action in self.actions],
            "win": None if self.win is None else self.win.to_json(),
            "draw": None if self.win is not None else {
                "tenpai": self.draw_tenpai,
                "reason": self.draw_reason,
            },
        }


@dataclass
class Hand:
    log_id: str
    rules: dict
    players: list[str]
    rounds: list[Round]

    @property
    def is_four_player(self) -> bool:
        return not self.rules.get("sanma", False)

    def to_json(self) -> dict:
        return {
            "log_id": self.log_id,
            "rules": self.rules,
            "players": self.players,
            "rounds": [round_.to_json() for round_ in self.rounds],
        }


# ---------------------------------------------------------------------------
# 解析
# ---------------------------------------------------------------------------
def parse_go(value: int) -> dict:
    """`<GO type=N>` 的规则位（与 `mjlog2mjai` 同口径，185 = 四麻南喰赤有赤）。"""
    return {
        "test": not bool(value & 0x01),
        "red": not bool(value & 0x02),      # 位为 0 ⇒ 有赤宝牌
        "kuikae": not bool(value & 0x04),   # 位为 0 ⇒ 有喰い替え
        "tonnan": bool(value & 0x08),
        "sanma": bool(value & 0x10),
        "soku": bool(value & 0x40),
        "tokujou": bool(value & 0x20),
        "joukyu": bool(value & 0x80),
    }


def _ints(text: str | None) -> list[int]:
    return [int(part) for part in text.split(",")] if text else []


def _hundreds(text: str | None) -> list[int]:
    """`ten` / `sc` 以「百点」为单位（350 = 35000）。"""
    return [value * 100 for value in _ints(text)]


def load(path: str) -> ET.Element:
    """读一个 mjlog：`.xml` 明文 / `.mjlog`（其实是 gz）/ `.gz` 均可。"""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return ET.fromstring(data)


def _split_tag(tag: str) -> tuple[str, int]:
    """`<T93/>` 被 ElementTree 解成 tag `"T93"` ⇒ (`"T"`, 93)。"""
    head = len(tag.rstrip("0123456789"))
    return tag[:head], int(tag[head:]) if head < len(tag) else -1


DRAW_TAGS = {"T": 0, "U": 1, "V": 2, "W": 3}
DISCARD_TAGS = {"D": 0, "E": 1, "F": 2, "G": 3}


def parse_hand(root: ET.Element, log_id: str = "") -> Hand:
    """把 `<mjloggm>` 解析成 `Hand`；每条自校验失败都抛 `MjlogError`。"""
    rules: dict = {}
    players: list[str] = []
    rounds: list[Round] = []
    current: Round | None = None
    hands: list[list[int]] = []
    melds: list[list[Meld]] = []
    last_draw: dict[int, int] = {}
    last_discard: dict[int, int] = {}

    for node in root:
        tag = node.tag
        if tag == "GO":
            rules = parse_go(int(node.get("type", "0")))
            continue
        if tag == "UN":
            players = [node.get(f"n{seat}", "") for seat in range(4)]
            continue
        if tag == "INIT":
            seed = _ints(node.get("seed"))
            current = Round(
                index=len(rounds),
                round_wind=seed[0] // 4,
                round_number=seed[0] % 4 + 1,
                honba=seed[1],
                kyoutaku=seed[2],
                dealer=int(node.get("oya", "0")),
                dora_indicators=[to_protocol_tile(seed[5])],
                points_before=_hundreds(node.get("ten")),
                hands=[],
            )
            hands = [_ints(node.get(f"hai{seat}")) for seat in range(4)]
            melds = [[] for _ in range(4)]
            last_draw = {}
            last_discard = {}
            current.hands = [sorted(to_protocol_tile(tile) for tile in hand) for hand in hands]
            rounds.append(current)
            continue
        if current is None:
            continue

        if tag == "DORA":
            current.dora_indicators.append(to_protocol_tile(int(node.get("hai", "0"))))
            continue

        head, value = _split_tag(tag)
        if head in DRAW_TAGS:
            seat = DRAW_TAGS[head]
            hands[seat].append(value)
            last_draw[seat] = value
            current.actions.append(Action("draw", seat, to_protocol_tile(value)))
            continue
        if head in DISCARD_TAGS:
            seat = DISCARD_TAGS[head]
            _discard(current, hands, melds, last_draw, last_discard, seat, value)
            continue

        if tag == "N":
            seat = int(node.get("who", "0"))
            meld = decode_meld(seat, int(node.get("m", "0")))
            _apply_meld(current, hands, melds, last_discard, meld)
            continue

        if tag == "REACH":
            # step 1 = 宣言（紧接着会打出那张牌），step 2 = 成立（扣 1000 供托）
            if int(node.get("step", "1")) == 1:
                seat = int(node.get("who", "0"))
                current.actions.append(Action("riichi", seat))
            continue

        if tag == "AGARI":
            current.win = _parse_agari(node, melds)
            _verify_agari(current, melds)
            continue

        if tag == "RYUUKYOKU":
            current.draw_tenpai = [bool(node.get(f"hai{seat}")) for seat in range(4)]
            current.draw_reason = node.get("type", "")

    return Hand(log_id=log_id, rules=rules, players=players, rounds=rounds)


def _discard(round_: Round, hands: list[list[int]], melds: list[list[Meld]],
             last_draw: dict[int, int], last_discard: dict[int, int],
             seat: int, tile: int) -> None:
    if tile not in hands[seat]:
        raise MjlogError(
            f"round {round_.index}: seat {seat} discarded {to_protocol_tile(tile)} which is not "
            f"in the reconstructed hand "
            f"{sorted(to_protocol_tile(held) for held in hands[seat])}")
    tsumogiri = last_draw.get(seat) == tile
    hands[seat].remove(tile)
    last_draw.pop(seat, None)
    last_discard[seat] = tile
    round_.actions.append(Action("discard", seat, to_protocol_tile(tile), tsumogiri))


def _apply_meld(round_: Round, hands: list[list[int]], melds: list[list[Meld]],
                last_discard: dict[int, int], meld: Meld) -> None:
    if meld.kind == NUKI:
        if meld.tiles[0] not in hands[meld.seat]:
            raise MjlogError(f"round {round_.index}: nuki of a tile the hand does not hold")
        hands[meld.seat].remove(meld.tiles[0])
        round_.actions.append(Action("nuki", meld.seat, to_protocol_tile(meld.tiles[0])))
        return

    for tile in meld.from_hand:
        if tile not in hands[meld.seat]:
            raise MjlogError(
                f"round {round_.index}: meld {meld.kind} of seat {meld.seat} needs "
                f"{to_protocol_tile(tile)} but the hand is "
                f"{sorted(to_protocol_tile(held) for held in hands[meld.seat])}")
        hands[meld.seat].remove(tile)

    if meld.kind != NUKI and meld.kind != KAKAN:
        # 被鸣的那张必须是该家最近一次弃牌 ⇒ 校验 `from_seat`（相对座位）解对了。
        # 加杠不参与：它抢的是自己早先碰出的那组，`from_seat` 只记录当初是谁打的，
        # 那个弃牌早已不在 `last_discard` 里（真实的杠牌谱里必然对不上）。
        expected = last_discard.get(meld.from_seat)
        if meld.claimed is not None and meld.claimed != expected:
            raise MjlogError(
                f"round {round_.index}: meld {meld.kind} by seat {meld.seat} claims "
                f"{to_protocol_tile(meld.claimed)} but seat {meld.from_seat} last discarded "
                f"{'nothing' if expected is None else to_protocol_tile(expected)}")

    if meld.kind == KAKAN and _upgrade_kakan(round_, melds, meld):
        round_.actions.append(Action("meld", meld.seat, meld=meld))
        return

    melds[meld.seat].append(meld)
    round_.actions.append(Action("meld", meld.seat, meld=meld))


def _upgrade_kakan(round_: Round, melds: list[list[Meld]], meld: Meld) -> bool:
    """加杠**就地升级**已有的碰（组数不变）—— 与本仓库 `ApplyKakan` 同语义。

    牌的账目同理：加杠只从手里多拿走 1 张（`meld.from_hand`），原来那 3 张早已不在手里。
    找不到对应的碰（旧牌谱/异常记录）时返回 false，让调用方按「新增一组」处理。
    """
    for index, existing in enumerate(melds[meld.seat]):
        if existing.kind == PON and face_of(existing.tiles[0]) == face_of(meld.tiles[0]):
            melds[meld.seat][index] = meld
            return True
    return False


def _parse_agari(node: ET.Element, melds: list[list[Meld]]) -> WinResult:
    seat = int(node.get("who", "0"))
    ten = _ints(node.get("ten"))
    scores = _hundreds(node.get("sc"))
    flat = _ints(node.get("yaku"))
    return WinResult(
        seat=seat,
        from_seat=int(node.get("fromWho", "0")),
        winning_tile=to_protocol_tile(int(node.get("machi", "0"))),
        concealed=sorted(to_protocol_tile(tile) for tile in _ints(node.get("hai"))),
        melds=list(melds[seat]),
        fu=ten[0] if ten else 0,
        # `ten` 的第二个值已经是**点数**（如 7800），不像 `sc` 那样以百点为单位 —— 真实牌谱
        # 逐个核对过（`ten="20,7800,0"` 的 4 番 20 符自摸，收入就是 7800）。
        points=ten[1] if len(ten) > 1 else 0,
        limit=ten[2] if len(ten) > 2 else 0,
        yaku=list(zip(flat[::2], flat[1::2])),
        yakuman=_ints(node.get("yakuman")),
        dora_indicators=_ints(node.get("doraHai")),
        ura_indicators=_ints(node.get("doraHaiUra")),
        delta=[scores[index + 1] for index in range(0, len(scores), 2)],
    )


def _verify_agari(round_: Round, melds: list[list[Meld]]) -> None:
    """`AGARI.hai` == 「重构出门前手牌 + 和牌张」，且赤牌总数与天凤的赤宝牌番数一致。"""
    win = round_.win
    assert win is not None
    if not win.concealed:
        raise MjlogError(f"round {round_.index}: AGARI without hai")

    # 赤宝牌总数：门前手牌 + 副露里的 4 张杠/3 张组都要算
    meld_aka = sum(1 for meld in melds[win.seat] for tile in meld.tiles if is_aka(tile))
    hand_aka = sum(1 for code in win.concealed if code.startswith("0"))
    reported = win.yaku_han(54)
    if hand_aka + meld_aka != reported:
        raise MjlogError(
            f"round {round_.index}: aka count mismatch: hand+meld={hand_aka + meld_aka} "
            f"but yaku 54 says {reported} (concealed={win.concealed})")

    # 天凤的「门前手牌」= 没被副露拿走的部分 + 和牌张。杠也只按账面 3 张算
    # （加杠升级的碰仍是 1 组、3 张账面），所以这里按 melds 的**组数**推期望张数。
    expected_len = 14 - 3 * len(win.melds)
    if len(win.concealed) != expected_len:
        raise MjlogError(
            f"round {round_.index}: AGARI hai has {len(win.concealed)} tiles, "
            f"expected {expected_len} for {len(win.melds)} melds "
            f"(kinds={[meld.kind for meld in win.melds]})")
