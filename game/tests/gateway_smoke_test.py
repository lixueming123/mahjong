#!/usr/bin/env python3
"""TCP JSON gateway smoke test for the mahjong server (task t3).

Standard library only (socket/json/subprocess). It starts the compiled server on an
ephemeral port and drives raw NDJSON frames against it, asserting the frozen behaviour of
game/docs/PROTOCOL.md:

  * normal login -> login_ok{player_id,name} + ping/pong;
  * NOT_LOGGED_IN for any non-login frame before login;
  * malformed JSON / unknown type -> error{BAD_REQUEST} with the connection kept;
  * invalid UTF-8 -> error{BAD_REQUEST} with the connection kept;
  * a frame larger than 64 KB (with and without a terminator) drops that connection only;
  * two concurrent connections do not interfere (separate players, separate rooms);
  * quota bookkeeping: the 10th bad frame and the 4th invalid name drop the connection;
  * a >500 frames/second burst trips RATE_LIMITED.

Usage:
  python3 game/tests/gateway_smoke_test.py [--server-bin PATH] [--keep-server] [-v]
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import ACTION, ERROR_CODE, FRAME_TYPE, WINDOW

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BIN_CANDIDATES = (
    "build/game/mahjong_server",
    "build-release/game/mahjong_server",
    "build/game/Debug/mahjong_server",
    "build/game/Release/mahjong_server",
)
MAX_FRAME_BYTES = 65536
# PROTOCOL.md §4.2: room ids are "r-" plus 13 lowercase base-36 digits.
ROOM_ID_PATTERN = re.compile(r"r-[0-9a-z]{13}")


class TestFailure(AssertionError):
    pass


class Client:
    """One TCP connection with NDJSON framing helpers."""

    def __init__(self, port: int, name: str = "client", timeout: float = 5.0,
                 recv_buffer_bytes: int | None = None):
        self.name = name
        self.buffer = b""
        if recv_buffer_bytes is None:
            self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        else:
            # SO_RCVBUF only affects the window that is advertised during the handshake, so it has
            # to be set on the socket before connect(). A small value is what makes the server's
            # write fill up quickly instead of after hundreds of kilobytes.
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, recv_buffer_bytes)
            self.sock.settimeout(timeout)
            self.sock.connect(("127.0.0.1", port))
        self.sock.settimeout(timeout)

    def send_raw(self, payload: bytes) -> None:
        self.sock.sendall(payload)

    def send(self, obj_or_text) -> None:
        if isinstance(obj_or_text, (bytes, bytearray)):
            self.send_raw(bytes(obj_or_text))
            return
        if isinstance(obj_or_text, str):
            payload = obj_or_text.encode("utf-8")
        else:
            payload = json.dumps(obj_or_text, ensure_ascii=False).encode("utf-8")
        if not payload.endswith(b"\n"):
            payload += b"\n"
        self.send_raw(payload)

    def send_frame(self, frame: dict) -> None:
        self.send(json.dumps(frame, ensure_ascii=False))

    def recv_frame(self, timeout: float = 5.0) -> dict:
        """Reads one '\n' terminated frame; raises TestFailure on EOF/timeout."""
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestFailure(f"{self.name}: timed out waiting for a frame; buffered={self.buffer!r}")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise TestFailure(f"{self.name}: timed out waiting for a frame; buffered={self.buffer!r}")
            except ConnectionResetError:
                raise TestFailure(f"{self.name}: connection reset; buffered={self.buffer!r}")
            if not chunk:
                raise TestFailure(f"{self.name}: connection closed; buffered={self.buffer!r}")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        try:
            return json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise TestFailure(f"{self.name}: server sent a non-JSON frame {line!r}: {exc}")

    def expect_type(self, expected: int, timeout: float = 5.0) -> dict:
        frame = self.recv_frame(timeout)
        if frame.get("type") != expected:
            raise TestFailure(f"{self.name}: expected type={expected!r}, got {frame!r}")
        return frame

    def expect_error(self, code: int, timeout: float = 5.0) -> dict:
        frame = self.expect_type(FRAME_TYPE.ERROR, timeout)
        if frame.get("code") != code:
            raise TestFailure(f"{self.name}: expected error code {code!r}, got {frame!r}")
        if not isinstance(frame.get("message"), str) or not frame["message"]:
            raise TestFailure(f"{self.name}: error frame needs a non-empty message: {frame!r}")
        return frame

    def expect_no_frame(self, timeout: float = 0.4) -> None:
        """Asserts nothing arrives within the timeout (uses a short socket timeout)."""
        deadline = time.monotonic() + timeout
        while True:
            if b"\n" in self.buffer:
                line, _, _ = self.buffer.partition(b"\n")
                raise TestFailure(f"{self.name}: unexpected frame {line!r}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return
            except ConnectionResetError:
                raise TestFailure(f"{self.name}: connection reset while expecting silence")
            if not chunk:
                raise TestFailure(f"{self.name}: connection closed while expecting silence")
            self.buffer += chunk

    def expect_closed(self, timeout: float = 5.0) -> bytes:
        """Drains until EOF and returns whatever was buffered before the close."""
        collected = self.buffer
        self.buffer = b""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestFailure(f"{self.name}: server did not close the connection")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise TestFailure(f"{self.name}: server did not close the connection")
            except ConnectionResetError:
                return collected
            if not chunk:
                return collected
            collected += chunk

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def frames_of(payload: bytes) -> list:
    frames = []
    for line in payload.split(b"\n"):
        if not line:
            continue
        try:
            frames.append(json.loads(line.decode("utf-8")))
        except (UnicodeDecodeError, json.JSONDecodeError):
            frames.append({"type": "<not-json>", "raw": line.decode("utf-8", "replace")})
    return frames


def login(client: Client, name: str = "玩家_") -> dict:
    client.send_frame({"type": FRAME_TYPE.LOGIN, "name": name})
    frame = client.expect_type(FRAME_TYPE.LOGIN_OK)
    if not frame.get("player_id"):
        raise TestFailure(f"{client.name}: login_ok.player_id must be non-empty: {frame!r}")
    if frame.get("name") != name:
        raise TestFailure(f"{client.name}: login_ok.name mismatch: {frame!r}")
    if set(frame) != {"type", "player_id", "name"}:
        raise TestFailure(f"{client.name}: login_ok has unexpected fields: {frame!r}")
    return frame


# ---------------------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------------------


def test_login_ping_and_frame_shape(port: int) -> None:
    client = Client(port, "login")
    try:
        frame = login(client)
        if not isinstance(frame["player_id"], str):
            raise TestFailure("player_id must be a string")
        client.send({"type": FRAME_TYPE.PING})
        client.expect_type(FRAME_TYPE.PONG)
        # Two frames in one TCP segment must both be processed, in order.
        client.send(b'{"type":%d}\n{"type":%d}\n' % (FRAME_TYPE.PING, FRAME_TYPE.PING))
        client.expect_type(FRAME_TYPE.PONG)
        client.expect_type(FRAME_TYPE.PONG)
        # A second login on the same connection is a BAD_REQUEST (§4.1) and keeps it open.
        client.send({"type": FRAME_TYPE.LOGIN, "name": "重复登录"})
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send({"type": FRAME_TYPE.PING})
        client.expect_type(FRAME_TYPE.PONG)
    finally:
        client.close()


def test_frames_split_across_segments(port: int) -> None:
    """A frame may arrive in arbitrary TCP chunks; NDJSON framing must reassemble it."""
    client = Client(port, "split-frame")
    try:
        login(client, "分片帧")
        # 1. Byte-by-byte delivery of a short frame.
        for byte in b'{"type":%d}\n' % FRAME_TYPE.PING:
            client.send_raw(bytes([byte]))
        client.expect_type(FRAME_TYPE.PONG)
        # 2. A >16 KB frame split into three writes (forces several read() calls).
        big = json.dumps({"type": FRAME_TYPE.PING, "pad": "b" * 40000}).encode("utf-8") + b"\n"
        third = len(big) // 3
        for chunk in (big[:third], big[third:2 * third], big[2 * third:]):
            client.send_raw(chunk)
            time.sleep(0.05)
        client.expect_type(FRAME_TYPE.PONG)
    finally:
        client.close()


def test_not_logged_in(port: int) -> None:
    client = Client(port, "not-logged-in")
    try:
        client.send({"type": FRAME_TYPE.PING})
        client.expect_error(ERROR_CODE.NOT_LOGGED_IN)
        client.send({"type": FRAME_TYPE.CREATE_ROOM})
        client.expect_error(ERROR_CODE.NOT_LOGGED_IN)
        # The connection survives and can still log in.
        login(client, "迟到玩家")
    finally:
        client.close()


def test_invalid_name_quota(port: int) -> None:
    client = Client(port, "invalid-name")
    try:
        client.send({"type": FRAME_TYPE.LOGIN, "name": ""})
        client.expect_error(ERROR_CODE.INVALID_NAME)
        client.send({"type": FRAME_TYPE.LOGIN, "name": "x" * 17})
        client.expect_error(ERROR_CODE.INVALID_NAME)
        # The 4th invalid name drops the connection (§1.7).
        client.send({"type": FRAME_TYPE.LOGIN, "name": "\u0007bad"})
        client.expect_error(ERROR_CODE.INVALID_NAME)
        client.send({"type": FRAME_TYPE.LOGIN, "name": ""})
        client.expect_error(ERROR_CODE.INVALID_NAME)
        payload = client.expect_closed()
        if payload:
            raise TestFailure(f"invalid-name quota: unexpected trailing payload {payload!r}")
    finally:
        client.close()

    client = Client(port, "invalid-name-then-ok")
    try:
        # Three invalid names keep the connection usable.
        for _ in range(3):
            client.send({"type": FRAME_TYPE.LOGIN, "name": "x" * 17})
            client.expect_error(ERROR_CODE.INVALID_NAME)
        login(client, "正常玩家")
    finally:
        client.close()


def test_invalid_json_keeps_connection(port: int) -> None:
    client = Client(port, "bad-json")
    try:
        client.send(b'{"type":%d,\n' % FRAME_TYPE.LOGIN)
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send("{not json at all}\n")
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send("[]\n")  # valid JSON but not an object
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send('{"name":"no-type"}\n')  # missing "type"
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send('{"type":42}\n')  # non-string "type"
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        login(client, "坏帧之后")
    finally:
        client.close()


def test_unknown_type(port: int) -> None:
    client = Client(port, "unknown-type")
    try:
        login(client, "未知类型")
        client.send({"type": "definitely_not_a_frame"})
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send({"type": FRAME_TYPE.PING})
        client.expect_type(FRAME_TYPE.PONG)
    finally:
        client.close()


def test_bad_frame_quota(port: int) -> None:
    client = Client(port, "bad-frame-quota")
    try:
        login(client, "坏帧预算")
        for _ in range(9):
            client.send({"type": "nope"})
            client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send({"type": "nope"})
        client.expect_error(ERROR_CODE.BAD_REQUEST)  # 10th bad frame: error, then close
        payload = client.expect_closed()
        frames = frames_of(payload)
        for frame in frames:
            if frame.get("type") != FRAME_TYPE.ERROR or frame.get("code") != ERROR_CODE.BAD_REQUEST:
                raise TestFailure(f"bad-frame quota: unexpected trailing frame {frame!r}")
    finally:
        client.close()


def test_invalid_utf8(port: int) -> None:
    client = Client(port, "bad-utf8")
    try:
        client.send_raw(b'{"type":%d,"name":"\xff\xfe"}\n' % FRAME_TYPE.LOGIN)
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        client.send_raw(b'{"type":%d,"name":"\xc0\xaf"}\n' % FRAME_TYPE.LOGIN)  # overlong encoding
        client.expect_error(ERROR_CODE.BAD_REQUEST)
        login(client, "UTF8之后")
    finally:
        client.close()


def test_oversized_frame(port: int) -> None:
    # 1. Oversized but terminated: FRAME_TOO_LARGE (best effort) then close.
    client = Client(port, "oversized-terminated")
    try:
        login(client, "超长帧")
        padding = "a" * (MAX_FRAME_BYTES + 1024)
        client.send({"type": FRAME_TYPE.PING, "pad": padding})
        payload = client.expect_closed()
        frames = frames_of(payload)
        if frames and frames[0].get("code") != ERROR_CODE.FRAME_TOO_LARGE:
            raise TestFailure(f"oversized frame: expected FRAME_TOO_LARGE, got {frames!r}")
    finally:
        client.close()

    # 2. Oversized without any terminator: drop the connection, never buffer without bound.
    client = Client(port, "oversized-unterminated")
    try:
        client.send_raw(b"a" * (MAX_FRAME_BYTES + 4096))
        payload = client.expect_closed()
        frames = frames_of(payload)
        if frames and frames[0].get("code") != ERROR_CODE.FRAME_TOO_LARGE:
            raise TestFailure(f"unterminated oversized frame: expected FRAME_TOO_LARGE, got {frames!r}")
    finally:
        client.close()


def test_two_connections_are_isolated(port: int) -> None:
    alpha = Client(port, "alpha")
    beta = Client(port, "beta")
    try:
        alpha_frame = login(alpha, "甲玩家")
        beta_frame = login(beta, "乙玩家")
        if alpha_frame["player_id"] == beta_frame["player_id"]:
            raise TestFailure("two connections must get distinct player ids")

        alpha.send({"type": FRAME_TYPE.CREATE_ROOM})
        joined = alpha.expect_type(FRAME_TYPE.ROOM_JOINED)
        if not isinstance(joined.get("room_id"), str) or not ROOM_ID_PATTERN.fullmatch(joined["room_id"]):
            raise TestFailure(f"room_joined.room_id must look like r-<13 base36>: {joined!r}")
        if joined.get("seat") != 0:
            raise TestFailure(f"create_room must seat the caller at 0: {joined!r}")
        seats = joined.get("seats")
        if not isinstance(seats, list) or len(seats) != 4:
            raise TestFailure(f"room_joined.seats must hold 4 entries: {joined!r}")
        for index, seat in enumerate(seats):
            if seat.get("seat") != index or "hand" in seat or set(seat) != {"seat", "name", "is_bot"}:
                raise TestFailure(f"room_joined.seats[{index}] malformed: {seat!r}")
        if seats[0].get("is_bot") is not False or any(seat.get("is_bot") is not True for seat in seats[1:]):
            raise TestFailure(f"seat 0 must be the human and 1..3 bots: {seats!r}")

        # beta must see nothing of alpha's room and must still be able to talk.
        beta.expect_no_frame()
        alpha.send({"type": FRAME_TYPE.CREATE_ROOM})
        alpha.expect_error(ERROR_CODE.BAD_REQUEST)  # already in a room
        beta.send({"type": FRAME_TYPE.PING})
        beta.expect_type(FRAME_TYPE.PONG)

        # Closing beta must not disturb alpha.
        beta.close()
        time.sleep(0.2)
        alpha.send({"type": FRAME_TYPE.PING})
        alpha.expect_type(FRAME_TYPE.PONG)
    finally:
        alpha.close()
        beta.close()


def test_room_not_found(port: int) -> None:
    owner = Client(port, "room-owner")
    client = Client(port, "room-lookup")
    late = Client(port, "room-late")
    try:
        login(client, "找房间")
        client.send({"type": FRAME_TYPE.JOIN_ROOM, "room_id": "r-9999"})
        client.expect_error(ERROR_CODE.ROOM_NOT_FOUND)
        client.send({"type": FRAME_TYPE.READY})  # not in a room yet
        client.expect_error(ERROR_CODE.ROOM_NOT_FOUND)
        client.send({"type": FRAME_TYPE.JOIN_ROOM, "room_id": 7})
        client.expect_error(ERROR_CODE.BAD_REQUEST)

        # Before ready, joining a live room takes an AI seat (PROTOCOL §4.3).
        login(owner, "房主")
        owner.send({"type": FRAME_TYPE.CREATE_ROOM})
        room_id = owner.expect_type(FRAME_TYPE.ROOM_JOINED)["room_id"]
        client.send({"type": FRAME_TYPE.JOIN_ROOM, "room_id": room_id})
        joined = client.expect_type(FRAME_TYPE.ROOM_JOINED)
        seat = joined.get("seat")
        if joined.get("room_id") != room_id or not isinstance(seat, int) or seat < 1:
            raise TestFailure(f"join before ready must return room_joined seat>=1: {joined!r}")

        # PROTOCOL §5.17 (v1.23): a pre-game join broadcasts one room_update to every
        # human in the room. Both connections must consume it before the game_start frames.
        owner_update = owner.expect_type(FRAME_TYPE.ROOM_UPDATE)
        client_update = client.expect_type(FRAME_TYPE.ROOM_UPDATE)
        if owner_update.get("room_id") != room_id or client_update.get("room_id") != room_id:
            raise TestFailure(f"room_update.room_id mismatch: {owner_update!r} / {client_update!r}")
        if owner_update.get("seats") != client_update.get("seats"):
            raise TestFailure(f"room_update rosters must match: {owner_update!r} / {client_update!r}")
        if not any(s.get("name") == "找房间" and s.get("is_bot") is False
                   for s in owner_update.get("seats", [])):
            raise TestFailure(f"owner must see the newcomer in room_update: {owner_update!r}")

        owner.send({"type": FRAME_TYPE.READY})
        owner.expect_type(FRAME_TYPE.GAME_START)
        client.expect_type(FRAME_TYPE.GAME_START)

        login(late, "后来者")
        late.send({"type": FRAME_TYPE.JOIN_ROOM, "room_id": room_id})
        late.expect_error(ERROR_CODE.ROOM_NOT_FOUND)
        late.send({"type": FRAME_TYPE.PING})
        late.expect_type(FRAME_TYPE.PONG)
    finally:
        client.close()
        owner.close()
        late.close()


def test_error_codes_are_from_the_frozen_table(port: int) -> None:
    frozen_codes = {
        ERROR_CODE.INVALID_NAME,
        ERROR_CODE.NOT_LOGGED_IN,
        ERROR_CODE.ROOM_NOT_FOUND,
        ERROR_CODE.NOT_YOUR_TURN,
        ERROR_CODE.NO_PENDING_ACTION,
        ERROR_CODE.ILLEGAL_ACTION,
        ERROR_CODE.UNKNOWN_TILE,
        ERROR_CODE.BAD_REQUEST,
        ERROR_CODE.FRAME_TOO_LARGE,
        ERROR_CODE.RATE_LIMITED,
        ERROR_CODE.INTERNAL,
    }
    client = Client(port, "error-codes")
    seen = []
    try:
        login(client, "错误码")
        # Connection-level validation happens before any room lookup.
        client.send({"type": FRAME_TYPE.DISCARD, "tile": "10z", "seq": 1})
        seen.append(client.expect_error(ERROR_CODE.UNKNOWN_TILE)["code"])
        client.send({"type": FRAME_TYPE.DISCARD, "tile": "3m"})  # missing seq
        seen.append(client.expect_error(ERROR_CODE.BAD_REQUEST)["code"])
        client.send({"type": FRAME_TYPE.ACTION, "action": "dance", "tiles": [], "seq": 1})
        seen.append(client.expect_error(ERROR_CODE.BAD_REQUEST)["code"])
        # "5x" is not a tile in either the 27-face or the 34-face encoding (a bare "1z"
        # would become legal once the honour tiles land).
        client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.PONG, "tiles": ["5x"], "seq": 1})
        seen.append(client.expect_error(ERROR_CODE.UNKNOWN_TILE)["code"])
        client.send({"type": FRAME_TYPE.DISCARD, "tile": "5x", "seq": 1})
        seen.append(client.expect_error(ERROR_CODE.UNKNOWN_TILE)["code"])
        client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.PONG, "tiles": "9s", "seq": 1})  # tiles not an array
        seen.append(client.expect_error(ERROR_CODE.BAD_REQUEST)["code"])
        # Room-scoped intents without a room are rejected with ROOM_NOT_FOUND.
        client.send({"type": FRAME_TYPE.READY})
        seen.append(client.expect_error(ERROR_CODE.ROOM_NOT_FOUND)["code"])
        client.send({"type": FRAME_TYPE.SYNC})
        seen.append(client.expect_error(ERROR_CODE.ROOM_NOT_FOUND)["code"])
        client.send({"type": FRAME_TYPE.DISCARD, "tile": "3m", "seq": 1})
        seen.append(client.expect_error(ERROR_CODE.ROOM_NOT_FOUND)["code"])
        for code in seen:
            if code not in frozen_codes:
                raise TestFailure(f"server sent a code outside PROTOCOL.md §7.1: {code}")

        # Inside a room with no open window, a well-formed discard/action is silently
        # dropped (PROTOCOL.md §6.3: a mismatching seq is never answered with an error).
        client.send({"type": FRAME_TYPE.CREATE_ROOM})
        client.expect_type(FRAME_TYPE.ROOM_JOINED)
        client.send({"type": FRAME_TYPE.DISCARD, "tile": "3m", "seq": 1})
        client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.PASS, "tiles": [], "seq": 1})
        client.expect_no_frame()
    finally:
        client.close()


def test_rate_limit_storm(port: int) -> None:
    client = Client(port, "rate-limit")
    try:
        login(client, "限速")
        burst = (b'{"type":%d}\n' % FRAME_TYPE.PING) * 1200
        client.send_raw(burst)
        deadline = time.monotonic() + 10
        saw_rate_limited = False
        closed = False
        buffered = b""
        while time.monotonic() < deadline:
            try:
                client.sock.settimeout(max(0.1, deadline - time.monotonic()))
                chunk = client.sock.recv(65536)
            except socket.timeout:
                break
            except ConnectionResetError:
                closed = True
                break
            if not chunk:
                closed = True
                break
            buffered += chunk
        for frame in frames_of(buffered):
            if frame.get("type") == FRAME_TYPE.ERROR and frame.get("code") == ERROR_CODE.RATE_LIMITED:
                saw_rate_limited = True
        if not saw_rate_limited:
            raise TestFailure("rate-limit storm: no RATE_LIMITED error was sent")
        if not closed:
            raise TestFailure("rate-limit storm: the connection was not closed")
    finally:
        client.close()


def _seat_disconnected(frame: dict, seat: int):
    """True/False for the seat's public `disconnected` flag in a state_sync, None otherwise."""
    if frame.get("type") != FRAME_TYPE.STATE_SYNC:
        return None
    entry = next((item for item in frame.get("seats", []) if item.get("seat") == seat), None)
    return None if entry is None else bool(entry.get("disconnected"))


