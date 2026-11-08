#!/usr/bin/env python3
"""End-to-end check for PROTOCOL.md §5.17 `room_update` (v1.23).

Starts a compiled mahjong_server on an ephemeral port and drives two raw NDJSON
connections: one owner creates a waiting room, a second human joins it, and both
must receive the broadcast seat roster. When the joiner disconnects before
`ready`, the owner must see that seat turn back into an AI.

Usage: python3 game/tests/room_update_e2e_test.py [--server-bin PATH] [-v]
"""
from __future__ import annotations

import argparse
import json
import os
import random
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import FRAME_TYPE

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BIN_CANDIDATES = (
    "build/game/mahjong_server",
    "build-release/game/mahjong_server",
    "build/game/Debug/mahjong_server",
    "build/game/Release/mahjong_server",
)


class TestFailure(AssertionError):
    pass


class Client:
    """One TCP connection with NDJSON framing helpers."""

    def __init__(self, port: int, name: str, timeout: float = 5.0):
        self.name = name
        self.buffer = b""
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)

    def send(self, frame: dict) -> None:
        payload = json.dumps(frame, ensure_ascii=False).encode("utf-8") + b"\n"
        self.sock.sendall(payload)

    def recv(self, timeout: float = 5.0) -> dict:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestFailure(f"{self.name}: timed out, buffered={self.buffer!r}")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout as exc:
                raise TestFailure(f"{self.name}: timed out, buffered={self.buffer!r}") from exc
            if not chunk:
                raise TestFailure(f"{self.name}: connection closed, buffered={self.buffer!r}")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def expect(self, expected: int, timeout: float = 5.0) -> dict:
        frame = self.recv(timeout)
        if frame.get("type") != expected:
            raise TestFailure(f"{self.name}: expected {expected!r}, got {frame!r}")
        return frame

    def close(self) -> None:
        self.sock.close()


def check(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)
    print(f"  ok   {message}")


def find_binary(explicit: str | None) -> str:
    candidates = [explicit] if explicit else [os.path.join(REPO_ROOT, path) for path in DEFAULT_BIN_CANDIDATES]
    for path in candidates:
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return os.path.abspath(path)
    raise SystemExit("mahjong_server not found; build it first (cmake --build build --target mahjong_server)")


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_for_server(port: int, process: subprocess.Popen, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise SystemExit(f"server exited early with code {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise SystemExit(f"server did not listen on port {port} within {timeout}s")


def seat_of(frame: dict, name: str) -> dict | None:
    for seat in frame.get("seats", []):
        if seat.get("name") == name:
            return seat
    return None


def run(port: int) -> None:
    owner = Client(port, "房主")
    guest = Client(port, "好友")
    try:
        owner.send({"type": FRAME_TYPE.LOGIN, "name": "房主"})
        owner.expect(FRAME_TYPE.LOGIN_OK)
        owner.send({"type": FRAME_TYPE.CREATE_ROOM})
        created = owner.expect(FRAME_TYPE.ROOM_JOINED)
        room_id = created["room_id"]
        check(created["seat"] == 0, "房主坐 0 号位")
        check(len(created["seats"]) == 4, "初始四席位")

        guest.send({"type": FRAME_TYPE.LOGIN, "name": "好友"})
        guest.expect(FRAME_TYPE.LOGIN_OK)
        guest.send({"type": FRAME_TYPE.JOIN_ROOM, "room_id": room_id})
        joined = guest.expect(FRAME_TYPE.ROOM_JOINED)
        guest_seat = joined["seat"]
        check(guest_seat >= 1, f"好友入座 {guest_seat} 号位")

        # §5.17：房主（先到的真人）必须收到广播，而不是继续显示 3 个 AI
        owner_update = owner.expect(FRAME_TYPE.ROOM_UPDATE)
        check(owner_update["room_id"] == room_id, "广播带正确 room_id")
        check(len(owner_update["seats"]) == 4, "广播四席位")
        joined_seat = seat_of(owner_update, "好友")
        check(joined_seat is not None and joined_seat["is_bot"] is False,
              "房主看到好友昵称且不是 AI")
        check(joined_seat is not None and joined_seat["seat"] == guest_seat,
              "好友座位号与 room_joined 一致")

        # 新加入者也会收到同一份快照（幂等渲染）
        guest_update = guest.expect(FRAME_TYPE.ROOM_UPDATE)
        check(guest_update["seats"] == owner_update["seats"], "两位真人收到同一份座位快照")

        guest.close()
        # 未开局的非房主断开：座位恢复 AI，房主同样要收到广播
        left = owner.expect(FRAME_TYPE.ROOM_UPDATE)
        check(left["room_id"] == room_id, "离开广播带正确 room_id")
        freed = next((seat for seat in left["seats"] if seat["seat"] == guest_seat), None)
        check(freed is not None and freed["is_bot"] is True, "好友离开后座位恢复为 AI")
        check(seat_of(left, "好友") is None, "离开广播里不再有好友昵称")
    finally:
        owner.close()
        try:
            guest.close()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-bin", default=None, help="path to the mahjong_server binary")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = find_binary(args.server_bin)
    port = free_port()
    log_file = tempfile.NamedTemporaryFile(prefix="room_update_server_", suffix=".log", delete=False)
    print(f"[room-update] binary={binary} port={port}")
    process = subprocess.Popen(
        [binary, "--port", str(port), "--seed", str(random.randint(1, 2**31 - 1))],
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_for_server(port, process)
        started = time.monotonic()
        run(port)
        print(f"[room-update] ALL PASS ({time.monotonic() - started:.2f}s)")
        if process.poll() is not None:
            print("[room-update] FAIL  server exited during the test")
            return 1
        return 0
    except (TestFailure, SystemExit) as exc:
        print(f"[room-update] FAIL  {exc}")
        return 1
    finally:
        process.send_signal(2)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if args.verbose:
            with open(log_file.name, "r", encoding="utf-8", errors="replace") as handle:
                print(handle.read().strip())
        log_file.close()


if __name__ == "__main__":
    sys.exit(main())
