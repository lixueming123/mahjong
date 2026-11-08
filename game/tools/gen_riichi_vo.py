#!/usr/bin/env python3
"""生成日麻缺失的播报语音（RULINGS 复用库够不到的役种句）。

背景：client/assets/vo 的 57 条资产是 classic 口径（役名+番值整句），
日麻独有役（立直/一发/门前自摸/抢杠/海底/河底/二杯口/纯全/混全/三杠子/
混老头）与开/闭番差句（混一色三番、清一色六番、三色/一气/混全一番）全部缺。
本脚本用 macOS `say`（与原管线同嗓音 Tingting、同语速 175、同格式
1ch/22050Hz/Int16）补齐，裁首尾静音，回写 vo_lines.tsv 与 vo_manifest.json。

用法：python3 game/tools/gen_riichi_vo.py   （在仓库根目录执行）
"""

from __future__ import annotations

import array
import json
import math
import struct
import subprocess
import sys
import tempfile
import wave
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VO = ROOT / "client" / "assets" / "vo"
VOICE = "Tingting"
RATE = 175
SAMPLE_RATE = 22050

#: 新增句（id, 文案）。番值后缀 = 服务端 riichi_rules.cc 的日麻番值；
#: 与 classic 同名同番的（断幺/平和/一杯口/对对/三暗/小三元/杠上开花/七对…）
#: 复用现有资产，不在这里重复生成。
NEW_LINES: list[tuple[str, str]] = [
    ("vo_yaku_riichi_1", "立直一番"),
    ("vo_yaku_ippatsu_1", "一发一番"),
    ("vo_yaku_tsumo_1", "门前清自摸一番"),
    ("vo_yaku_chankan_1", "抢杠一番"),
    ("vo_yaku_haitei_1", "海底捞月一番"),
    ("vo_yaku_houtei_1", "河底捞鱼一番"),
    ("vo_yaku_sanshoku_doujun_1", "三色同顺一番"),
    ("vo_yaku_ittsuu_1", "一气通贯一番"),
    ("vo_yaku_chanta_1", "混全带幺九一番"),
    ("vo_yaku_chanta_2", "混全带幺九两番"),
    ("vo_yaku_junchan_2", "纯全带幺九两番"),
    ("vo_yaku_junchan_3", "纯全带幺九三番"),
    ("vo_yaku_honitsu_3", "混一色三番"),
    ("vo_yaku_chinitsu_6", "清一色六番"),
    ("vo_yaku_ryanpeikou_3", "二杯口三番"),
    ("vo_yaku_honroutou_2", "混老头两番"),
    ("vo_yaku_sankantsu_2", "三杠子两番"),
    ("vo_yakuman", "役满"),
]

#: 静音阈值（线性振幅）与两端保留的呼吸位（ms）
THRESHOLD = 300
PAD_MS = 70


def synth(text: str, out: Path) -> None:
    subprocess.run(
        ["say", "-v", VOICE, "-r", str(RATE), "--data-format=LEI16@22050",
         "-o", str(out), text],
        check=True,
    )


def trim(src: Path, dst: Path) -> tuple[float, float, float]:
    """裁首尾静音，写回 dst，返回 (ms, peak_dbfs, rms_dbfs)。"""
    with wave.open(str(src), "rb") as w:
        assert w.getframerate() == SAMPLE_RATE and w.getnchannels() == 1 and w.getsampwidth() == 2
        samples = array.array("h")
        samples.frombytes(w.readframes(w.getnframes()))
    n = len(samples)
    lead = 0
    while lead < n and abs(samples[lead]) < THRESHOLD:
        lead += 1
    trail = n
    while trail > lead and abs(samples[trail - 1]) < THRESHOLD:
        trail -= 1
    pad = SAMPLE_RATE * PAD_MS // 1000
    lead = max(0, lead - pad)
    trail = min(n, trail + pad)
    body = samples[lead:trail]
    peak = max((abs(v) for v in body), default=0) / 32768.0
    rms = math.sqrt(sum(v * v for v in body) / max(1, len(body))) / 32768.0
    db = lambda x: 20 * math.log10(x) if x > 0 else -90.0
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(body.tobytes())
    return len(body) * 1000.0 / SAMPLE_RATE, db(peak), db(rms)


def main() -> int:
    manifest_path = VO / "vo_manifest.json"
    lines_path = VO / "vo_lines.tsv"
    manifest = json.loads(manifest_path.read_text())
    existing_lines = {
        row.split("\t", 1)[0]
        for row in lines_path.read_text().splitlines() if row.strip()
    }
    failures = []
    for cue_id, text in NEW_LINES:
        wav = VO / f"{cue_id}.wav"
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "raw.wav"
            try:
                synth(text, raw)
                ms, peak, rms = trim(raw, wav)
            except Exception as exc:  # noqa: BLE001 - 逐条降级，一条失败不拖垮整批
                failures.append((cue_id, str(exc)))
                continue
        manifest["items"][cue_id] = {
            "file": wav.name, "chars": len(text),
            "ms": round(ms, 1), "peak_dbfs": round(peak, 2), "rms_dbfs": round(rms, 2),
        }
        if cue_id not in existing_lines:
            with lines_path.open("a") as f:
                f.write(f"{cue_id}\t{text}\n")
        print(f"vo     {wav.name:<28} {ms:7.1f} ms  {text}")
    manifest["generated_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    if failures:
        for cue_id, err in failures:
            print(f"FAIL   {cue_id}: {err}", file=sys.stderr)
        return 1
    print(f"done: +{len(NEW_LINES)} lines -> {VO}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
