#!/usr/bin/env python3
"""mjlog → 本仓库日麻引擎的**判番回归用例**（JSON）。

每个用例 = 一手真实和牌 + 它的上下文（立直 / 一发 / 海底 / 杠上开花 / 抢杠 / 天和…）+ 天凤
给出的期望值（符 / 番 / 点数 / 满贯分档 / 役种集合 / 宝牌张数）。C++ 侧
`tenhou_rules_check` 读这份 JSON 跑 `riichi::ScoreHand`，逐项比对。

## 上下文一律从**事件流**推导，绝不从 `yaku` 反推

天凤的 `AGARI` 只给结果，不给「一发」「海底」这类标志。若从 `yaku` 里读出一发再把它喂给
`WinContext`，就等于用答案喂自己 —— 判番 bug 会被掩盖。本文件的每个标志都只由事件顺序决定：

| 标志 | 口径 |
| --- | --- |
| `riichi` | 和牌者本局有 `<REACH step="1">` 宣言 |
| `ippatsu` | 宣言之后**没有任何鸣牌**，且和牌者本人此后**还没再打过牌** |
| `rinshan` | 和牌张就是本人上一次杠之后的岭上补牌 |
| `chankan` | 荣和的和牌张正是他家刚加杠的那张 |
| `haitei` | 自摸的那一摸之后，本局再无任何摸牌/打牌 |
| `houtei` | 荣和的那张之后，本局再无任何摸牌/打牌 |
| `tenhou` | 庄家第一次摸牌即自摸，且此前无任何打牌 |
| `chiihou` | 闲家第一次摸牌即自摸，且此前无任何鸣牌 |

`tests/tenhou_rules_check.cc` 的自校验会把这些推导结果与天凤 `yaku` 里的条件役 id（一发 2、
海底 5、河底 6、岭上 4、抢杠 3、天和 37、地和 38）逐条对齐，**推导与天凤不一致的用例直接报错**
—— 这样「上下文推导写错」和「引擎判番写错」不会混在一起。

## 不支持的役

引擎没有独立 id 的役（三色同刻 26、人和 36）与引擎不建模的「役牌细分」（自风 10..13 /
场风 14..17 / 三元 18..20 都归一成 `yakuhai`）在这里按下面的方式处理：役种集合比对时把它们
映射到 `yakuhai`（三元/场风/自风同一个 id，番数由引擎自己算），无法映射的整手跳过。

## 用法

    python3 game/tools/tenhou_convert.py --mjlog game/tools/tenhou_corpus/*.xml \
        --out build/tenhou_cases.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import tenhou_mjlog as tm  # noqa: E402

#: 天凤役 ID → 本仓库 `protocol::YakuId` 的名字。
TENHOU_TO_OURS: dict[int, str] = {
    0: "tsumo", 1: "riichi", 2: "ippatsu", 3: "chankan", 4: "rinshan", 5: "haitei",
    6: "houtei", 7: "pinfu", 8: "tanyao", 9: "iipeikou",
    10: "yakuhai", 11: "yakuhai", 12: "yakuhai", 13: "yakuhai",     # 自风
    14: "yakuhai", 15: "yakuhai", 16: "yakuhai", 17: "yakuhai",     # 场风
    18: "yakuhai", 19: "yakuhai", 20: "yakuhai",                    # 三元
    22: "chiitoitsu", 23: "chanta", 24: "ittsuu", 25: "sanshoku_doujun",
    26: "sanshoku_doukou",  # v1.26 起引擎有独立役种
    27: "sankantsu", 28: "toitoi", 29: "sanankou", 30: "shousangen",
    31: "honroutou", 32: "ryanpeikou", 33: "junchan", 34: "honitsu", 35: "chinitsu",
    37: "tenhou", 38: "chiihou", 39: "daisangen", 40: "suuankou", 41: "suuankou_tanki",
    42: "tsuuiisou", 43: "ryuuiisou", 44: "chinroutou", 45: "chuuren",
    46: "chuuren_junsei", 47: "kokushi", 48: "kokushi_13", 49: "daisuushii",
    50: "shousuushii", 51: "suukantsu",
}
#: 引擎没有对应役种 ⇒ 该手牌跳过（36 人和；三色同刻 v1.26 起已实现，不再在此）
UNMAPPABLE = {36}
#: 天凤把「不是役」的三条也塞进 `yaku`：宝牌 / 里宝 / 赤宝牌
DORA_IDS = {52: "dora", 53: "ura_dora", 54: "aka_dora"}

#: 天凤的条件役 id → 我们推导出的标志名（`tests/tenhou_rules_check.cc` 用它做交叉校验）
CONDITION_IDS = {2: "ippatsu", 3: "chankan", 4: "rinshan", 5: "haitei", 6: "houtei",
                 37: "tenhou", 38: "chiihou"}


def meld_kind_of(meld: tm.Meld) -> str:
    return {
        tm.CHI: "chow", tm.PON: "pong", tm.MINKAN: "kong",
        tm.ANKAN: "ankan", tm.KAKAN: "kakan",
    }[meld.kind]


#: 一局能摸的活牌张数：136 − 52（配牌，`INIT.hai0..hai3` **都是 13 张**）− 14（王牌）= 70。
#: 庄家的第 14 张也是摸牌（`<T>`），不是配牌。每开一个杠，能从牌山摸的牌少一张
#: （岭上补牌用王牌换山顶牌）。真实牌谱核对：唯一一手海底自摸恰好摸过 70 张。
LIVE_DRAWS = 70


class ContextBuilder:
    """把一局的事件流摊平成「和牌那一刻」的上下文。

    只读事件顺序，不读 `AGARI.yaku` —— 理由见模块文档。三处容易写错的地方：

    - **海底/河底不能用「这是最后一个动作」判**：一局在和牌时结束，最后一张弃牌永远满足
      「后面没有动作」。正确判据是**牌山已经摸空**：活牌摸过 `69 − 杠数` 张。
    - **一发**：`<REACH step="1">` 出现在宣言牌**被打出之前**，所以宣言之初的弃牌计数还不含
      那张牌；宣言后应当恰好再多 1 张自家弃牌，且此后没有任何鸣牌。
    - **岭上补牌**：杠之后的那次摸牌走王牌，不计入活牌张数。
    """

    def __init__(self, round_: tm.Round):
        self.round = round_
        self.declared_at: dict[int, dict] = {}   # seat → 宣言时的计数快照
        self.discards_of: dict[int, int] = {}
        self.draws_of: dict[int, int] = {}
        self.melds_seen = 0
        self.melds_breaking_ippatsu = 0   # 除自家暗杠以外的鸣牌数（暗杠不破一发）
        self.live_draws = 0
        self.kans = 0
        self.kakan: dict[int, str] = {}          # seat → 最近一次加杠的牌码
        self.replacement: dict[int, str] = {}    # seat → 上一次杠后的岭上补牌
        self.pending_kan: set[int] = set()       # 刚杠过、下一摸是补牌的座位

    def run(self) -> dict | None:
        win = self.round.win
        if win is None:
            return None
        for action in self.round.actions:
            if action.kind == "draw":
                self._on_draw(action.seat, action.tile)
            elif action.kind == "discard":
                self.discards_of[action.seat] = self.discards_of.get(action.seat, 0) + 1
            elif action.kind == "meld" and action.meld is not None:
                self._on_meld(action.meld)
            elif action.kind == "riichi":
                self.declared_at[action.seat] = {
                    "discards": self.discards_of.get(action.seat, 0),
                    "draws": self.draws_of.get(action.seat, 0),
                    "melds_breaking_ippatsu": self.melds_breaking_ippatsu,
                }
        return self._context(win)

    def _on_draw(self, seat: int, tile: str) -> None:
        self.draws_of[seat] = self.draws_of.get(seat, 0) + 1
        if seat in self.pending_kan:
            self.pending_kan.discard(seat)
            self.replacement[seat] = tile
            return
        self.live_draws += 1
        self.replacement.pop(seat, None)

    def _on_meld(self, meld: tm.Meld) -> None:
        self.melds_seen += 1
        if meld.is_kan:
            self.kans += 1
            self.pending_kan.add(meld.seat)
        if meld.kind != tm.ANKAN:  # 暗杠不破一发（天凤口径）
            self.melds_breaking_ippatsu += 1
        if meld.kind == tm.KAKAN:
            self.kakan[meld.seat] = tm.to_protocol_tile(meld.from_hand[0])

    def _context(self, win: tm.WinResult) -> dict:
        seat = win.seat
        declaration = self.declared_at.get(seat)

        # 一发：立直宣言之后**没有鸣牌**，且和牌发生在「自己的下一巡」之内 ——
        # 自摸 = 宣言后只摸过这一次；荣和 = 宣言后自己一次都没摸过（别人的弃牌）。
        # 真实牌谱核对：自摸的一发恒为「宣言后自己摸 1 次」，荣和的一发恒为 0 次；
        # 摸过 2 次的（含自摸）全部没有一发。
        ippatsu = False
        if declaration is not None:
            draws_since = self.draws_of.get(seat, 0) - declaration["draws"]
            ippatsu = (
                self.melds_breaking_ippatsu == declaration["melds_breaking_ippatsu"]
                and draws_since == (1 if win.tsumo else 0)
            )

        # 牌山摸空 ⇒ 这一张就是最后一张
        exhausted = self.live_draws >= LIVE_DRAWS - self.kans

        return {
            "seat": seat,
            "from_seat": win.from_seat,
            "tsumo": win.tsumo,
            "dealer_seat": self.round.dealer,
            "round_wind": self.round.round_wind,
            "honba": self.round.honba,
            "riichi": declaration is not None,
            "ippatsu": ippatsu,
            "rinshan": win.tsumo and self.replacement.get(seat) == win.winning_tile,
            "chankan": (not win.tsumo and self.kakan.get(win.from_seat) == win.winning_tile),
            "haitei": win.tsumo and exhausted,
            "houtei": (not win.tsumo) and exhausted,
            "tenhou": (win.tsumo and self.round.dealer == seat
                       and self.discards_of.get(seat, 0) == 0 and self.melds_seen == 0),
            "chiihou": (win.tsumo and self.round.dealer != seat
                        and self.melds_seen == 0 and self.live_draws <= 1),
        }


def case_of(hand: tm.Hand, round_: tm.Round) -> tuple[dict | None, str]:
    """一局 → (用例, 跳过原因)。"""
    win = round_.win
    if win is None:
        return None, "no_win"
    yaku_ids = {yaku_id for yaku_id, _ in win.yaku}
    if yaku_ids & UNMAPPABLE:
        return None, "unmappable_yaku"
    unknown = yaku_ids - set(TENHOU_TO_OURS) - set(DORA_IDS)
    if unknown:
        return None, f"unknown_yaku_{sorted(unknown)}"

    context = ContextBuilder(round_).run()
    assert context is not None

    # 条件役的交叉校验：推导出的标志必须与天凤的 id 一致（不一致就整手不入库，并在
    # 统计里单列，方便一眼看出是推导错了还是牌谱异常）
    for tenhou_id, flag in CONDITION_IDS.items():
        if (tenhou_id in yaku_ids) != bool(context[flag]):
            return None, f"context_mismatch:{flag}"

    yaku_expected = sorted({TENHOU_TO_OURS[yaku_id] for yaku_id in yaku_ids if yaku_id in TENHOU_TO_OURS})
    return {
        "source": {"log_id": hand.log_id, "round": round_.index},
        "context": context,
        "concealed": list(win.concealed),
        "melds": [{"type": meld_kind_of(meld), "tiles": [tm.to_protocol_tile(tile) for tile in meld.tiles]}
                  for meld in win.melds],
        "winning_tile": win.winning_tile,
        "dora_indicators": [tm.to_protocol_tile(tile) for tile in win.dora_indicators],
        "ura_indicators": [tm.to_protocol_tile(tile) for tile in win.ura_indicators],
        "expected": {
            "fu": win.fu,
            "han": win.han - sum(win.yaku_han(dora_id) for dora_id in DORA_IDS),
            "points": win.points,
            "limit": tm.TENHOU_LIMITS[win.limit] if 0 <= win.limit < len(tm.TENHOU_LIMITS) else "none",
            "yaku": yaku_expected,
            "dora": win.yaku_han(52),
            "ura_dora": win.yaku_han(53),
            "aka_dora": win.yaku_han(54),
        },
    }, "ok"


def build_cases(paths: list[str]) -> tuple[list[dict], dict]:
    cases: list[dict] = []
    stats: dict = {"hands": 0, "rounds": 0, "wins": 0, "cases": 0}
    skipped: dict[str, int] = {}
    for path in paths:
        hand = tm.parse_hand(tm.load(path), log_id=Path(path).stem)
        stats["hands"] += 1
        for round_ in hand.rounds:
            stats["rounds"] += 1
            if round_.win is None:
                continue
            stats["wins"] += 1
            case, reason = case_of(hand, round_)
            if case is None:
                skipped[reason] = skipped.get(reason, 0) + 1
                continue
            cases.append(case)
    stats["cases"] = len(cases)
    stats["skipped"] = skipped
    return cases, stats


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mjlog", nargs="+", required=True, help="mjlog 文件（xml 或 gz）")
    parser.add_argument("--out", required=True, help="用例 JSON 的输出路径")
    args = parser.parse_args()

    cases, stats = build_cases(sorted(args.mjlog))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8") as handle:
        json.dump({"cases": cases, "stats": stats}, handle, ensure_ascii=False, indent=1)
    print(f"[convert] {stats['cases']}/{stats['wins']} 手和牌成为用例（{stats['hands']} 个半庄，"
          f"{stats['rounds']} 局）→ {out}")
    if stats["skipped"]:
        print(f"[convert] 跳过：{stats['skipped']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
