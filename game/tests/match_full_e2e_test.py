#!/usr/bin/env python3
"""E2（A1_ACCEPTANCE §七）：真实生产链路 + **真实定时器** 的整场推进测试。

起源 = P0「局终后下一局不来」的复现脚本（后经用户证实为旧二进制误报），保留为常规资产并
升级为 E2 主体——它补上的正是这次漏网的覆盖缺口：**所有进程内局间推进测试都用注入调度器，
真实 WindowTimer 路径此前零覆盖**。

与 start_server.sh 同款装配：真服（可接 MySQL 账号库，缺账号库自动降级游客）+ TCP 无头客户端
（等价 pygame 客户端的帧行为：账号注册登录、局间发 stats_request / 心跳 ping）。

断言（刻意保持最小，逐帧哈希为可选不做——用户裁决）：
  * 整场走满：game_end 4..8 次（C15 双闸）后**恰好一条 match_end**；
  * M2/D2：match_end.hands_played == game_end 下发次数；renchan_count 同最后一次 game_end；
  * M7：首个局间发一帧对局帧，必须回**恰好一条** error{BAD_REQUEST}（单独计数，不算 error）；
  * M8：每个 game_end → 下一局 game_start 的间隔 ≥ --inter-hand-delay-ms（容差 50ms）；
  * C：除 M7 探针外零 error 帧；

兜底超时 = **960s**（Q3 裁决：8 局 × 120s；--match-timeout 可覆盖）。

运行：
    ./.venv/bin/python game/tests/match_full_e2e_test.py                 # 默认 5000ms 局间隔
    ./.venv/bin/python game/tests/match_full_e2e_test.py --inter-hand-delay-ms 300
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import ACTION, ERROR_CODE, FRAME_TYPE, WINDOW

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN_CANDIDATES = ("build/game/mahjong_server", "build-release/game/mahjong_server")
MATCH_TIMEOUT_S = 960.0  # Q3 裁决：整场兜底 8 × 120s
SEAT = 4


def find_binary(explicit: str | None) -> str:
    if explicit:
        if not os.path.isfile(explicit):
            raise SystemExit(f"--server-bin does not exist: {explicit}")
        return explicit
    env = os.environ.get("MAHJONG_SERVER_BIN")
    if env and os.path.isfile(env):
        return env
    for rel in BIN_CANDIDATES:
        path = os.path.join(REPO, rel)
        if os.path.isfile(path):
            return path
    raise SystemExit("cannot find mahjong_server; build it or pass --server-bin")


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Client:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""

    def send(self, frame: dict) -> None:
        self.sock.sendall(json.dumps(frame, ensure_ascii=False).encode() + b"\n")

    def recv(self, deadline: float) -> dict | None:
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.sock.settimeout(min(left, 1.0))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                raise SystemExit("server closed the connection unexpectedly")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())


def run(args: argparse.Namespace) -> int:
    binary = find_binary(args.server_bin)
    port = free_port()
    log = tempfile.NamedTemporaryFile(prefix="e2_match_", suffix=".log", delete=False)
    env = dict(os.environ, MAHJONG_MYSQL_PASSWORD=os.environ.get("MAHJONG_MYSQL_PASSWORD", "12345678"))
    command = [binary, "--port", str(port), "--seed", str(args.seed),
               "--inter-hand-delay-ms", str(args.inter_hand_delay_ms),
               "--mysql-host", args.mysql_host, "--mysql-port", str(args.mysql_port),
               "--mysql-user", args.mysql_user, "--mysql-db", args.mysql_db]
    proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
    deadline = time.monotonic() + args.match_timeout
    try:
        while time.monotonic() < min(deadline, time.monotonic() + 15):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.05)
        client = Client(port)
        username = f"e2match{int(time.monotonic()) % 1000000}"
        client.send({"type": FRAME_TYPE.REGISTER, "username": username, "password": "pw_e2match"})
        ends: list[dict] = []
        starts: list[dict] = []
        gaps_ms: list[float] = []
        m7_bad = 0
        m7_armed = False
        match_end: dict | None = None
        errors: list[dict] = []
        hand: set[str] = set()
        last_end_at: float | None = None
        while match_end is None:
            if time.monotonic() > deadline:
                raise SystemExit(f"E2 FAIL: 整场未在 {args.match_timeout:.0f}s 内结束（兜底超时）")
            frame = client.recv(deadline)
            if frame is None:
                break
            kind = frame.get("type")
            if kind == FRAME_TYPE.ERROR:
                if m7_armed and frame.get("code") == ERROR_CODE.BAD_REQUEST:
                    m7_armed = False
                    m7_bad += 1
                    continue
                errors.append(frame)
            elif kind == FRAME_TYPE.LOGIN_OK:
                client.send({"type": FRAME_TYPE.CREATE_ROOM})
            elif kind == FRAME_TYPE.ROOM_JOINED:
                client.send({"type": FRAME_TYPE.READY})
            elif kind == FRAME_TYPE.GAME_START:
                if last_end_at is not None:
                    gap_ms = (time.monotonic() - last_end_at) * 1000.0
                    gaps_ms.append(gap_ms)
                    floor = max(0.0, args.inter_hand_delay_ms - 50)
                    if gap_ms < floor:
                        raise SystemExit(f"E2 FAIL: M8 局间隔 {gap_ms:.0f}ms < {args.inter_hand_delay_ms}ms")
                    if gap_ms > 2000 + args.inter_hand_delay_ms:
                        raise SystemExit(f"E2 FAIL: M8 局间隔 {gap_ms:.0f}ms 异常长")
                starts.append(frame)
                hand = set(frame["hand"])
            elif kind == FRAME_TYPE.GAME_END:
                ends.append(frame)
                last_end_at = time.monotonic()
                if frame.get("match_will_continue") is False:
                    continue  # match_end 下一帧就到
                # M7：首个局间发一帧对局帧（服务端必处局间态），预期恰好一条 BAD_REQUEST。
                if len(ends) == 1 and not m7_armed and m7_bad == 0:
                    m7_armed = True
                    client.send({"type": FRAME_TYPE.DISCARD, "tile": "1m", "seq": 1})
                # 与 pygame 客户端一致：账号会话局间拉一次战绩 + 心跳照发（不影响判定）。
                client.send({"type": FRAME_TYPE.STATS_REQUEST})
            elif kind == FRAME_TYPE.MATCH_END:
                match_end = frame
            elif kind == FRAME_TYPE.STATS:
                continue
            elif kind == FRAME_TYPE.DRAW:
                if frame.get("seat") == 0:
                    hand.add(frame["tile"])
            elif kind == FRAME_TYPE.DISCARD_EVENT:
                if frame.get("seat") == 0:
                    hand.discard(frame["tile"])
            elif kind == FRAME_TYPE.STATE_SYNC:
                hand = set(frame["hand"])
            elif kind == FRAME_TYPE.ACTION_REQUEST:
                seq = frame["seq"]
                window = frame["window"]
                options = {o.get("action") for o in frame.get("options", [])}
                if window == WINDOW.DISCARD:
                    client.send({"type": FRAME_TYPE.DISCARD, "tile": sorted(hand)[-1], "seq": seq})
                elif ACTION.WIN in options:
                    client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.WIN, "tiles": [], "seq": seq})
                else:
                    client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.PASS, "tiles": [], "seq": seq})
        if match_end is None:
            raise SystemExit("E2 FAIL: stream ended without match_end")
        if errors:
            raise SystemExit(f"E2 FAIL: unexpected error frames: {errors[:3]}")
        ends_count = len(ends)
        if not 4 <= ends_count <= 8:
            raise SystemExit(f"E2 FAIL: game_end 次数 {ends_count} ∉ [4,8]（C15 双闸）")
        if match_end.get("hands_played") != ends_count:
            raise SystemExit(f"E2 FAIL: M2/D2 hands_played={match_end.get('hands_played')} != {ends_count}")
        if match_end.get("renchan_count") != ends[-1].get("renchan_count"):
            raise SystemExit("E2 FAIL: M2 renchan_count 与最后一次 game_end 不一致")
        if m7_bad != 1:
            raise SystemExit(f"E2 FAIL: M7 局间 BAD_REQUEST 探针应恰 1 次，got {m7_bad}")
        print(f"[e2-match] PASS: rounds={ends_count} hands_played={match_end['hands_played']} "
              f"renchan={match_end['renchan_count']} m7_bad={m7_bad} "
              f"gaps_ms={[round(g) for g in gaps_ms]} (真实定时器, delay={args.inter_hand_delay_ms}ms)")
        return 0
    finally:
        proc.send_signal(2)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        log.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server-bin", default=None)
    parser.add_argument("--seed", type=int, default=20251005)
    parser.add_argument("--inter-hand-delay-ms", type=int, default=5000,
                        help="服务端局间隔；M8 断言每个 game_end→game_start 间隔 ≥ 该值（默认 5000）")
    parser.add_argument("--match-timeout", type=float, default=MATCH_TIMEOUT_S,
                        help=f"整场兜底超时秒数（默认 {MATCH_TIMEOUT_S:.0f} = Q3 裁决 8×120s）")
    parser.add_argument("--mysql-host", default="127.0.0.1")
    parser.add_argument("--mysql-port", type=int, default=3306)
    parser.add_argument("--mysql-user", default="root")
    parser.add_argument("--mysql-db", default="mahjong")
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
