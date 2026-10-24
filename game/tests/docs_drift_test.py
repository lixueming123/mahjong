#!/usr/bin/env python3
"""文档漂移探测器（SETTLEMENT_BROADCAST_RULINGS v1.2 §4 F4）。

规格里「番数四段式」的加法只有两项：`base + dealer`。历史上的错误写法
`min(base + tsumo + dealer, 6)` 会把 `tsumo`（自摸）当成 +1 番参与加法，直接击穿
「自摸不加番」（RULINGS C9）与 S3 的回归锚（0 番 × 庄家 × 自摸 ⇒ 1 番 / 2 倍 / 12 分）。

本探测器只做两件事，纯标准库、不启动任何进程、退出码 0 即通过：

1. 断言 `docs/PROTOCOL.md` **不含**将 `tsumo` 写进 `min(...)` 的写法；
2. 断言 `docs/PROTOCOL.md` **含**正确写法 `min(base + dealer, 6)`；

并顺带把 `UI_UX_SPEC.md` 的「自摸 +N」线框/说明也纳入（F2 的回流防护）。

运行：
    python3 game/tests/docs_drift_test.py
"""

from __future__ import annotations

import re
import sys

HERE = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
DOCS = f"{HERE}/../docs"

#: 禁止回流：把 tsumo 写进封顶公式的任何变体（空格/加法顺序/大小写都不放过）
FORBIDDEN_FORMULAS = (
    re.compile(r"min\(\s*base\s*\+\s*tsumo\s*\+\s*dealer\s*,\s*6\s*\)"),
    re.compile(r"min\(\s*base\s*\+\s*dealer\s*\+\s*tsumo\s*,\s*6\s*\)"),
    re.compile(r"min\(\s*base\s*\+\s*tsumo\s*\+\s*dealer\s*,\s*6\s*\)", re.IGNORECASE),
)
#: 正确写法（必须至少出现一次）
REQUIRED_FORMULA = re.compile(r"min\(\s*base\s*\+\s*dealer\s*,\s*6\s*\)")
#: 禁止回流：线框/说明里的「自摸 +N」（自摸不加番，屏上不得出现「自摸 +0」）
FORBIDDEN_TSUMO_BONUS = re.compile(r"自摸\s*\+\s*(?:\d+|`?tsumo`?|[Nn])")


def read(name: str) -> str:
    with open(f"{DOCS}/{name}", encoding="utf-8") as handle:
        return handle.read()


def main() -> int:
    failures: list[str] = []

    protocol = read("PROTOCOL.md")
    for pattern in FORBIDDEN_FORMULAS:
        for match in pattern.finditer(protocol):
            line = protocol[: match.start()].count("\n") + 1
            failures.append(f"PROTOCOL.md:{line} 出现禁用公式 {match.group(0)!r}（tsumo 恒 0，不参与加法）")
    if not REQUIRED_FORMULA.search(protocol):
        failures.append("PROTOCOL.md 缺少正确公式 `min(base + dealer, 6)`")

    ui = read("UI_UX_SPEC.md")
    for name, text in (("UI_UX_SPEC.md", ui), ("PROTOCOL.md", protocol)):
        for match in FORBIDDEN_TSUMO_BONUS.finditer(text):
            line = text[: match.start()].count("\n") + 1
            failures.append(f"{name}:{line} 出现「自摸 +N」{match.group(0)!r}（自摸不是番种，不得渲染）")

    if failures:
        print("docs drift FAILED:")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("docs drift OK：番数公式只有 base + dealer，且无线框级的「自摸 +N」")
    return 0


if __name__ == "__main__":
    sys.exit(main())