def test_write_stall_is_bounded(port: int) -> None:
    """A peer that stops reading must be closed, not kept forever.

    asio operations have no timeout of their own, and a peer that stops reading does not produce an
    error: TCP advertises a zero window and the kernel blocks the write instead of failing it, so
    `async_write_some` simply never completes (measured: still pending after 10 s). Without the
    writer's own bound that connection would pin its write coroutine, its read coroutine (nothing
    cancels it short of the socket close in CloseClient) and its whole Client — an unbounded leak
    reachable by any client that stops reading while a match is being broadcast to it.

    The repro has to stay under the rate limit while producing outbound traffic: `sync` is cheap to
    send and each one is answered with a full state_sync, so 40 per second (well under the 100/s
    limit, and steady enough not to trip the storm rule) fills a 2 KB window in a second or two. The
    peer then never reads a byte of it.
    """
    slow = Client(port, "write-stall", recv_buffer_bytes=2048)
    try:
        login(slow, "不收包")
        slow.send({"type": FRAME_TYPE.CREATE_ROOM})
        slow.expect_type(FRAME_TYPE.ROOM_JOINED)
        slow.send({"type": FRAME_TYPE.READY})
        slow.expect_type(FRAME_TYPE.GAME_START)

        # Keep asking for the board and never read a reply. The receive window closes quickly, the
        # server's write stops making progress, and kWriteStallTimeoutMs (5 s) is what has to end it.
        #
        # Note what must NOT be the reason: a rate-limit storm. 40/s is under the 100/s threshold,
        # so a close here is the write path's bound, not the quota.
        deadline = time.monotonic() + 90
        closed = False
        while time.monotonic() < deadline:
            try:
                slow.send({"type": FRAME_TYPE.SYNC})
            except (BrokenPipeError, ConnectionResetError):
                closed = True
                break
            time.sleep(0.025)
            # MSG_PEEK probes for EOF without consuming: the data stays queued, so the window stays
            # shut and the pending write stays stalled.
            slow.sock.settimeout(0.005)
            try:
                chunk = slow.sock.recv(1, socket.MSG_PEEK)
            except socket.timeout:
                continue
            except ConnectionResetError:
                closed = True
                break
            if not chunk:
                closed = True
                break

        if not closed:
            raise TestFailure(
                "a peer that never reads was not closed: the write path has no timeout "
                "(the connection would leak its write coroutine, its read coroutine and its Client)")
    finally:
        slow.close()


