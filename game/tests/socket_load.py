#!/usr/bin/env python3
"""Real-socket load: N rooms x 4 TCP clients, each thinking before it answers.

Same decisions as room_bench's AutoClient: discard the tile just drawn (the dealer's
first discard is the first tile of game_start.hand), and pass every claim. Rooms sit
down first, then ready together, and each room stops at its first game_end.

The listen backlog is 128, so connects are capped and retried. A connection that has
not sent for 8s pings, because the server drops a silent socket at 20s.

Usage:
  python3 game/tests/socket_load.py --rooms 10000 --think-ms 1000
  python3 game/tests/socket_load.py --rooms 1 --think-ms 0
"""

from __future__ import annotations

import argparse
import asyncio
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

LOGIN = 1
PING = 3
CREATE_ROOM = 6
JOIN_ROOM = 7
READY = 8
DISCARD = 10
ACTION = 11
LOGIN_OK = 12
ERROR = 13
ROOM_JOINED = 14
GAME_START = 15
DRAW = 16
ACTION_REQUEST = 18
GAME_END = 21
PASS = 1
WINDOW_CLAIM = 2

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BIN = REPO_ROOT / "build-release" / "game" / "mahjong_server"


def field_int(text: str, key: str) -> int | None:
    token = f'"{key}":'
    at = text.find(token)
    if at < 0:
        return None
    at += len(token)
    end = at
    if end < len(text) and text[end] == "-":
        end += 1
    while end < len(text) and text[end].isdigit():
        end += 1
    if end == at:
        return None
    return int(text[at:end])


def field_str(text: str, key: str) -> str:
    token = f'"{key}":"'
    at = text.find(token)
    if at < 0:
        return ""
    at += len(token)
    end = text.find('"', at)
    if end < 0:
        return ""
    return text[at:end]


def first_hand_tile(text: str) -> str:
    at = text.find('"hand":["')
    if at < 0:
        return ""
    return text[at + 9 : at + 11]


class ProtocolError(Exception):
    pass


class Conn:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, name: str):
        self.reader = reader
        self.writer = writer
        self.name = name
        self.buf = bytearray()
        self.lock = asyncio.Lock()
        self.sent_at = time.monotonic()
        self.done = False
        self.drawn = ""
        self.first = ""

    async def send(self, frame: dict) -> None:
        payload = json.dumps(frame, separators=(",", ":")).encode() + b"\n"
        async with self.lock:
            self.writer.write(payload)
            await self.writer.drain()
            self.sent_at = time.monotonic()

    async def recv_line(self) -> str:
        while True:
            newline = self.buf.find(b"\n")
            if newline >= 0:
                line = bytes(self.buf[:newline])
                del self.buf[: newline + 1]
                return line.decode()
            chunk = await self.reader.read(65536)
            if not chunk:
                raise EOFError(self.name)
            self.buf += chunk

    async def expect(self, wanted: int, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"{self.name}: no type {wanted}")
            line = await asyncio.wait_for(self.recv_line(), remaining)
            kind = field_int(line, "type")
            if kind == ERROR:
                message = field_str(line, "message") or line[:180]
                raise ProtocolError(f"{self.name}: {message}")
            if kind == wanted:
                return line

    async def keepalive(self) -> None:
        try:
            while not self.done:
                await asyncio.sleep(8)
                if self.done:
                    return
                if time.monotonic() - self.sent_at >= 8:
                    await self.send({"type": PING})
        except Exception:
            return

    def close(self) -> None:
        self.done = True
        try:
            self.writer.close()
        except OSError:
            pass


class Stats:
    def __init__(self, rooms: int):
        self.rooms = rooms
        self.connected = 0
        self.seated = 0
        self.seat_done = 0
        self.finished = 0
        self.failed = 0
        self.samples: list[str] = []
        self.began = time.monotonic()
        self.barrier_at = 0.0
        self.play_done = 0.0

    def note(self, text: str) -> None:
        if len(self.samples) < 8:
            self.samples.append(text)


async def open_conn(port: int, name: str, limit: asyncio.Semaphore) -> Conn:
    delay = 0.02
    last: Exception | None = None
    for _ in range(400):
        async with limit:
            try:
                reader, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), 5)
                sock = writer.get_extra_info("socket")
                if sock is not None:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                return Conn(reader, writer, name)
            except (ConnectionRefusedError, OSError, asyncio.TimeoutError) as exc:
                last = exc
        await asyncio.sleep(delay)
        delay = min(delay * 1.3, 0.3)
    raise ConnectionError(f"{name}: connect failed ({last})")


async def login(conn: Conn, name: str) -> None:
    await conn.send({"type": LOGIN, "name": name})
    await conn.expect(LOGIN_OK, 30)


async def play(conn: Conn, think_s: float) -> None:
    while True:
        line = await conn.recv_line()
        kind = field_int(line, "type")
        if kind == DRAW:
            tile = field_str(line, "tile")
            if tile:
                conn.drawn = tile
            continue
        if kind == GAME_START:
            if not conn.first:
                conn.first = first_hand_tile(line)
            continue
        if kind == ACTION_REQUEST:
            if think_s > 0:
                await asyncio.sleep(think_s)
            seq = field_int(line, "seq")
            if field_int(line, "window") == WINDOW_CLAIM:
                await conn.send({"type": ACTION, "action": PASS, "tiles": [], "seq": seq})
            else:
                tile = conn.drawn or conn.first
                if not tile:
                    raise ProtocolError(f"{conn.name}: discard window without a tile")
                await conn.send({"type": DISCARD, "tile": tile, "seq": seq})
            continue
        if kind == GAME_END:
            return
        if kind == ERROR:
            message = field_str(line, "message") or line[:180]
            raise ProtocolError(f"{conn.name}: {message}")


