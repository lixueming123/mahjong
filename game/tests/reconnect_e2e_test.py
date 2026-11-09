#!/usr/bin/env python3
"""Account reconnect end-to-end test (PROTOCOL v2.2 §8.5).

Starts the real server against MySQL and drives the account-only reconnect path:

  * after `ready`, a dropped connection logs back in with the same account and receives
    `login_ok{in_room:true}` + `room_joined` + `game_start` + `state_sync`;
  * `game_start` now carries `match_total[4]` / `renchan_count`;
  * `sync` on the new connection answers a `state_sync` for the same seat, proving the seat
    was rebound rather than a new room created;
  * `login{resume:false}` leaves the old room and answers `in_room:false`, after which a new
    room can be created;
  * refreshing a waiting room (before `ready`) is not a resumable game: it answers
    `in_room:false`, matching the documented pre-deal behaviour.

Exit code 77 (ctest: SKIP) when MySQL is unreachable, the same convention as
account_e2e_test.py.

Usage:
  python3 game/tests/reconnect_e2e_test.py --server-bin build/game/mahjong_server
"""

from __future__ import annotations

import argparse
import os
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import ERROR_CODE, FRAME_TYPE, WINDOW

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from account_e2e_test import (  # noqa: E402
    DEFAULT_MYSQL_PASSWORD,
    TEST_DB,
    Client,
    Server,
    TestFailure,
    check,
    cleanup_rows,
    mysql_reachable,
    resolve_binary,
    skip,
)

LOGIN_OK = FRAME_TYPE.LOGIN_OK
ROOM_JOINED = FRAME_TYPE.ROOM_JOINED
GAME_START = FRAME_TYPE.GAME_START
STATE_SYNC = FRAME_TYPE.STATE_SYNC
GAME_END = FRAME_TYPE.GAME_END
MATCH_END = FRAME_TYPE.MATCH_END
ERROR = FRAME_TYPE.ERROR
READY = FRAME_TYPE.READY
CREATE_ROOM = FRAME_TYPE.CREATE_ROOM
SYNC = FRAME_TYPE.SYNC
ACTION_REQUEST = FRAME_TYPE.ACTION_REQUEST
DISCARD = FRAME_TYPE.DISCARD
DISCARD_EVENT = FRAME_TYPE.DISCARD_EVENT


def fresh_account(prefix: str) -> tuple[str, str]:
    username = "{}_{}_{}".format(prefix[:4], os.getpid() % 10000, random.randint(1000, 9999))
    return username, "pw_" + username


def register_and_login(client: Client, username: str, password: str) -> dict:
    client.send({"type": FRAME_TYPE.REGISTER, "username": username, "password": password})
    return client.recv_type(LOGIN_OK)


def login_with_release_retry(client: Client, username: str, password: str, *, resume: bool | None = None) -> dict:
    """Log in, tolerating the old connection's not-yet-processed close.

    One account has one live connection (§4.2), so a refresh can hit ALREADY_ONLINE for a few
    hundred milliseconds. Retrying on the same connection is exactly what the Web client does.
    """
    frame: dict = {"type": FRAME_TYPE.LOGIN, "username": username, "password": password}
    if resume is False:
        frame["resume"] = False
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        client.send(frame)
        reply = client.recv_frame()
        if reply.get("type") == LOGIN_OK:
            return reply
        if reply.get("type") == ERROR and reply.get("code") == ERROR_CODE.ALREADY_ONLINE:
            time.sleep(0.1)
            continue
        raise TestFailure(f"unexpected login reply: {reply}")
    raise TestFailure("the account was still online after 15s")


def read_until(client: Client, wanted: set[int], timeout: float = 20.0) -> dict[int, dict]:
    """Reads frames until every type in |wanted| has been seen at least once."""
    seen: dict[int, dict] = {}
    deadline = time.monotonic() + timeout
    while set(seen) != wanted:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TestFailure(f"timed out waiting for {sorted(wanted - set(seen))}; got {sorted(seen)}")
        frame = client.recv_frame(timeout=remaining)
        kind = frame.get("type")
        if kind == ERROR:
            raise TestFailure(f"unexpected error frame: {frame}")
        if kind in wanted and kind not in seen:
            seen[kind] = frame
    return seen