def test_first_frame_timeout(port: int) -> None:
    """A connection that never completes a frame must be closed.

    The idle timeout measures silence between frames, so a peer that dribbles one byte every few
    seconds is never idle; and because it never sends a terminator, no line reaches the parser, so
    neither the bad-frame quota nor the 64 KB unframed cap ever trips either (64 KB at one byte per
    19 s is about 14 days). Each such connection holds a PlayerActor and two buffers, and there is no
    cap on how many a peer may open. kFirstFrameTimeoutMs is the bound that closes it.
    """
    slow = Client(port, "slowloris")
    try:
        # Half a frame, then a byte every few seconds: alive, never complete.
        slow.send_raw(b'{"type":')
        started = time.monotonic()
        deadline = started + 90
        closed = False
        last_dribble = started
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now - last_dribble >= 5:
                slow.send_raw(b" ")
                last_dribble = now
            slow.sock.settimeout(1.0)
            try:
                chunk = slow.sock.recv(65536)
            except socket.timeout:
                continue
            except (ConnectionResetError, BrokenPipeError):
                closed = True
                break
            if not chunk:
                closed = True
                break
        if not closed:
            raise TestFailure("a connection that never sent a complete frame was not closed "
                              "(kFirstFrameTimeoutMs is not enforced)")
    finally:
        slow.close()


