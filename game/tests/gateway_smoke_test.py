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

    def __init__(self, port: int, name: str = "client", timeout: float = 5.0):
        self.name = name
        self.buffer = b""
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
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

    def expect_type(self, expected: str, timeout: float = 5.0) -> dict:
        frame = self.recv_frame(timeout)
        if frame.get("type") != expected:
            raise TestFailure(f"{self.name}: expected type={expected!r}, got {frame!r}")
        return frame

    def expect_error(self, code: str, timeout: float = 5.0) -> dict:
        frame = self.expect_type("error", timeout)
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
    client.send_frame({"type": "login", "name": name})
    frame = client.expect_type("login_ok")
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
        client.send({"type": "ping"})
        client.expect_type("pong")
        # Two frames in one TCP segment must both be processed, in order.
        client.send(b'{"type":"ping"}\n{"type":"ping"}\n')
        client.expect_type("pong")
        client.expect_type("pong")
        # A second login on the same connection is a BAD_REQUEST (§4.1) and keeps it open.
        client.send({"type": "login", "name": "重复登录"})
        client.expect_error("BAD_REQUEST")
        client.send({"type": "ping"})
        client.expect_type("pong")
    finally:
        client.close()


def test_frames_split_across_segments(port: int) -> None:
    """A frame may arrive in arbitrary TCP chunks; NDJSON framing must reassemble it."""
    client = Client(port, "split-frame")
    try:
        login(client, "分片帧")
        # 1. Byte-by-byte delivery of a short frame.
        for byte in b'{"type":"ping"}\n':
            client.send_raw(bytes([byte]))
        client.expect_type("pong")
        # 2. A >16 KB frame split into three writes (forces several read() calls).
        big = json.dumps({"type": "ping", "pad": "b" * 40000}).encode("utf-8") + b"\n"
        third = len(big) // 3
        for chunk in (big[:third], big[third:2 * third], big[2 * third:]):
            client.send_raw(chunk)
            time.sleep(0.05)
        client.expect_type("pong")
    finally:
        client.close()


def test_not_logged_in(port: int) -> None:
    client = Client(port, "not-logged-in")
    try:
        client.send({"type": "ping"})
        client.expect_error("NOT_LOGGED_IN")
        client.send({"type": "create_room"})
        client.expect_error("NOT_LOGGED_IN")
        # The connection survives and can still log in.
        login(client, "迟到玩家")
    finally:
        client.close()


def test_invalid_name_quota(port: int) -> None:
    client = Client(port, "invalid-name")
    try:
        client.send({"type": "login", "name": ""})
        client.expect_error("INVALID_NAME")
        client.send({"type": "login", "name": "x" * 17})
        client.expect_error("INVALID_NAME")
        # The 4th invalid name drops the connection (§1.7).
        client.send({"type": "login", "name": "\u0007bad"})
        client.expect_error("INVALID_NAME")
        client.send({"type": "login", "name": ""})
        client.expect_error("INVALID_NAME")
        payload = client.expect_closed()
        if payload:
            raise TestFailure(f"invalid-name quota: unexpected trailing payload {payload!r}")
    finally:
        client.close()

    client = Client(port, "invalid-name-then-ok")
    try:
        # Three invalid names keep the connection usable.
        for _ in range(3):
            client.send({"type": "login", "name": "x" * 17})
            client.expect_error("INVALID_NAME")
        login(client, "正常玩家")
    finally:
        client.close()


def test_invalid_json_keeps_connection(port: int) -> None:
    client = Client(port, "bad-json")
    try:
        client.send(b'{"type":"login",\n')
        client.expect_error("BAD_REQUEST")
        client.send("{not json at all}\n")
        client.expect_error("BAD_REQUEST")
        client.send("[]\n")  # valid JSON but not an object
        client.expect_error("BAD_REQUEST")
        client.send('{"name":"no-type"}\n')  # missing "type"
        client.expect_error("BAD_REQUEST")
        client.send('{"type":42}\n')  # non-string "type"
        client.expect_error("BAD_REQUEST")
        login(client, "坏帧之后")
    finally:
        client.close()


def test_unknown_type(port: int) -> None:
    client = Client(port, "unknown-type")
    try:
        login(client, "未知类型")
        client.send({"type": "definitely_not_a_frame"})
        client.expect_error("BAD_REQUEST")
        client.send({"type": "ping"})
        client.expect_type("pong")
    finally:
        client.close()


