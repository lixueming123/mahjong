#!/usr/bin/env python3
"""抓取天凤牌谱（mjlog），遵守官方 `https://tenhou.net/sc/raw/` 的下载规约。

## 规约（官方页面明写，违反会被封）

1. **最短 20 分钟**一次；同一时刻只允许 **1 个会话**；
2. 只下 `*.log.gz`（压缩件），不要走「表示」的无压缩转义；
3. 先读 `list.cgi` 的 **size** 判重，大小没变就别下；
4. URL 一律从 `https://tenhou.net/sc/raw/dat/` 出发（官方会重定向/分区）；
5. **牌谱与索引禁止再配布**、禁止做「不特定多数人下载」的服务、禁止用于竞争产品的开发。

本脚本把这些都写进代码：默认 20 分钟节流（状态记在 `--out/.fetch-state.json`）、单线程顺序下载、
按 size 跳过、只取 `scc`（凤凰卓，是唯一带牌谱链接的分类）。

## 用法

    # 抓最近一天的四凤牌谱，最多 20 个半庄
    python3 game/tools/tenhou_fetch.py --out game/tools/tenhou_corpus --days 1 --limit 20

    # 强制忽略节流（本地调试用；**别在循环里用**）
    python3 game/tools/tenhou_fetch.py --out ... --limit 3 --no-throttle

    # 指定日期（归档：/sc/raw/dat/YYYY/sccYYYYMMDD.html.gz）
    python3 game/tools/tenhou_fetch.py --out ... --date 20261105 --limit 20

抓下来的文件是 `<log_id>.xml`（天凤返回的明文 XML，不压缩存放，方便直接读）。
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

BASE = "https://tenhou.net/sc/raw/"
DAT = BASE + "dat/"
INDEX = BASE + "list.cgi"
LOG = "https://tenhou.net/0/log/?"
USER_AGENT = "mahjong-tenhou-tools/1.0 (+local regression corpus)"
#: 官方要求的最短抓取间隔（秒）
MIN_INTERVAL_SECONDS = 20 * 60
#: 每次 HTTP 请求之间的礼貌间隔（同一会话内顺序下载，别打满服务器）
REQUEST_DELAY_SECONDS = 1.0


def fetch(url: str, timeout: float = 30.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def read_index(old: bool = False) -> list[tuple[str, int]]:
    """`list.cgi` → [(file, size)]。`old=True` 取历史归档（文件名带 `YYYY/` 前缀）。"""
    text = fetch(INDEX + ("?old" if old else "")).decode("utf-8", "replace")
    return [(name, int(size)) for name, size in re.findall(r"file:'([^']+)',size:(\d+)", text)]


def log_ids_of(entry: str, size: int) -> list[str]:
    """下载一个 `scc*.html.gz` 并抽出其中的牌谱 ID（只保留四凤，三凤规则不同）。"""
    body = gzip.decompress(fetch(DAT + entry)).decode("utf-8", "replace")
    ids: list[str] = []
    for line in body.splitlines():
        if "四鳳" not in line:
            continue
        match = re.search(r"log=([0-9]+gm-[0-9a-z]{4}-[0-9a-z]{4}-[0-9a-z]{8})", line)
        if match:
            ids.append(match.group(1))
    return ids


def download_log(log_id: str, out_dir: Path) -> bool:
    path = out_dir / f"{log_id}.xml"
    if path.exists() and path.stat().st_size > 0:
        return False
    data = fetch(LOG + log_id)
    if not data.startswith(b"<mjloggm"):
        raise RuntimeError(f"{log_id}: 返回的不是 mjlog（{len(data)} 字节）")
    path.write_bytes(data)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="牌谱输出目录")
    parser.add_argument("--limit", type=int, default=20, help="最多抓几个半庄（默认 20）")
    parser.add_argument("--days", type=int, default=1, help="回溯最近几个 scc 文件（默认 1）")
    parser.add_argument("--date", default=None, help="指定归档日期 YYYYMMDD（走历史归档）")
    parser.add_argument("--no-throttle", action="store_true",
                        help="忽略 20 分钟节流（仅本地调试；别在循环里用）")
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    state_path = out_dir / ".fetch-state.json"
    state = json.loads(state_path.read_text()) if state_path.exists() else {"last_fetch": 0}

    now = time.time()
    if not args.no_throttle and now - state.get("last_fetch", 0) < MIN_INTERVAL_SECONDS:
        wait = int(MIN_INTERVAL_SECONDS - (now - state["last_fetch"]))
        print(f"[fetch] 距上次抓取不足 20 分钟，跳过（还需等 {wait} 秒）。"
              f"官方规约要求最短 20 分钟一次。")
        return 0

    if args.date:
        entries = [(f"{args.date[:4]}/scc{args.date}.html.gz", 0)]
        old = True
    else:
        index = read_index(old=False)
        files = [item for item in index if Path(item[0]).name.startswith("scc")]
        entries = files[-args.days:]
        old = False
    if not entries:
        print("[fetch] 索引里没有 scc 文件")
        return 1

    taken = 0
    for entry, size in entries:
        if taken >= args.limit:
            break
        # size 判重：记下已处理过的文件大小，没变就不再下
        known = state.setdefault("sizes", {}).get(entry)
        if known == size and size != 0:
            print(f"[fetch] {entry}: size 未变（{size}），跳过")
            continue
        print(f"[fetch] {entry}（{size or '?'} 字节）")
        for log_id in log_ids_of(entry, size):
            if taken >= args.limit:
                break
            if download_log(log_id, out_dir):
                taken += 1
                print(f"  + {log_id}")
            else:
                print(f"  = {log_id}（已存在）")
            time.sleep(REQUEST_DELAY_SECONDS)
        state["sizes"][entry] = size

    state["last_fetch"] = time.time()
    state_path.write_text(json.dumps(state, indent=1))
    print(f"[fetch] 新增 {taken} 个半庄到 {out_dir}")
    if taken == 0:
        print("[fetch] 提示：scc（凤凰卓）之外的分类（sca/scb/scd）**没有牌谱链接**，抓不到牌谱。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except urllib.error.URLError as error:
        print(f"[fetch] 网络错误：{error}", file=sys.stderr)
        sys.exit(2)