def _drain_until(client: "Client", wanted: int, timeout: float, seen: list) -> dict:
    """Reads frames until |wanted| arrives, collecting the ones seen on the way.

    A game keeps producing frames while a client is silent (its window times out, the bots
    play on), so a reply is never guaranteed to be the *next* frame on the wire.
    """
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TestFailure(f"{client.name}: no {wanted!r} within {timeout:.0f}s (seen {len(seen)} frame(s))")
        frame = client.recv_frame(timeout=remaining)
        seen.append(frame)
        if frame.get("type") == wanted:
            return frame


def test_idle_timeout(port: int) -> None:
    """PROTOCOL.md §1.6/§8.5: 20s of silence drops a lobby connection but keeps a room seat.

    Two connections, because the two halves of §8.5 are only observable from different sides:

      * the silent *lobby* connection must be closed outright (§1.6: 大厅中 → 断开);
      * the silent *in-room* connection must stay open and keep answering pings (§1.6:
        对局中 → 进入托管，不立即断开), while a *second* seat in the same room sees that
        entrustment in `state_sync.seats[].disconnected` (§8.5: 告知其他座位).

    The observer used to be one connection checking its own `disconnected` flag, which the
    protocol never promises: once a seat is entrusted its endpoint is dropped (room_actor.cc
    OnPlayerDisconnected), so a `sync` from that seat can no longer be answered at all. What §8.5
    does promise is that the *other* seats are told, and that is what this checks now.
    """
    lobby = Client(port, "idle-lobby")
    quiet = Client(port, "idle-quiet")
    observer = Client(port, "idle-observer")
    try:
        login(lobby, "大厅发呆")
        login(quiet, "房内发呆")
        login(observer, "旁观托管")

        # One room, two humans: seat 0 goes silent, seat 1 keeps heartbeating and watches.
        quiet.send({"type": FRAME_TYPE.CREATE_ROOM})
        room_id = quiet.expect_type(FRAME_TYPE.ROOM_JOINED)["room_id"]
        observer.send({"type": FRAME_TYPE.JOIN_ROOM, "room_id": room_id})
        joined = observer.expect_type(FRAME_TYPE.ROOM_JOINED)
        observer_seat = joined["seat"]
        if observer_seat == 0:
            raise TestFailure(f"the observer must not inherit seat 0: {joined!r}")
        quiet.expect_type(FRAME_TYPE.ROOM_UPDATE)
        observer.expect_type(FRAME_TYPE.ROOM_UPDATE)
        quiet.send({"type": FRAME_TYPE.READY})
        quiet.expect_type(FRAME_TYPE.GAME_START)
        observer.expect_type(FRAME_TYPE.GAME_START)

        started = time.monotonic()
        # Two clocks run at once from here, and both matter:
        #
        #   * the lobby connection must be closed after ~20s of silence (§1.6: 大厅中 → 断开);
        #   * the observer must keep heartbeating from *now*, because §1.6 counts inbound frames:
        #     a connection that sends nothing for 20s is entrusted even if it is reading. Waiting
        #     for the lobby close first and only then starting the observer's heartbeat would
        #     leave the observer silent for the whole window and entrust it too, which is what an
        #     earlier version of this test did by accident.
        lobby.sock.settimeout(1.0)
        lobby_closed_at = None
        last_ping = started
        while True:
            now = time.monotonic()
            if lobby_closed_at is None:
                try:
                    chunk = lobby.sock.recv(65536)
                except socket.timeout:
                    chunk = None  # still open, just quiet
                except ConnectionResetError:
                    chunk = b""
                if chunk is not None and not chunk:
                    lobby_closed_at = now
            if now - last_ping >= 3.0:
                observer.send({"type": FRAME_TYPE.PING})
                last_ping = now
            try:
                observer.recv_frame(timeout=0.2)
            except TestFailure:
                pass  # broadcasts interleave with the pong; only the flag below matters
            if lobby_closed_at is not None and now - started >= 22:
                break

        elapsed = lobby_closed_at - started
        if elapsed < 15:
            raise TestFailure(f"lobby connection closed after only {elapsed:.1f}s (expected ~20s)")

        # The in-room connection stays open and still answers (its seat is entrusted, not dropped).
        quiet.send({"type": FRAME_TYPE.PING})
        seen: list = []
        _drain_until(quiet, FRAME_TYPE.PONG, 10.0, seen)

        # ... and the entrustment is visible where §8.5 says it is: to the *other* seats. A `sync`
        # asks for the authoritative board, so this does not depend on a broadcast happening to
        # fire during the silence.
        #
        # The observer's buffer can still hold a state_sync queued *before* the seat was entrusted,
        # so "the next state_sync" is not necessarily an answer to this request. Drop what is
        # already buffered and read the fresh one: `sync` always answers, and its reply is the
        # board as of now.
        observer.buffer = b""
        seen.clear()
        observer.send({"type": FRAME_TYPE.SYNC})
        snapshot = _drain_until(observer, FRAME_TYPE.STATE_SYNC, 10.0, seen)
        entrust = _seat_disconnected(snapshot, 0)
        if entrust is not True:
            raise TestFailure("§8.5: after 20s of silence the quiet seat must be entrusted and the "
                              f"other seats must see disconnected=true, got {entrust!r} in {snapshot!r}")
        if _seat_disconnected(snapshot, observer_seat) is not False:
            raise TestFailure("the heartbeating seat must NOT be entrusted: "
                              f"{snapshot.get('seats')!r}")
    finally:
        lobby.close()
        quiet.close()
        observer.close()