def test_bad_frame_quota(port: int) -> None:
    client = Client(port, "bad-frame-quota")
    try:
        login(client, "坏帧预算")
        for _ in range(9):
            client.send({"type": "nope"})
            client.expect_error("BAD_REQUEST")
        client.send({"type": "nope"})
        client.expect_error("BAD_REQUEST")  # 10th bad frame: error, then close
        payload = client.expect_closed()
        frames = frames_of(payload)
        for frame in frames:
            if frame.get("type") != "error" or frame.get("code") != "BAD_REQUEST":
                raise TestFailure(f"bad-frame quota: unexpected trailing frame {frame!r}")
    finally:
        client.close()


def test_invalid_utf8(port: int) -> None:
    client = Client(port, "bad-utf8")
    try:
        client.send_raw(b'{"type":"login","name":"\xff\xfe"}\n')
        client.expect_error("BAD_REQUEST")
        client.send_raw(b'{"type":"login","name":"\xc0\xaf"}\n')  # overlong encoding
        client.expect_error("BAD_REQUEST")
        login(client, "UTF8之后")
    finally:
        client.close()


def test_oversized_frame(port: int) -> None:
    # 1. Oversized but terminated: FRAME_TOO_LARGE (best effort) then close.
    client = Client(port, "oversized-terminated")
    try:
        login(client, "超长帧")
        padding = "a" * (MAX_FRAME_BYTES + 1024)
        client.send({"type": "ping", "pad": padding})
        payload = client.expect_closed()
        frames = frames_of(payload)
        if frames and frames[0].get("code") != "FRAME_TOO_LARGE":
            raise TestFailure(f"oversized frame: expected FRAME_TOO_LARGE, got {frames!r}")
    finally:
        client.close()

    # 2. Oversized without any terminator: drop the connection, never buffer without bound.
    client = Client(port, "oversized-unterminated")
    try:
        client.send_raw(b"a" * (MAX_FRAME_BYTES + 4096))
        payload = client.expect_closed()
        frames = frames_of(payload)
        if frames and frames[0].get("code") != "FRAME_TOO_LARGE":
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

        alpha.send({"type": "create_room"})
        joined = alpha.expect_type("room_joined")
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
        alpha.send({"type": "create_room"})
        alpha.expect_error("BAD_REQUEST")  # already in a room
        beta.send({"type": "ping"})
        beta.expect_type("pong")

        # Closing beta must not disturb alpha.
        beta.close()
        time.sleep(0.2)
        alpha.send({"type": "ping"})
        alpha.expect_type("pong")
    finally:
        alpha.close()
        beta.close()


def test_room_not_found(port: int) -> None:
    owner = Client(port, "room-owner")
    client = Client(port, "room-lookup")
    late = Client(port, "room-late")
    try:
        login(client, "找房间")
        client.send({"type": "join_room", "room_id": "r-9999"})
        client.expect_error("ROOM_NOT_FOUND")
        client.send({"type": "ready"})  # not in a room yet
        client.expect_error("ROOM_NOT_FOUND")
        client.send({"type": "join_room", "room_id": 7})
        client.expect_error("BAD_REQUEST")

        # Before ready, joining a live room takes an AI seat (PROTOCOL §4.3).
        login(owner, "房主")
        owner.send({"type": "create_room"})
        room_id = owner.expect_type("room_joined")["room_id"]
        client.send({"type": "join_room", "room_id": room_id})
        joined = client.expect_type("room_joined")
        seat = joined.get("seat")
        if joined.get("room_id") != room_id or not isinstance(seat, int) or seat < 1:
            raise TestFailure(f"join before ready must return room_joined seat>=1: {joined!r}")

        # PROTOCOL §5.17 (v1.23): a pre-game join broadcasts one room_update to every
        # human in the room. Both connections must consume it before the game_start frames.
        owner_update = owner.expect_type("room_update")
        client_update = client.expect_type("room_update")
        if owner_update.get("room_id") != room_id or client_update.get("room_id") != room_id:
            raise TestFailure(f"room_update.room_id mismatch: {owner_update!r} / {client_update!r}")
        if owner_update.get("seats") != client_update.get("seats"):
            raise TestFailure(f"room_update rosters must match: {owner_update!r} / {client_update!r}")
        if not any(s.get("name") == "找房间" and s.get("is_bot") is False
                   for s in owner_update.get("seats", [])):
            raise TestFailure(f"owner must see the newcomer in room_update: {owner_update!r}")

        owner.send({"type": "ready"})
        owner.expect_type("game_start")
        client.expect_type("game_start")

        login(late, "后来者")
        late.send({"type": "join_room", "room_id": room_id})
        late.expect_error("ROOM_NOT_FOUND")
        late.send({"type": "ping"})
        late.expect_type("pong")
    finally:
        client.close()
        owner.close()
        late.close()


