#!/usr/bin/env python3
"""牌面图集居中回归：躺牌图案不许整体偏移（用户截图「二万向右歪」）。

历史 bug：`build_web.py::_flat_cell` 把 176x234 的墨迹层按 `(16, 22)` 左上角
贴进 192x256 格子 —— 那是单边内边距而不是居中，图案被推右 8px、推下 11px。
牌河 / 副露 / 宝牌指示全用这套 flat 图集；被鸣牌还会旋转 90°，偏移换个方向
更刺眼。结算面板用 hand 图集（水平本来就居中），所以对比截图里「一个正常一个歪」。

本探测器只读 PNG，不启动服务：逐格统计彩色/纯黑墨迹的包围盒中心，四套图集
都要求水平居中，flat 额外要求垂直居中；并检查生成脚本仍用居中贴法，避免
下次重建又把偏移带回来。退出码 0 即通过。

运行：python3 game/tests/atlas_centering_test.py
"""
from __future__ import annotations

import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "web", "assets")
BUILD_SCRIPT = os.path.join(HERE, "..", "build_web.py")

CODES = ([f"{r}{s}" for s in "mps" for r in range(1, 10)]
         + [f"{r}z" for r in range(1, 8)] + ["0m", "0p", "0s", "back"])
PROBES = ["2m", "4m", "2s", "3s", "4p", "7m"]
ATLASES = [
    "tile-atlas.png",
    "tile-atlas-flat.png",
    "tile-atlas-hand.png",
    "tile-atlas-ink.png",
]


def ink_offset(im, code):
    """返回 (dx, dy, cell_w, cell_h)：墨迹包围盒中心相对格子中心的偏移。"""
    cw, ch = im.width // 8, im.height // 5
    idx = CODES.index(code)
    col, row = idx % 8, idx // 8
    cell = im.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch)).convert("RGBA")
    px = cell.load()
    cols, rows = [0] * cw, [0] * ch
    for y in range(ch):
        for x in range(cw):
            r, g, b, a = px[x, y]
            hi, lo = max(r, g, b), min(r, g, b)
            # 彩色墨迹（红/绿/蓝字）或纯黑字；灰色牌身边框/细缝不计
            if a > 128 and ((hi - lo > 45 and hi < 215) or hi < 90):
                cols[x] += 1
                rows[y] += 1
    nx = [i for i, v in enumerate(cols) if v > 3]
    ny = [i for i, v in enumerate(rows) if v > 3]
    if not nx or not ny:
        return None
    return ((min(nx) + max(nx)) / 2 - cw / 2,
            (min(ny) + max(ny)) / 2 - ch / 2, cw, ch)


def main():
    fails = []
    for name in ATLASES:
        with Image.open(os.path.join(ASSETS, name)) as im:
            for code in PROBES:
                got = ink_offset(im, code)
                if got is None:
                    fails.append(f"{name} {code}: 找不到墨迹")
                    continue
                dx, dy, cw, ch = got
                tol_x = max(2.0, cw * .03)
                if abs(dx) > tol_x:
                    fails.append(f"{name} {code}: 水平偏 {dx:+.1f}px（容差 ±{tol_x:.1f}）")
                if name == "tile-atlas-flat.png":
                    tol_y = max(2.0, ch * .03)
                    if abs(dy) > tol_y:
                        fails.append(f"{name} {code}: 垂直偏 {dy:+.1f}px（容差 ±{tol_y:.1f}）")
                print(f"  ok   {name:22s} {code}  dx={dx:+.1f} dy={dy:+.1f}")

    with open(BUILD_SCRIPT, encoding="utf8") as f:
        src = f.read()
    if "((F_W - ink.width) // 2, (F_H - ink.height) // 2)" not in src:
        fails.append("build_web.py::_flat_cell 不再是居中贴法，重建会重新带偏")

    if fails:
        for line in fails:
            print(f"  FAIL {line}")
        print(f"\natlas centering: {len(fails)} FAILED")
        return 1
    print("\natlas centering: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