TESTS = (
    ("login/ping/framing", test_login_ping_and_frame_shape),
    ("frames split across segments", test_frames_split_across_segments),
    ("not-logged-in", test_not_logged_in),
    ("invalid-name quota", test_invalid_name_quota),
    ("invalid JSON", test_invalid_json_keeps_connection),
    ("unknown type", test_unknown_type),
    ("bad-frame quota", test_bad_frame_quota),
    ("invalid UTF-8", test_invalid_utf8),
    ("oversized frame", test_oversized_frame),
    ("concurrent connections", test_two_connections_are_isolated),
    ("room errors", test_room_not_found),
    ("frozen error codes", test_error_codes_are_from_the_frozen_table),
    ("rate limit storm", test_rate_limit_storm),
)

# Opt-in: asserts the 20s heartbeat policy end to end, so it is far too slow for ctest.
IDLE_THRESHOLD_S = 20.0  # protocol::kIdleTimeoutMs
HEARTBEAT_S = 3.0


def test_continuous_traffic_is_not_idle(port: int) -> None:
    """t29: a connection that keeps sending frames must never look idle.

    The idle window is measured from the last inbound frame (§1.6), so a lobby connection
    that heartbeats every second must survive far longer than the 20s threshold. Before the
    fix the gateway measured the *connection age* instead, so this connection was dropped at
    ~20s even though it was chatting the whole time.
    """
    client = Client(port, "busy-lobby")
    try:
        login(client, "一直心跳")
        started = time.monotonic()
        pings = 0
        while time.monotonic() - started < 2 * IDLE_THRESHOLD_S + 5:
            client.send({"type": FRAME_TYPE.PING})
            client.expect_type(FRAME_TYPE.PONG)
            pings += 1
            time.sleep(1.0)
        client.send({"type": FRAME_TYPE.PING})
        client.expect_type(FRAME_TYPE.PONG)
        print(f"[gateway-slow] continuous traffic survived {time.monotonic() - started:.1f}s "
              f"({pings} heartbeats) without being treated as idle")
    finally:
        client.close()