def test_error_codes_are_from_the_frozen_table(port: int) -> None:
    frozen_codes = {
        "INVALID_NAME",
        "NOT_LOGGED_IN",
        "ROOM_NOT_FOUND",
        "NOT_YOUR_TURN",
        "NO_PENDING_ACTION",
        "ILLEGAL_ACTION",
        "UNKNOWN_TILE",
        "BAD_REQUEST",
        "FRAME_TOO_LARGE",
        "RATE_LIMITED",
        "INTERNAL",
    }
    client = Client(port, "error-codes")
    seen = []
    try:
        login(client, "错误码")
        # Connection-level validation happens before any room lookup.
        client.send({"type": "discard", "tile": "10z", "seq": 1})
        seen.append(client.expect_error("UNKNOWN_TILE")["code"])
        client.send({"type": "discard", "tile": "3m"})  # missing seq
        seen.append(client.expect_error("BAD_REQUEST")["code"])
        client.send({"type": "action", "action": "dance", "tiles": [], "seq": 1})
        seen.append(client.expect_error("BAD_REQUEST")["code"])
        # "5x" is not a tile in either the 27-face or the 34-face encoding (a bare "1z"
        # would become legal once the honour tiles land).
        client.send({"type": "action", "action": "pong", "tiles": ["5x"], "seq": 1})
        seen.append(client.expect_error("UNKNOWN_TILE")["code"])
        client.send({"type": "discard", "tile": "5x", "seq": 1})
        seen.append(client.expect_error("UNKNOWN_TILE")["code"])
        client.send({"type": "action", "action": "pong", "tiles": "9s", "seq": 1})  # tiles not an array
        seen.append(client.expect_error("BAD_REQUEST")["code"])
        # Room-scoped intents without a room are rejected with ROOM_NOT_FOUND.
        client.send({"type": "ready"})
        seen.append(client.expect_error("ROOM_NOT_FOUND")["code"])
        client.send({"type": "sync"})
        seen.append(client.expect_error("ROOM_NOT_FOUND")["code"])
        client.send({"type": "discard", "tile": "3m", "seq": 1})
        seen.append(client.expect_error("ROOM_NOT_FOUND")["code"])
        for code in seen:
            if code not in frozen_codes:
                raise TestFailure(f"server sent a code outside PROTOCOL.md §7.1: {code}")

        # Inside a room with no open window, a well-formed discard/action is silently
        # dropped (PROTOCOL.md §6.3: a mismatching seq is never answered with an error).
        client.send({"type": "create_room"})
        client.expect_type("room_joined")
        client.send({"type": "discard", "tile": "3m", "seq": 1})
        client.send({"type": "action", "action": "pass", "tiles": [], "seq": 1})
        client.expect_no_frame()
    finally:
        client.close()


def test_rate_limit_storm(port: int) -> None:
    client = Client(port, "rate-limit")
    try:
        login(client, "限速")
        burst = b'{"type":"ping"}\n' * 1200
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
            if frame.get("type") == "error" and frame.get("code") == "RATE_LIMITED":
                saw_rate_limited = True
        if not saw_rate_limited:
            raise TestFailure("rate-limit storm: no RATE_LIMITED error was sent")
        if not closed:
            raise TestFailure("rate-limit storm: the connection was not closed")
    finally:
        client.close()


def _seat_disconnected(frame: dict, seat: int):
    """True/False for the seat's public `disconnected` flag in a state_sync, None otherwise."""
    if frame.get("type") != "state_sync":
        return None
    entry = next((item for item in frame.get("seats", []) if item.get("seat") == seat), None)
    return None if entry is None else bool(entry.get("disconnected"))


