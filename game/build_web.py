#!/usr/bin/env python3
"""从 client/assets 生成 game/web/assets 素材（牌面图集 + 音效）。

图集分工：
  tile-atlas.png       —— 旧版金框牌面（保留作兜底）
  tile-atlas-flat.png  —— 平躺牌（牌河 / 副露 / 宝牌）的**顶面**：与立牌同一套白牌身、
                          同样的圆角和细暗边，但没有牌顶、没有金框。
                          厚度不画进贴图，由 flat3d 的斜切盒侧面 + 灯光给出，
                          这样躺牌和立牌摆在一起不会像是两套素材。
  tile-atlas-hand.png  —— 本家立牌，按录像的 2.5D 立牌画法程序化绘制：
                          橙金圆角牌顶 + 灰紫阴影带 + 白色圆角牌面 + 左右暗缝 + 底部接触暗边

素材只取 client/assets/mahjong/faces/*.png 的「牌面墨迹」（白底抠成透明），
牌身、配色、立体层次全部按参考录像采样，不复用 pygame 客户端的界面绘制。
用法: ./.venv/bin/python game/build_web.py
"""
from __future__ import annotations

import json
import shutil
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parent
FACES = ROOT / "client" / "assets" / "mahjong" / "faces"
SFX = ROOT / "client" / "assets" / "sfx"
OUT = ROOT / "web" / "assets"

# 与牌局逻辑一致的 34 面 + 3 赤牌 + 牌背
CODES = (
    [f"{r}{s}" for s in "mps" for r in range(1, 10)]
    + [f"{r}z" for r in range(1, 8)]
    + ["0m", "0p", "0s", "back"]
)
assert len(CODES) == 38

COLS, ROWS = 8, 5

# ---- 平躺舍牌图集 ----
CELL_W, CELL_H = 96, 128
IVORY = (246, 241, 228)
BACK = (230, 158, 46)
INNER = 8
BORDER = (186, 148, 72)

# ---- 立牌图集：120:200 = 0.6，与设计坐标 90:150 完全一致（贴图不拉伸）----
H_W, H_H = 120, 200
CAP_H = 10           # 橙金牌顶（设计 7.5px，录像实测约 7px）
SHADE_H = 16         # 牌顶下的灰紫阴影带（设计 12px）
CORNER = 13