def _pick_tile(hand: list, drawn):
    """Any tile in hand is a legal discard; the drawn tile is what a real client plays."""
    if drawn is not None and drawn in hand:
        return drawn
    return hand[-1]


def _play_one_slow_game(port: int, attempt: int) -> dict:
    """Plays one game, pacing the human's windows so the game outlives the idle threshold.

    Returns {"entrusted": [...], "errors": [...], "answered": n, "elapsed": s, "reason": r}.
    """
    client = Client(port, f"slow-player-{attempt}", timeout=5.0)
    result = {"entrusted": [], "errors": [], "answered": 0, "elapsed": 0.0, "reason": None}
    try:
        login(client, f"慢速玩家{attempt}")
        client.send({"type": FRAME_TYPE.CREATE_ROOM})
        client.expect_type(FRAME_TYPE.ROOM_JOINED)
        client.send({"type": FRAME_TYPE.READY})
        start = client.expect_type(FRAME_TYPE.GAME_START)
        hand = list(start["hand"])
        drawn = None
        started = time.monotonic()
        next_heartbeat = started + HEARTBEAT_S
        while True:
            now = time.monotonic()
            if now >= next_heartbeat:
                # No pong expectation: the room may have other frames in flight, they are
                # handled by the same loop. A dead connection raises from recv_frame instead.
                client.send({"type": FRAME_TYPE.PING})
                next_heartbeat = time.monotonic() + HEARTBEAT_S
                continue
            try:
                frame = client.recv_frame(timeout=max(0.05, next_heartbeat - now))
            except TestFailure as exc:
                if "timed out waiting" in str(exc):
                    continue  # heartbeat is due; the next round sends it
                raise
            kind = frame["type"]
            if kind == FRAME_TYPE.ERROR:
                result["errors"].append(frame)
                continue
            if kind == FRAME_TYPE.GAME_END:
                result["reason"] = frame.get("reason")
                break
            if kind == FRAME_TYPE.DRAW:
                hand.append(frame["tile"])
                drawn = frame["tile"]
                continue
            if kind == FRAME_TYPE.PONG:
                continue  # heartbeat answer
            if kind == FRAME_TYPE.DISCARD_EVENT:
                if frame["seat"] == 0:
                    if frame["tile"] in hand:
                        hand.remove(frame["tile"])
                    drawn = None
                continue
            if kind == FRAME_TYPE.STATE_SYNC:
                seat0 = next((entry for entry in frame["seats"] if entry["seat"] == 0), None)
                if seat0 is not None and seat0["disconnected"]:
                    result["entrusted"].append(frame["seq"])
                continue
            if kind == FRAME_TYPE.ACTION_REQUEST:
                # Slow down until the game has outlived the idle threshold, then finish fast.
                elapsed = time.monotonic() - started
                time.sleep(9.0 if elapsed < IDLE_THRESHOLD_S + 2 else 0.2)
                seq = frame["seq"]
                actions = {option["action"] for option in frame.get("options", [])}
                if ACTION.WIN in actions:
                    client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.WIN, "tiles": [], "seq": seq})
                elif frame["window"] == WINDOW.DISCARD:
                    client.send({"type": FRAME_TYPE.DISCARD, "tile": _pick_tile(hand, drawn), "seq": seq})
                else:
                    client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.PASS, "tiles": [], "seq": seq})
                result["answered"] += 1
                continue
            # discard/draw for others, action_event, ... : nothing to do
        result["elapsed"] = time.monotonic() - started
        return result
    finally:
        client.close()