def _drain_until(client: "Client", wanted: str, timeout: float, seen: list) -> dict:
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
    """PROTOCOL.md §1.6: 20s of silence drops a lobby connection but keeps a room seat."""
    lobby = Client(port, "idle-lobby")
    in_room = Client(port, "idle-room")
    try:
        login(lobby, "大厅发呆")
        login(in_room, "房内发呆")
        in_room.send({"type": "create_room"})
        in_room.expect_type("room_joined")
        in_room.send({"type": "ready"})
        in_room.expect_type("game_start")
        started = time.monotonic()
        # The lobby connection must be closed after ~20s of silence.
        deadline = started + 30
        lobby.sock.settimeout(deadline - time.monotonic())
        while True:
            try:
                chunk = lobby.sock.recv(65536)
            except (socket.timeout, ConnectionResetError):
                chunk = b""
            if not chunk:
                break
        elapsed = time.monotonic() - started
        if elapsed < 15:
            raise TestFailure(f"lobby connection closed after only {elapsed:.1f}s (expected ~20s)")
        # The in-room connection stays open and still answers (its seat becomes entrusted).
        time.sleep(max(0.0, 21 - elapsed))
        in_room.send({"type": "ping"})
        seen: list = []
        _drain_until(in_room, "pong", 10.0, seen)
        # ... and the silence really did entrust the seat: the game either kept broadcasting
        # snapshots while we were quiet, or answers a `sync` with the authoritative one.
        # This `disconnected` flag is exactly what the client turns into its 托管 badge.
        entrust_flags = [flag for flag in (_seat_disconnected(frame, 0) for frame in seen) if flag is not None]
        if not any(entrust_flags):
            in_room.send({"type": "sync"})
            _drain_until(in_room, "state_sync", 10.0, seen)
            entrust_flags = [flag for flag in (_seat_disconnected(frame, 0) for frame in seen) if flag is not None]
        if not any(entrust_flags):
            raise TestFailure("20s of real silence must entrust the in-room seat "
                              f"(no state_sync showed disconnected=true; {len(seen)} frame(s) seen)")
    finally:
        lobby.close()
        in_room.close()


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
            client.send({"type": "ping"})
            client.expect_type("pong")
            pings += 1
            time.sleep(1.0)
        client.send({"type": "ping"})
        client.expect_type("pong")
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
        client.send({"type": "create_room"})
        client.expect_type("room_joined")
        client.send({"type": "ready"})
        start = client.expect_type("game_start")
        hand = list(start["hand"])
        drawn = None
        started = time.monotonic()
        next_heartbeat = started + HEARTBEAT_S
        while True:
            now = time.monotonic()
            if now >= next_heartbeat:
                # No pong expectation: the room may have other frames in flight, they are
                # handled by the same loop. A dead connection raises from recv_frame instead.
                client.send({"type": "ping"})
                next_heartbeat = time.monotonic() + HEARTBEAT_S
                continue
            try:
                frame = client.recv_frame(timeout=max(0.05, next_heartbeat - now))
            except TestFailure as exc:
                if "timed out waiting" in str(exc):
                    continue  # heartbeat is due; the next round sends it
                raise
            kind = frame["type"]
            if kind == "error":
                result["errors"].append(frame)
                continue
            if kind == "game_end":
                result["reason"] = frame.get("reason")
                break
            if kind == "draw":
                hand.append(frame["tile"])
                drawn = frame["tile"]
                continue
            if kind == "pong":
                continue  # heartbeat answer
            if kind == "discard_event":
                if frame["seat"] == 0:
                    if frame["tile"] in hand:
                        hand.remove(frame["tile"])
                    drawn = None
                continue
            if kind == "state_sync":
                seat0 = next((entry for entry in frame["seats"] if entry["seat"] == 0), None)
                if seat0 is not None and seat0["disconnected"]:
                    result["entrusted"].append(frame["seq"])
                continue
            if kind == "action_request":
                # Slow down until the game has outlived the idle threshold, then finish fast.
                elapsed = time.monotonic() - started
                time.sleep(9.0 if elapsed < IDLE_THRESHOLD_S + 2 else 0.2)
                seq = frame["seq"]
                actions = {option["action"] for option in frame.get("options", [])}
                if "win" in actions:
                    client.send({"type": "action", "action": "win", "tiles": [], "seq": seq})
                elif frame["window"] == "discard":
                    client.send({"type": "discard", "tile": _pick_tile(hand, drawn), "seq": seq})
                else:
                    client.send({"type": "action", "action": "pass", "tiles": [], "seq": seq})
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
