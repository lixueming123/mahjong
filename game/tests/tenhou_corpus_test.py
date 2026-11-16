#!/usr/bin/env python3
"""`tenhou_rules_check` 的 ctest 包装：用例 JSON 不存在时 SKIP（退出码 77）。

为什么不是直接 `add_test(COMMAND tenhou_rules_check ...)`：天凤牌谱**禁止再配布**，所以用例
JSON 与牌谱语料都不入库。干净 checkout / CI 上它们不存在 —— 那种情况下这个测试应该是
SKIP（看得见「没跑」），而不是失败或静默变绿。

本地怎么让它是真的跑起来：

    python3 game/tools/tenhou_fetch.py --out game/tools/tenhou_corpus --limit 20
    python3 game/tools/tenhou_convert.py --mjlog game/tools/tenhou_corpus/*.xml \
        --out build/tenhou_cases.json
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True, help="tenhou_rules_check 可执行文件")
    parser.add_argument("--cases", required=True, help="tenhou_convert.py 产出的用例 JSON")
    args = parser.parse_args()

    cases = Path(args.cases)
    if not cases.is_file() or cases.stat().st_size == 0:
        print(f"[tenhou] SKIP：{cases} 不存在（牌谱语料不入库，见 game/docs/TENHOU_CORPUS.md）")
        return 77

    result = subprocess.run([args.binary, str(cases)])
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