def test_slow_play_is_never_entrusted(port: int) -> None:
    """t29 core repro: playing a game past the idle threshold must never entrust the seat.

    The gateway used to compare the threshold against the connection's age, so at ~20s the
    seat was marked entrusted even while the player kept discarding; an entrusted seat then
    plays its default action without waiting (§5.6.2), which fast-forwards the game and makes
    every state_sync advertise disconnected=true (the client's "断线托管" badge).

    A game can legitimately end before the threshold (a bot wins early), so up to three games
    are played; *every* game must show the seat never entrusted, and at least one of them must
    outlive the threshold for the check to be meaningful.
    """
    attempts = []
    for attempt in range(3):
        result = _play_one_slow_game(port, attempt)
        attempts.append(result)
        if result["elapsed"] > IDLE_THRESHOLD_S:
            break
    for index, result in enumerate(attempts):
        if result["entrusted"]:
            raise TestFailure(f"game {index + 1}: the seat was marked entrusted (disconnected=true) at seq "
                              f"{result['entrusted']} although the client kept playing and heartbeating")
        if result["errors"]:
            raise TestFailure(f"game {index + 1}: unexpected error frames: {result['errors']}")
        if result["reason"] is None:
            raise TestFailure(f"game {index + 1}: the game never ended")
        if result["answered"] < 1:
            raise TestFailure(f"game {index + 1}: the player never answered a window, so the game did not "
                              f"advance by its own actions")
    longest = max(result["elapsed"] for result in attempts)
    if longest <= IDLE_THRESHOLD_S:
        raise TestFailure(f"no game outlived the {IDLE_THRESHOLD_S:.0f}s idle threshold "
                          f"(longest {longest:.1f}s); the check would be vacuous")
    total_answers = sum(result["answered"] for result in attempts)
    print(f"[gateway-slow] played {len(attempts)} game(s), longest {longest:.1f}s "
          f"(> {IDLE_THRESHOLD_S:.0f}s idle threshold), {total_answers} window(s) answered by the player, "
          f"seat never entrusted, last reason={attempts[-1]['reason']}")


