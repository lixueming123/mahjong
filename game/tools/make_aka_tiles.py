#!/usr/bin/env python3
"""从 CC0 牌面贴图派生日麻赤宝牌（`0m` / `0p` / `0s`）。

赤 5 在物理上就是普通 5 的第 4 张（`mahjong_rules::kRedFiveCopy`），牌面图案
与普通 5 完全一样、只是油墨换成红色。所以这里不做重新绘制，而是对现有
`5m.png` / `5p.png` / `5s.png` 做**颜色替换**：

1. 纸面是近白（`min(r,g,b) ≈ 246`），据此估每个像素的「墨浓度」
   `ink = 1 - min(r,g,b)/246`；
2. 墨区（含黑描边、深蓝筒、绿竹、暗红「萬」）统一按 `ink` 混合到目标红，
   抗锯齿边缘的 `ink` 天然偏低，所以过渡与原图一致；
3. 纸面、白色高光、内描白线保持原样 —— 于是赤 5 与其它 33 种牌**同源同光照**，
   在牌桌上看不出是「另画的一套」。

用法：
    python3 game/tools/make_aka_tiles.py            # 生成 0m/0p/0s.png
    python3 game/tools/make_aka_tiles.py --check    # 只比对，不写文件
    python3 game/tools/make_aka_tiles.py --red 200,40,40

素材授权：FluffyStuff / riichi-mahjong-tiles，CC0 1.0 公有领域；派生出的
赤 5 同样是 CC0（见 `game/client/assets/mahjong/SOURCE.txt`）。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - 生成工具，不在运行时依赖
    sys.exit("需要 Pillow：./.venv/bin/pip install Pillow")

ASSET_DIR = Path(__file__).resolve().parents[1] / "client" / "assets" / "mahjong" / "faces"

#: 纸面基准白（贴图里牌面最亮的像素）
PAPER = 246.0
#: 低于这个「墨浓度」视作纸面（含纸纹与柔和阴影），保持原样
INK_FLOOR = 0.06
#: 墨浓度归一化上限（纯黑 ≈ 1.0，深色油墨 ≈ 0.85~0.9）
INK_SPAN = 0.86
#: 默认赤牌红 = 客户端强调色 `theme.AKA`
DEFAULT_RED = (208, 36, 40)

RED_FIVES = {"5m": "0m", "5p": "0p", "5s": "0s"}


def recolor(image: Image.Image, red: tuple[int, int, int]) -> Image.Image:
    source = image.convert("RGBA")
    out = source.copy()
    src, dst = source.load(), out.load()
    for y in range(source.height):
        for x in range(source.width):
            r, g, b, a = src[x, y]
            if a == 0:
                continue
            coverage = (1.0 - min(r, g, b) / PAPER - INK_FLOOR) / INK_SPAN
            if coverage <= 0.0:
                continue
            coverage = min(1.0, coverage)
            dst[x, y] = (
                round(r + (red[0] - r) * coverage),
                round(g + (red[1] - g) * coverage),
                round(b + (red[2] - b) * coverage),
                a,
            )
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="生成日麻赤宝牌贴图（0m/0p/0s.png）")
    parser.add_argument("--red", default=",".join(str(c) for c in DEFAULT_RED),
                        help="目标红色，形如 208,36,40")
    parser.add_argument("--check", action="store_true", help="只校验现有文件是否为最新派生结果")
    parser.add_argument("--sheet", metavar="PATH", help="额外输出一张原图/赤牌对照图")
    args = parser.parse_args(argv)

    red = tuple(int(part) for part in args.red.split(","))
    if len(red) != 3:
        parser.error("--red 需要三个分量，例如 208,36,40")

    stale: list[str] = []
    pairs: list[tuple[Image.Image, Image.Image, str]] = []
    for normal, aka in RED_FIVES.items():
        source_path = ASSET_DIR / f"{normal}.png"
        target_path = ASSET_DIR / f"{aka}.png"
        if not source_path.exists():
            print(f"缺少源贴图：{source_path}", file=sys.stderr)
            return 1
        result = recolor(Image.open(source_path), red)
        pairs.append((Image.open(source_path).convert("RGBA"), result, normal))
        if args.check:
            if not target_path.exists():
                stale.append(f"{aka}.png 缺失")
                continue
            current = Image.open(target_path).convert("RGBA")
            if current.size != result.size or current.tobytes() != result.tobytes():
                stale.append(f"{aka}.png 与派生结果不一致")
            continue
        result.save(target_path)
        print(f"{target_path.relative_to(ASSET_DIR.parents[3])}: {result.width}x{result.height} ← {normal}.png")

    if args.sheet:
        pad = 10
        width = sum(item[1].width for item in pairs) + pad * (len(pairs) + 1)
        height = pairs[0][1].height * 2 + pad * 3
        sheet = Image.new("RGBA", (width, height), (32, 32, 36, 255))
        x = pad
        for source, result, _ in pairs:
            sheet.alpha_composite(source, (x, pad))
            sheet.alpha_composite(result, (x, pad * 2 + source.height))
            x += source.width + pad
        sheet.save(args.sheet)
        print(f"对照图：{args.sheet}（上排原图 / 下排赤牌）")

    if args.check:
        if stale:
            print("赤牌贴图过期：", "；".join(stale), file=sys.stderr)
            return 1
        print("赤牌贴图已是最新")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