def build_atlas() -> Path:
    """平躺舍牌：保留金边，单张辨识度优先。"""
    atlas = Image.new("RGB", (COLS * CELL_W, ROWS * CELL_H), IVORY)
    for i, code in enumerate(CODES):
        im = Image.open(FACES / f"{code}.png").convert("RGBA").resize(
            (CELL_W - INNER * 2, CELL_H - INNER * 2), Image.LANCZOS
        )
        if code == "back":
            gray = im.convert("L")
            tinted = Image.new("RGBA", im.size)
            gp, sp, op = gray.load(), im.load(), tinted.load()
            for y in range(im.size[1]):
                for x in range(im.size[0]):
                    v = min(1.0, (gp[x, y] / 255.0) * 1.55)
                    op[x, y] = (int(BACK[0] * v), int(BACK[1] * v),
                                int(BACK[2] * v), sp[x, y][3])
            im = tinted
        cell = Image.new("RGB", (CELL_W, CELL_H), IVORY)
        ImageDraw.Draw(cell).rounded_rectangle(
            [1, 1, CELL_W - 2, CELL_H - 2], radius=9,
            fill=(252, 249, 240), outline=BORDER, width=3,
        )
        cell.paste(im, (INNER, INNER), im)
        atlas.paste(cell, ((i % COLS) * CELL_W, (i // COLS) * CELL_H))

    q = atlas.quantize(colors=128, method=Image.Quantize.MEDIANCUT).convert("RGB")
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "tile-atlas.png"
    q.save(path, format="PNG", optimize=True)
    (OUT / "tile-codes.json").write_text(json.dumps(CODES, ensure_ascii=False), encoding="utf-8")
    return path


# ---- 平躺牌面图集：和立牌同一套白牌面，厚度交给 flat3d 的斜切盒侧面 ----
# 2x 超采样：本家副露顶面画到 ~94x150 还要吃透视放大，96x128 的格子被
# 拉大采样会糊（手牌 120x200 有余量所以清晰）；192x256 后全程缩小采样。
F_W, F_H = 192, 256
F_CORNER = 22


def _face_plate(w: int, h: int, corner: int) -> Image.Image:
    """纯白牌面（无牌顶、无金框）：竖向渐变 + 左右柱面压暗 + 细暗边。

    立牌和躺牌共用这一套，两边的白、冷暖、圆角才完全对得上。
    """
    base = _gradient([
        (0.00, (232, 232, 230)), (0.42, (252, 252, 250)), (1.00, (226, 227, 225)),
    ], h, w).astype(np.float32)
    xs = np.linspace(0, 1, w)[None, :, None]
    edge = np.clip((np.abs(xs - .5) * 2 - .70) / .30, 0, 1)
    base *= (1 - edge * .13)

    body = Image.new("L", (w, h), 0)
    ImageDraw.Draw(body).rounded_rectangle([1, 1, w - 2, h - 2], radius=corner, fill=255)
    arr = np.concatenate([
        np.clip(base, 0, 255).astype(np.uint8), np.asarray(body)[..., None],
    ], axis=2)
    plate = Image.fromarray(arr, "RGBA")
    ImageDraw.Draw(plate).rounded_rectangle(
        [1, 1, w - 2, h - 2], radius=corner, outline=(96, 100, 112, 120), width=2
    )
    return plate


def _flat_cell(code: str) -> Image.Image:
    """一张躺下的牌面：白牌身 + 图案，四角透明（圆角由 alpha 决定）。"""
    cell = _face_plate(F_W, F_H, F_CORNER)
    ink = _ink_layer(code, F_W - 16, F_H - 22)
    if code == "back":
        # 牌背朝上（宝牌指示等）：花纹按牌背的橙金色压一层
        a = np.asarray(ink, dtype=np.float32)
        tint = np.array([230, 158, 46], np.float32)
        a[..., :3] = np.clip(a[..., :3] * .32 + tint * .78, 0, 255)
        ink = Image.fromarray(a.astype(np.uint8), "RGBA")
    # 墨迹层要在格子里居中。过去按 (16, 22) 当「内边距」贴，是单边偏移：
    # 176x234 的图案被推右 8px、推下 11px，牌河/副露/宝牌的躺牌看起来
    # 集体向右歪，横放那张旋转 90° 后偏移换方向更明显。
    cell.alpha_composite(ink, ((F_W - ink.width) // 2, (F_H - ink.height) // 2))
    return cell


def build_flat_atlas() -> Path:
    """平躺牌面图集（牌河 / 副露 / 宝牌）——只给顶面用，牌厚靠几何。"""
    atlas = Image.new("RGBA", (COLS * F_W, ROWS * F_H), (0, 0, 0, 0))
    for i, code in enumerate(CODES):
        atlas.alpha_composite(_flat_cell(code), ((i % COLS) * F_W, (i // COLS) * F_H))
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "tile-atlas-flat.png"
    atlas.save(path, format="PNG", optimize=True)
    return path


def _ink_layer(code: str, w: int, h: int) -> Image.Image:
    """从白底素材里只取「墨迹」：裁掉素材自带的圆角牌框，白底 -> alpha 0。

    配色完全沿用素材原色（含四风的蓝字），不做任何按录像的改色。
    """
    im = Image.open(FACES / f"{code}.png").convert("RGB")
    W0, H0 = im.size
    im = im.crop((int(W0 * .055), int(H0 * .05),
                  int(W0 * .945), int(H0 * .95))).resize((w, h), Image.LANCZOS)
    a = np.asarray(im, dtype=np.float32)
    lum = a[..., 0] * .299 + a[..., 1] * .587 + a[..., 2] * .114
    alpha = np.clip(np.clip((1 - lum / 255 - .05) / .95, 0, 1) * 1.10, 0, 1)
    return Image.fromarray(
        np.concatenate([a, (alpha * 255)[..., None]], axis=2).astype(np.uint8), "RGBA"
    )


def _gradient(stops: list[tuple[float, tuple[int, int, int]]], h: int, w: int) -> np.ndarray:
    """按 y 生成竖向渐变，返回 (h, w, 3) uint8。stops=[(位置0..1, RGB)]。"""
    ys = np.linspace(0, 1, h)
    line = np.zeros((h, 3), dtype=np.float32)
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        sel = (ys >= p0) & (ys <= p1)
        t = ((ys[sel] - p0) / max(p1 - p0, 1e-6))[:, None]
        line[sel] = np.array(c0, np.float32) * (1 - t) + np.array(c1, np.float32) * t
    return np.repeat(np.clip(line, 0, 255).astype(np.uint8)[:, None, :], w, axis=1)


def _hand_cell(code: str) -> Image.Image:
    """一张立牌：橙金牌顶 + 灰紫阴影带 + 白色圆角牌面 + 左右暗缝 + 触底暗边。"""
    # 牌身轮廓（圆角外全透明 -> 牌间露出深色台布形成缝）
    body = Image.new("L", (H_W, H_H), 0)
    ImageDraw.Draw(body).rounded_rectangle([2, 2, H_W - 3, H_H - 4], radius=CORNER, fill=255)
    body_a = np.asarray(body, dtype=np.uint8)

    # 白色牌面：上略灰 -> 中亮 -> 下略灰，左右再压暗形成柱面受光
    base = _gradient([
        (0.00, (232, 232, 230)), (0.42, (252, 252, 250)), (1.00, (226, 227, 225)),
    ], H_H, H_W).astype(np.float32)
    xs = np.linspace(0, 1, H_W)[None, :, None]
    edge = np.clip((np.abs(xs - .5) * 2 - .70) / .30, 0, 1)
    base *= (1 - edge * .13)
    arr = np.concatenate([
        np.clip(base, 0, 255).astype(np.uint8), body_a[..., None],
    ], axis=2)

    # 灰紫阴影带：牌顶内侧，越往下越亮（录像里这段是明显的灰紫渐变）
    y0 = 2 + CAP_H
    for y in range(y0, y0 + SHADE_H):
        t = (y - y0) / SHADE_H
        c = np.array([186, 183, 198], np.float32) * (1 - t) + \
            np.array([246, 246, 244], np.float32) * t
        row = arr[y, :, :3].astype(np.float32)
        arr[y, :, :3] = np.clip(
            np.where(body_a[y, :, None] > 0, row * .18 + c * .82, row), 0, 255
        ).astype(np.uint8)

    cell = Image.fromarray(arr, "RGBA")

    # 橙金圆角牌顶：圆上角、直下缘，上亮下深
    cap = Image.new("L", (H_W, H_H), 0)
    ImageDraw.Draw(cap).rounded_rectangle(
        [2, 2, H_W - 3, 2 + CAP_H + CORNER], radius=CORNER, fill=255
    )
    strip = Image.new("L", (H_W, H_H), 0)
    ImageDraw.Draw(strip).rectangle([0, 0, H_W, 2 + CAP_H], fill=255)
    cap = ImageChops.multiply(cap, strip)
    cap_h = 2 + CAP_H + 1
    cap_rgba = np.zeros((H_H, H_W, 4), np.uint8)
    cap_rgba[:cap_h, :, :3] = _gradient([
        (0.00, (255, 198, 92)), (0.45, (231, 155, 48)), (1.00, (178, 106, 26)),
    ], cap_h, H_W)
    cap_rgba[:, :, 3] = np.asarray(cap, dtype=np.uint8)
    cell = Image.alpha_composite(cell, Image.fromarray(cap_rgba, "RGBA"))

    d = ImageDraw.Draw(cell)
    d.line([(CORNER, 3), (H_W - CORNER, 3)], fill=(255, 224, 152, 190), width=2)
    d.line([(3, 2 + CAP_H), (H_W - 3, 2 + CAP_H)], fill=(120, 74, 20, 150), width=2)
    d.rounded_rectangle([2, 2, H_W - 3, H_H - 4], radius=CORNER,
                        outline=(62, 66, 80, 135), width=3)

    if code == "back":
        # 牌背：橙金底 + 原牌背花纹做浮雕明暗
        top = y0 + SHADE_H
        iw, ih = H_W - 22, H_H - top - 12
        relief = np.asarray(Image.open(FACES / "back.png").convert("L").resize(
            (iw, ih), Image.LANCZOS), dtype=np.float32)[..., None] / 255.0
        gold = np.array([230, 158, 46], np.float32)[None, None, :]
        rgb = np.clip(gold * (.55 + relief * .75), 0, 255).astype(np.uint8)
        patch = Image.fromarray(np.concatenate([
            rgb, np.full((ih, iw, 1), 255, np.uint8),
        ], axis=2), "RGBA")
        inner = Image.new("L", (H_W, H_H), 0)
        ImageDraw.Draw(inner).rounded_rectangle(
            [11, top, 11 + iw, top + ih], radius=8, fill=255
        )
        layer = Image.new("RGBA", (H_W, H_H), (0, 0, 0, 0))
        layer.paste(patch, (11, top))
        layer.putalpha(ImageChops.multiply(layer.getchannel("A"), inner))
        cell = Image.alpha_composite(cell, layer)
    else:
        ink = _ink_layer(code, H_W - 22, H_H - 46)
        cell.alpha_composite(ink, (11, y0 + SHADE_H + 4))

    return cell


def build_hand_atlas() -> Path:
    """本家立牌图集：8x5，每格透明圆角牌身。"""
    atlas = Image.new("RGBA", (COLS * H_W, ROWS * H_H), (0, 0, 0, 0))
    for i, code in enumerate(CODES):
        atlas.alpha_composite(_hand_cell(code), ((i % COLS) * H_W, (i // COLS) * H_H))
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "tile-atlas-hand.png"
    atlas.save(path, format="PNG", optimize=True)
    return path


def build_ink_atlas() -> Path:
    """舍牌专用：只保留牌面墨迹的透明图集（8x5, 96x128）。

    牌身（白色圆角 + 灰渐变 + 橙金厚边）在 tiles.js 里独立绘制且不旋转，
    只有这一层墨迹按各家方向旋转，厚边才不会被一起转走。
    """
    atlas = Image.new("RGBA", (COLS * CELL_W, ROWS * CELL_H), (0, 0, 0, 0))
    for i, code in enumerate(CODES):
        cell = Image.new("RGBA", (CELL_W, CELL_H), (0, 0, 0, 0))
        if code == "back":
            ImageDraw.Draw(cell).rounded_rectangle(
                [6, 6, CELL_W - 7, CELL_H - 7], radius=9, fill=(230, 158, 46, 255)
            )
        else:
            ink = _ink_layer(code, CELL_W - 18, CELL_H - 20)
            cell.alpha_composite(ink, (9, 10))
        atlas.alpha_composite(cell, ((i % COLS) * CELL_W, (i // COLS) * CELL_H))
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "tile-atlas-ink.png"
    atlas.save(path, format="PNG", optimize=True)
    return path


def copy_sfx() -> list[str]:
    names = {
        "discard.ogg": "sfx_tile_discard.ogg",
        "draw.ogg": "sfx_tile_draw.ogg",
        "click.ogg": "sfx_ui_click.ogg",
    }
    (OUT / "sfx").mkdir(parents=True, exist_ok=True)
    copied = []
    for dst, src in names.items():
        s = SFX / src
        if s.exists():
            shutil.copy2(s, OUT / "sfx" / dst)
            copied.append(dst)
    return copied


def copy_vo() -> list[str]:
    """结算播报语音（client/assets/vo/*.wav，含日麻补录句）→ web/assets/vo。"""
    src_dir = ROOT / "client" / "assets" / "vo"
    out_dir = OUT / "vo"
    out_dir.mkdir(parents=True, exist_ok=True)
    copied = []
    for wav in sorted(src_dir.glob("*.wav")):
        shutil.copy2(wav, out_dir / wav.name)
        copied.append(wav.name)
    return copied


if __name__ == "__main__":
    for p in (build_atlas(), build_flat_atlas(), build_hand_atlas(), build_ink_atlas()):
        print(f"atlas  {p.relative_to(ROOT)}  {p.stat().st_size:,} B")
    for n in copy_sfx():
        f = OUT / "sfx" / n
        print(f"sfx    {f.relative_to(ROOT)}  {f.stat().st_size:,} B")
    vo = copy_vo()
    print(f"vo     {len(vo)} files -> { (OUT / 'vo').relative_to(ROOT) }  "
          f"{sum((OUT / 'vo' / n).stat().st_size for n in vo):,} B")
    print("done -> game/web/assets/")