SLOW_TESTS = (
    ("idle timeout", test_idle_timeout),
    ("continuous traffic is not idle", test_continuous_traffic_is_not_idle),
    ("slow play is never entrusted", test_slow_play_is_never_entrusted),
    ("write stall is bounded", test_write_stall_is_bounded),
    ("first frame timeout", test_first_frame_timeout),
)


# ---------------------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------------------


def find_binary(explicit: str | None) -> str:
    if explicit:
        if not os.path.isfile(explicit):
            raise SystemExit(f"--server-bin does not exist: {explicit}")
        return explicit
    env = os.environ.get("MAHJONG_SERVER_BIN")
    if env and os.path.isfile(env):
        return env
    for candidate in DEFAULT_BIN_CANDIDATES:
        path = os.path.join(REPO_ROOT, candidate)
        if os.path.isfile(path):
            return path
    raise SystemExit(
        "cannot find the mahjong_server binary; build it with "
        "`cmake --build build --target mahjong_server` or pass --server-bin"
    )


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-bin", default=None, help="path to the mahjong_server binary")
    parser.add_argument("--keep-server", action="store_true", help="leave the server running without SIGINT")
    parser.add_argument("--slow-tests", action="store_true", help="also run the ~25s heartbeat/idle-timeout case")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    selected = list(TESTS)
    if args.slow_tests:
        selected += list(SLOW_TESTS)

    binary = find_binary(args.server_bin)
    port = free_port()
    seed = random.randint(1, 2**31 - 1)
    log_file = tempfile.NamedTemporaryFile(prefix="mahjong_server_", suffix=".log", delete=False)
    print(f"[smoke] binary={binary}")
    print(f"[smoke] port={port} seed={seed}")

    process = subprocess.Popen(
        [binary, "--port", str(port), "--seed", str(seed)],
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    failures = []
    try:
        wait_for_server(port, process)
        for name, test in selected:
            started = time.monotonic()
            try:
                test(port)
                print(f"[smoke] PASS  {name} ({time.monotonic() - started:.2f}s)")
            except Exception as exc:  # noqa: BLE001 - report every failure kind
                failures.append((name, exc))
                print(f"[smoke] FAIL  {name}: {exc}")
        if process.poll() is not None:
            failures.append(("server alive", TestFailure(f"server exited with code {process.returncode}")))
    finally:
        if args.keep_server:
            print("[smoke] --keep-server: leaving the server running (pid=%d)" % process.pid)
        else:
            process.send_signal(2)  # SIGINT: graceful shutdown
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                failures.append(("graceful shutdown", TestFailure("server ignored SIGINT")))
                process.kill()
                process.wait(timeout=5)
        log_file.close()

    if args.verbose or failures:
        with open(log_file.name, "r", encoding="utf-8", errors="replace") as handle:
            log_text = handle.read().strip()
        if log_text:
            print("[smoke] ---- server log ----")
            print(log_text)
            print("[smoke] --------------------")
    else:
        os.unlink(log_file.name)

    if failures:
        print(f"[smoke] FAILED: {len(failures)} test(s)")
        for name, exc in failures:
            print(f"[smoke]   - {name}: {exc}")
        return 1
    print(f"[smoke] OK: {len(selected)} test(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
