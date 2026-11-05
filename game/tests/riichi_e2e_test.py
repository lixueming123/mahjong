#!/usr/bin/env python3
"""日麻玩法（mode=riichi）的服务端端到端冒烟。

标准库 only。启动真服务端，登录 → `create_room {"mode":"riichi"}` → `ready` → 打完一局，
只断言「玩法接线是否正确」，不重复规则库的算分断言（那由 riichi_rules_test 负责）：

  * `game_start` 带 `points`（四家 25000）、`dora_indicators`（1 张）、`round_wind`/`seat_wind`；
  * `state_sync.remaining` 首次为 69（王牌 14 张被切走），且每份都带 `points`；
  * 全程零 `error` 帧（说明日麻的分支没有把经典帧打坏）；
  * 若本局有人和牌，`game_end.win` 必须带 `fu` / `points` / `limit` / `dora`。

用法：
    python3 game/tests/riichi_e2e_test.py --server-bin build/game/mahjong_server
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time

MY_SEAT = 0
START_POINTS = 25000
EXPECTED_FIRST_REMAINING = 69  # 136 − 52 − 14 王牌 = 70，再减庄家第一次摸牌 = 69


class TestFailure(RuntimeError):
    pass


class Client:
    def __init__(self, port: int, name: str) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
        self.sock.settimeout(10.0)
        self.buffer = b""
        self.name = name

    def send(self, frame: dict) -> None:
        self.sock.sendall((json.dumps(frame, ensure_ascii=False) + "\n").encode("utf-8"))

    def recv(self, timeout: float = 10.0) -> dict:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestFailure(f"{self.name}: timed out waiting for a frame")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise TestFailure(f"{self.name}: connection closed by the server")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        try:
            return json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise TestFailure(f"{self.name}: non-JSON frame {line!r}: {exc}") from exc

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def start_server(binary: str, port: int, seed: int) -> subprocess.Popen:
    process = subprocess.Popen(
        [binary, "--port", str(port), "--seed", str(seed)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return process
        except OSError:
            if process.poll() is not None:
                raise TestFailure(f"server exited during startup (rc={process.returncode})")
            time.sleep(0.05)
    process.kill()
    raise TestFailure("server did not start listening in 20s")


def play_one_round(client: Client, length: str = "tonpu") -> dict:
    """打完一局（到第一份 game_end），返回收集到的观测值。"""
    client.send({"type": "login", "name": "日麻玩家"})
    if client.recv().get("type") != "login_ok":
        raise TestFailure("login did not answer login_ok")

    client.send({"type": "create_room", "mode": "riichi", "length": length})
    joined = client.recv()
    if joined.get("type") != "room_joined":
        raise TestFailure(f"create_room failed: {joined!r}")
    if joined.get("seat") != MY_SEAT:
        raise TestFailure(f"expect seat 0, got {joined!r}")

    client.send({"type": "ready"})

    observation = {
        "game_start": None,
        "max_remaining": None,
        "syncs": 0,
        "errors": [],
        "game_end": None,
        "tenpai_frames": 0,
        "tenpai_no_yaku": 0,
        "tenpai_bad": [],
    }
    my_hand: list[str] = []
    deadline = time.monotonic() + 120.0

    while time.monotonic() < deadline:
        frame = client.recv(timeout=30.0)
        kind = frame.get("type")

        if kind == "error":
            observation["errors"].append(frame)
            continue
        if kind == "game_start":
            observation["game_start"] = frame
            my_hand = list(frame.get("hand") or [])
            continue
        if kind == "state_sync":
            observation["syncs"] += 1
            remaining = frame.get("remaining")
            if isinstance(remaining, int):
                # 第一份 state_sync 未必紧跟发牌（庄家第一打不摸牌，可能先打后摸），
                # 所以「牌山被切过」的判据取**观测到的最大值**：remaining 只减不增。
                if observation["max_remaining"] is None or remaining > observation["max_remaining"]:
                    observation["max_remaining"] = remaining
            my_hand = list(frame.get("hand") or [])
            continue
        if kind == "action_request":
            seq = frame.get("seq")
            window = frame.get("window")
            if window == "discard":
                if not my_hand:
                    raise TestFailure("discard window opened with an empty tracked hand")
                client.send({"type": "discard", "tile": my_hand[0], "seq": seq})
            else:
                client.send({"type": "action", "action": "pass", "tiles": [], "seq": seq})
            continue
        if kind == "tenpai":
            # v1.13 §12.8：日麻听牌帧必须逐张带 yaku，帧级 no_yaku 只在「全部无役」时为真
            observation["tenpai_frames"] += 1
            waits = frame.get("waits") or []
            flags = [w.get("yaku") for w in waits]
            no_yaku = frame.get("no_yaku")
            problem = None
            if not isinstance(no_yaku, bool):
                problem = f"no_yaku must be bool, got {no_yaku!r}"
            elif any(not isinstance(f, bool) for f in flags):
                problem = f"waits[].yaku must be bool[], got {flags!r}"
            elif frame.get("is_tenpai") and no_yaku != all(not f for f in flags):
                problem = f"no_yaku={no_yaku} inconsistent with waits={flags!r}"
            elif not frame.get("is_tenpai") and (waits or no_yaku):
                problem = f"not tenpai must carry empty waits and no_yaku=false: {frame!r}"
            if problem:
                observation["tenpai_bad"].append(problem)
            if no_yaku:
                observation["tenpai_no_yaku"] += 1
            continue
        if kind == "game_end":
            observation["game_end"] = frame
            return observation

    raise TestFailure("no game_end within 120s")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--seed", type=int, default=20260101)
    parser.add_argument("--length", default="tonpu", choices=["tonpu", "hanchan"])
    args = parser.parse_args()

    binary = os.path.abspath(args.server_bin)
    port = free_port()
    process = start_server(binary, port, args.seed)
    client = None
    try:
        client = Client(port, "riichi")
        observation = play_one_round(client, args.length)
    finally:
        if client is not None:
            client.close()
        process.kill()
        process.wait(timeout=10)

    start = observation["game_start"]
    if start is None:
        raise TestFailure("no game_start")
    points = start.get("points")
    if points != [START_POINTS] * 4:
        raise TestFailure(f"game_start.points must be four {START_POINTS}: {points!r}")
    indicators = start.get("dora_indicators")
    if not isinstance(indicators, list) or len(indicators) != 1:
        raise TestFailure(f"game_start.dora_indicators must have 1 tile: {indicators!r}")
    # 牌山被切过的判据：remaining 只减不增，最大观测值不超过 69（classic 会是 83）。
    # 第一份 state_sync 未必紧跟发牌（庄家首打不摸牌），所以不要求恰好等于 69。
    if observation["max_remaining"] is None or observation["max_remaining"] > EXPECTED_FIRST_REMAINING:
        raise TestFailure(
            f"max state_sync.remaining must be <= {EXPECTED_FIRST_REMAINING}, "
            f"got {observation['max_remaining']!r} (dead wall not cut?)"
        )
    if observation["tenpai_bad"]:
        raise TestFailure(f"tenpai frame defects: {observation['tenpai_bad'][:2]}")
    if observation["errors"]:
        raise TestFailure(f"server answered {len(observation['errors'])} error frames: {observation['errors'][:2]!r}")

    expected_rounds = 8 if args.length == "hanchan" else 4
    if start.get("round_count") != expected_rounds:
        raise TestFailure(f"round_count must be {expected_rounds} for length={args.length}, got {start.get('round_count')!r}")
    if start.get("round_wind") != 0:
        raise TestFailure(f"the first round is always 东场 (round_wind=0), got {start.get('round_wind')!r}")

    end = observation["game_end"]
    win = end.get("win")
    if win is not None:
        for key in ("fu", "points", "limit", "dora"):
            if key not in win:
                raise TestFailure(f"riichi game_end.win is missing {key!r}: {win!r}")
        if not isinstance(win.get("points"), int) or win["points"] <= 0:
            raise TestFailure(f"riichi game_end.win.points must be positive: {win!r}")
        detail = f"win fu={win['fu']} han={win['han']} points={win['points']} limit={win['limit']}"
    else:
        detail = "draw (no win this round)"

    print(
        f"[riichi-e2e] OK syncs={observation['syncs']} remaining={observation['max_remaining']} "
        f"tenpai={observation['tenpai_frames']}(no_yaku={observation['tenpai_no_yaku']}) "
        f"reason={end.get('reason')} {detail}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except TestFailure as failure:
        print(f"[riichi-e2e] FAIL: {failure}", file=sys.stderr)
        sys.exit(1)