async def one_room(
    index: int,
    port: int,
    think_s: float,
    limit: asyncio.Semaphore,
    start: asyncio.Event,
    stats: Stats,
) -> None:
    conns: list[Conn] = []
    seated = False
    open_tasks = [asyncio.create_task(open_conn(port, f"r{index}s{seat}", limit)) for seat in range(4)]
    try:
        try:
            opened = await asyncio.gather(*open_tasks)
        except Exception:
            for task in open_tasks:
                if task.done() and task.exception() is None:
                    task.result().close()
            raise
        conns.extend(opened)
        stats.connected += 4
        for conn in conns:
            asyncio.create_task(conn.keepalive())
        await asyncio.gather(*(login(conn, f"p{index * 4 + seat}") for seat, conn in enumerate(conns)))
        await conns[0].send({"type": CREATE_ROOM})
        created = await conns[0].expect(ROOM_JOINED, 30)
        room_id = field_str(created, "room_id")
        if not room_id:
            raise ProtocolError(f"r{index}: create_room returned no room_id")

        async def join(conn: Conn) -> None:
            await conn.send({"type": JOIN_ROOM, "room_id": room_id})
            await conn.expect(ROOM_JOINED, 30)

        await asyncio.gather(*(join(conn) for conn in conns[1:]))
        seated = True
        stats.seated += 1
    except Exception as exc:
        stats.note(f"r{index} seat: {exc}")
        stats.failed += 1
    finally:
        stats.seat_done += 1
        if stats.seat_done == stats.rooms:
            stats.barrier_at = time.monotonic()
            start.set()
    if not seated:
        for conn in conns:
            conn.close()
        return
    try:
        await start.wait()
        plays = [asyncio.create_task(play(conn, think_s)) for conn in conns]
        await conns[0].send({"type": READY})
        timeout = think_s * 250 + 60
        await asyncio.wait_for(asyncio.gather(*plays), timeout)
        stats.finished += 1
    except Exception as exc:
        stats.note(f"r{index} play: {exc}")
        stats.failed += 1
    finally:
        for conn in conns:
            conn.close()
        if stats.finished + stats.failed == stats.rooms:
            stats.play_done = time.monotonic()


async def report(stats: Stats, stop: asyncio.Event) -> None:
    while not stop.is_set():
        print(
            f"  connected={stats.connected} seated={stats.seated}/{stats.rooms} "
            f"game_end={stats.finished} failed={stats.failed}",
            flush=True,
        )
        try:
            await asyncio.wait_for(stop.wait(), 5)
        except asyncio.TimeoutError:
            pass


async def drive(port: int, rooms: int, think_s: float) -> int:
    stats = Stats(rooms)
    start = asyncio.Event()
    stop = asyncio.Event()
    limit = asyncio.Semaphore(32)
    reporter = asyncio.create_task(report(stats, stop))
    print(f"socket_load: {rooms} rooms, {rooms * 4} connections, think={think_s:.1f}s", flush=True)
    await asyncio.gather(*(one_room(index, port, think_s, limit, start, stats) for index in range(rooms)))
    if stats.play_done == 0:
        stats.play_done = time.monotonic()
    stop.set()
    await reporter
    seat_s = (stats.barrier_at or stats.play_done) - stats.began
    play_s = stats.play_done - stats.barrier_at if stats.barrier_at else 0
    print(
        f"socket_load: seated {stats.seated}/{rooms} in {seat_s:.1f}s, "
        f"play {play_s:.1f}s, game_end={stats.finished} failed={stats.failed}",
        flush=True,
    )
    for sample in stats.samples:
        print(f"  {sample}", flush=True)
    return 0 if stats.finished == rooms and stats.failed == 0 else 1


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_until_listening(port: int, proc: subprocess.Popen, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited {proc.returncode} before listening")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"server did not listen on {port}")


def main() -> int:
    parser = argparse.ArgumentParser(description="4-player rooms over real TCP sockets")
    parser.add_argument("--rooms", type=int, default=10000)
    parser.add_argument("--think-ms", type=int, default=1000)
    parser.add_argument("--server-bin", type=Path, default=DEFAULT_BIN)
    args = parser.parse_args()
    if args.rooms < 1:
        print("socket_load: --rooms must be >= 1", file=sys.stderr)
        return 2
    if not args.server_bin.is_file():
        print(f"socket_load: server not found: {args.server_bin}", file=sys.stderr)
        return 2

    port = free_port()
    log_path = REPO_ROOT / "build-release" / "socket_load_server.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_file = log_path.open("w")
    proc = subprocess.Popen(
        [
            str(args.server_bin),
            "--port",
            str(port),
            "--seed",
            "20251005",
            "--ai-delay-ms",
            "0",
            "--action-timeout-ms",
            "15000",
            "--quiet",
        ],
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_until_listening(port, proc)
        code = asyncio.run(drive(port, args.rooms, args.think_ms / 1000))
    except Exception as exc:
        print(f"socket_load: {exc}", file=sys.stderr, flush=True)
        code = 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        log_file.close()
    if code != 0:
        tail = log_path.read_text(errors="replace")[-2000:]
        if tail.strip():
            print(tail, file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