def recv_type_skipping(client: Client, expected: int, timeout: float = 10.0) -> dict:
    """Reads until |expected|, skipping unrelated frames (e.g. a replayed game_end)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = client.recv_frame(timeout=max(0.1, deadline - time.monotonic()))
        if frame.get("type") == expected:
            return frame
    raise TestFailure(f"timed out waiting for frame {expected}")


def start_game(client: Client, room_id: str) -> tuple[dict, dict]:
    # The first round opens with game_start + action_request (no state_sync: the hand rides in
    # game_start). A reconnect later replays all three, which is what read_until checks below.
    client.send({"type": READY})
    seen = read_until(client, {GAME_START, ACTION_REQUEST})
    start = seen[GAME_START]
    check(start.get("match_total") is not None, "game_start must carry match_total")
    check(isinstance(start.get("match_total"), list), "match_total must be an array")
    check(len(start["match_total"]) == 4, "match_total must have four entries")
    check(isinstance(start.get("renchan_count"), int), "game_start must carry renchan_count")
    request = seen[ACTION_REQUEST]
    check(request.get("window") == WINDOW.DISCARD,
          f"the dealer must own the first discard window: {request}")
    return start, request


def wait_room_dropped(client: Client, username: str, password: str) -> dict:
    """After the old socket is closed, log in once and expect an in-room snapshot.

    A successful login cannot be retried on the same connection (CanStartAuth is false), so
    the ALREADY_ONLINE wait lives inside login_with_release_retry and this is a single attempt.
    """
    reply = login_with_release_retry(client, username, password)
    check(reply.get("in_room") is True, f"a mid-game account must report in_room:true: {reply}")
    return reply


def run(args: argparse.Namespace) -> None:
    binary = resolve_binary(args.server_bin)
    if not mysql_reachable(args.mysql_host, args.mysql_port):
        skip(f"no MySQL at {args.mysql_host}:{args.mysql_port}")

    server = Server(binary, args, action_timeout_ms=60000)
    server.start()
    usernames: list[str] = []
    try:
        # ---- 1. mid-game reconnect -------------------------------------------------------
        username, password = fresh_account("recon")
        usernames.append(username)
        first = Client(server.port, "first")
        first_ok = register_and_login(first, username, password)
        player_id = first_ok["player_id"]
        first.send({"type": CREATE_ROOM})
        joined = first.recv_type(ROOM_JOINED)
        room_id = joined["room_id"]
        seat = joined["seat"]
        start_before, request_before = start_game(first, room_id)
        hand_before = list(start_before.get("hand", []))
        check(len(hand_before) in (13, 14), f"bad hand snapshot: {hand_before}")
        first_seq = request_before["seq"]
        first_deadline = request_before["deadline_ms"]
        first.close()

        second = Client(server.port, "second")
        resumed = wait_room_dropped(second, username, password)
        check(resumed.get("player_id") == player_id, "reconnect must keep the same player_id")
        check(resumed.get("in_room") is True, "reconnect must report in_room:true")
        seen = read_until(second, {ROOM_JOINED, GAME_START, STATE_SYNC, ACTION_REQUEST})
        check(seen[ROOM_JOINED]["room_id"] == room_id, "reconnect must return to the same room")
        check(seen[ROOM_JOINED]["seat"] == seat, "reconnect must keep the same seat")
        check(seen[STATE_SYNC].get("hand") == hand_before,
              "a disconnect must not auto-discard before the countdown: "
              f"{hand_before} -> {seen[STATE_SYNC].get('hand')}")
        check(isinstance(seen[GAME_START].get("match_total"), list), "the resumed game_start needs match_total")
        replayed = seen[ACTION_REQUEST]
        check(replayed["seq"] == first_seq, "the reconnect must replay the same window, not open a new one")
        check(replayed["deadline_ms"] == first_deadline,
              "the replayed window must keep the original deadline, not grant a fresh one")

        # The tile is still in hand: the player can answer the replayed window themselves.
        hand_after = list(seen[STATE_SYNC]["hand"])
        second.send({"type": DISCARD, "tile": hand_after[-1], "seq": first_seq})
        event = recv_type_skipping(second, DISCARD_EVENT)
        check(event.get("seat") == seat,
              f"the replayed discard window must accept the player's tile: {event}")
        # A `sync` on the new socket must answer for the rebound seat, not a new room.
        second.send({"type": SYNC})
        sync_after = recv_type_skipping(second, STATE_SYNC)
        check("hand" in sync_after, "sync after reconnect must answer the authoritative state")
        second.close()
        print(f"[reconnect-e2e] mid-game resume OK: room={room_id} seat={seat} player={player_id}")

        # ---- 2. resume:false leaves the old room -----------------------------------------
        third = Client(server.port, "third")
        left = login_with_release_retry(third, username, password, resume=False)
        check(left.get("in_room") is False, f"resume:false must not reattach: {left}")
        check(left.get("player_id") == player_id, "resume:false must keep the account identity")
        third.send({"type": CREATE_ROOM})
        new_joined = third.recv_type(ROOM_JOINED)
        check(new_joined["room_id"] != room_id, "resume:false must allow a brand-new room")
        third.close()
        print("[reconnect-e2e] resume:false detached and opened a new room")

        # ---- 3. waiting-room refresh is not resumable ------------------------------------
        wait_user, wait_pass = fresh_account("wait")
        usernames.append(wait_user)
        waiting = Client(server.port, "waiting")
        register_and_login(waiting, wait_user, wait_pass)
        waiting.send({"type": CREATE_ROOM})
        waiting_joined = waiting.recv_type(ROOM_JOINED)
        waiting_room = waiting_joined["room_id"]
        waiting.close()

        again = Client(server.port, "waiting-again")
        no_room = login_with_release_retry(again, wait_user, wait_pass)
        check(no_room.get("in_room") is False, f"a pre-deal room must not be resumed: {no_room}")
        again.send({"type": CREATE_ROOM})
        replacement = again.recv_type(ROOM_JOINED)
        check(replacement["room_id"] != waiting_room, "a waiting-room owner must get a fresh room")
        again.close()
        print("[reconnect-e2e] pre-deal refresh answered in_room:false")
    except TestFailure:
        print("[reconnect-e2e] server log:\n" + server.log_tail(), file=sys.stderr)
        raise
    finally:
        server.stop()
        cleanup_rows(args, usernames)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server-bin", default=None, help="path to mahjong_server")
    parser.add_argument("--port", type=int, default=0, help="listen port (0 = pick a free one)")
    parser.add_argument("--mysql-host", default="127.0.0.1")
    parser.add_argument("--mysql-port", type=int, default=3306)
    parser.add_argument("--mysql-user", default="root")
    parser.add_argument("--mysql-password", default=os.environ.get("MAHJONG_MYSQL_PASSWORD", DEFAULT_MYSQL_PASSWORD))
    parser.add_argument("--mysql-db", default=TEST_DB, help="test database, created if missing")
    args = parser.parse_args()

    try:
        run(args)
    except TestFailure as failure:
        print(f"[reconnect-e2e] FAIL: {failure}", file=sys.stderr)
        return 1
    print("[reconnect-e2e] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
