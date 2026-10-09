#!/usr/bin/env python3
"""t4 end-to-end test: play one complete game against the real server.

Standard library only. It starts the compiled server, logs in, creates a room, readies up
and then plays the human seat (seat 0) to the end of the round, cross-checking every frame
against the frozen protocol:

  * handshake order login -> create_room -> ready -> game_start (PROTOCOL.md §9.1);
  * the human always answers in-window: `win` when offered (H1), claim `pong`/`chow` from
    `options` when offered, otherwise `discard` the tile just drawn (§4.5, §6.2);
  * every request/event carries the seq the window expects (§6.2), request seq is strictly
    increasing, `pass` never shows up in `action_event` (§5.8);
  * `state_sync` is authoritative: the locally tracked hand/melds/rivers/remaining/current
    seat must match it exactly before it is adopted (§5.9, H9);
  * H6: `seats[]` never leaks a hand, and hand_count + 3*melds stays within {13,14};
  * game_end: 4 distinct seats, Σ delta_score == 0, win details match the ruleset arithmetic
    (RULESET §6), the winner's flat hand has 14 + kong tiles and contains the winning tile;
  * the whole run produces zero `error` frames.

`--timeout-scenario` additionally starts the server with --action-timeout-ms 1000 and never
answers the first window: the server must play the default action (§5.6.2) while a late reply
with the stale seq stays silent.

Usage:
  python3 game/tests/room_full_game_test.py --server-bin build/game/mahjong_server
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
from collections import Counter

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BIN_CANDIDATES = (
    "build/game/mahjong_server",
    "build-release/game/mahjong_server",
    "build/game/Debug/mahjong_server",
    "build/game/Release/mahjong_server",
)
SEAT_COUNT = 4
# Tile order is m < p < s < z (the frozen encoding); the exact suit order never matters for
# the equality checks below because both sides are sorted with this same key, and the
# "server sent them sorted" checks only assert suit grouping + ascending ranks.
SUIT_ORDER = {"m": 0, "p": 1, "s": 2, "z": 3}


class TestFailure(AssertionError):
    pass


def check(condition: bool, label: str) -> None:
    if not condition:
        raise TestFailure(label)


class Client:
    def __init__(self, port: int, name: str, timeout: float = 15.0):
        self.name = name
        self.buffer = b""
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)

    def send(self, frame: dict) -> None:
        self.sock.sendall(json.dumps(frame, ensure_ascii=False).encode("utf-8") + b"\n")

    def recv_frame(self, timeout: float = 20.0) -> dict:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestFailure(f"{self.name}: timed out waiting for a frame (buffer={self.buffer!r})")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise TestFailure(f"{self.name}: timed out waiting for a frame (buffer={self.buffer!r})")
            if not chunk:
                raise TestFailure(f"{self.name}: connection closed while waiting for a frame")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def sorted_tiles(tiles: list[str]) -> list[str]:
    def key(code: str) -> tuple[int, int, str]:
        return (SUIT_ORDER.get(code[1], len(SUIT_ORDER)), int(code[0]), code[1])

    return sorted(tiles, key=key)


def is_grouped_and_sorted(tiles: list[str]) -> bool:
    """PROTOCOL.md §2.2 without pinning the suit order: each suit forms one contiguous block
    and the ranks ascend inside a block (works for the 108 and the 136 tile encoding)."""
    blocks: list[tuple[str, int]] = []
    for code in tiles:
        suit, rank = code[1], int(code[0])
        if blocks and blocks[-1][0] == suit:
            if rank < blocks[-1][1]:
                return False
            blocks[-1] = (suit, rank)
        else:
            blocks.append((suit, rank))
    suits = [suit for suit, _ in blocks]
    return len(set(suits)) == len(suits)


def sorted_melds(melds: list[dict]) -> list[tuple[str, tuple[str, ...]]]:
    return [(meld["type"], tuple(sorted_tiles(meld["tiles"]))) for meld in melds]


class TableModel:
    """The human's local view; used to prove state_sync is authoritative and consistent."""

    def note(self, frame: dict) -> None:
        self.recent.append(json.dumps(frame, ensure_ascii=False)[:200])
        self.recent = self.recent[-8:]

    def __init__(self, seat: int, hand: list[str], dealer: int):
        self.recent: list[str] = []
        self.seat = seat
        self.dealer = dealer
        self.hand = list(hand)
        self.melds: list[dict] = []
        self.rivers: dict[int, list[str]] = {s: [] for s in range(SEAT_COUNT)}
        self.remaining: int | None = None
        self.current_seat: int | None = None
        self.drawn: str | None = None
        self.total_tiles: int | None = None  # calibrated on the first state_sync
        self.ron_seen = False

    # ---- incremental events ---------------------------------------------------------
    def on_draw(self, seat: int, tile: str) -> None:
        check(seat == self.seat, f"draw must only reach the drawer (got seat {seat})")
        self.hand.append(tile)
        self.drawn = tile

    def on_discard_event(self, seat: int, tile: str) -> None:
        self.rivers[seat].append(tile)
        if seat == self.seat:
            self.hand.remove(tile)
            self.drawn = None

    def on_action_event(self, event: dict) -> None:
        action = event["action"]
        check(action != "pass", "pass must never be emitted as an action_event (§5.8)")
        seat = event["seat"]
        if action == "win":
            # from_seat is present exactly for a ron (§5.8); a tsumo tile was already added
            # by the preceding draw frame.
            if "from_seat" in event:
                self.ron_seen = True
                if seat == self.seat:
                    self.hand.extend(event["tiles"])
            return
        check("from_seat" in event, f"{action} must carry from_seat (§5.8)")
        from_seat = event["from_seat"]
        claimed = self.rivers[from_seat][-1]
        self.rivers[from_seat].pop()
        if seat == self.seat:
            if action == "pong":
                for _ in range(2):
                    self.hand.remove(claimed)
                meld_tiles = [claimed] * 3
            elif action == "kong":
                for _ in range(3):
                    self.hand.remove(claimed)
                meld_tiles = [claimed] * 4
            else:  # chow
                for tile in event["tiles"]:
                    self.hand.remove(tile)
                meld_tiles = list(event["tiles"]) + [claimed]
            self.melds.append({"type": action, "tiles": sorted_tiles(meld_tiles)})
            self.drawn = None

    # ---- authoritative snapshot -----------------------------------------------------
    def check_and_adopt(self, frame: dict) -> None:
        expected_hand = sorted_tiles(self.hand)
        check(sorted_tiles(frame["hand"]) == expected_hand,
              f"state_sync.hand mismatch: server={sorted_tiles(frame['hand'])} client={expected_hand}\n"
              "recent frames:\n  " + "\n  ".join(self.recent))
        check(sorted_melds(frame["melds"]) == sorted_melds(self.melds),
              f"state_sync.melds mismatch: server={frame['melds']} client={self.melds}")

        seats = frame["seats"]
        check(len(seats) == SEAT_COUNT, "state_sync.seats must hold 4 entries")
        for entry in seats:
            check("hand" not in entry, "seats[] must not leak a hand (H6)")
            concealed = entry["hand_count"] + 3 * len(entry["melds"])
            check(concealed in (13, 14),
                  f"hand_count + 3*melds must be 13 or 14, got {concealed} for seat {entry['seat']}")
            check(entry["discards"] == self.rivers[entry["seat"]],
                  f"river mismatch for seat {entry['seat']}: server={entry['discards']} "
                  f"client={self.rivers[entry['seat']]}")
        if self.remaining is not None:
            check(frame["remaining"] <= self.remaining, "remaining must never grow")
        # The private view must agree with the public one for the recipient's own seat.
        mine = seats[self.seat]
        check(len(frame["hand"]) == mine["hand_count"],
              "state_sync.hand disagrees with seats[self].hand_count")
        check(sorted_melds(frame["melds"]) == sorted_melds(mine["melds"]),
              "state_sync.melds disagrees with seats[self].melds")
        # Physical conservation, calibrated from the first authoritative snapshot so the test
        # survives a tile-set change (108 -> 136): wall pool + every hand + melds + rivers is
        # constant, and is one tile larger once a ron winner's flat hand also reports the tile
        # still lying in the river (PROTOCOL.md §5.10 H4).
        total = frame["remaining"] + sum(entry["hand_count"] for entry in seats)
        total += sum(len(meld["tiles"]) for entry in seats for meld in entry["melds"])
        total += sum(len(entry["discards"]) for entry in seats)
        if self.total_tiles is None:
            self.total_tiles = total
        else:
            allowed = {self.total_tiles}
            if self.ron_seen:
                allowed.add(self.total_tiles + 1)
            check(total in allowed,
                  f"tile conservation broken: {total} tiles accounted for in seq {frame['seq']}, "
                  f"expected {sorted(allowed)}")
        self.remaining = frame["remaining"]
        self.current_seat = frame["current_seat"]

def answer_request(client: Client, model: TableModel, request: dict) -> str:
    """Replies with a legal action and returns the frame type that was sent."""
    seq = request["seq"]
    window = request["window"]
    options = request.get("options", [])
    actions = {option["action"]: option for option in options}
    if "win" in actions:
        client.send({"type": "action", "action": "win", "tiles": [], "seq": seq})
        return "win"
    if window == "discard":
        tile = model.drawn if model.drawn is not None else sorted_tiles(model.hand)[-1]
        client.send({"type": "discard", "tile": tile, "seq": seq})
        return "discard"
    if "pong" in actions:
        client.send({"type": "action", "action": "pong", "tiles": actions["pong"]["tiles"], "seq": seq})
        return "pong"
    if "chow" in actions:
        client.send({"type": "action", "action": "chow", "tiles": actions["chow"]["tiles"], "seq": seq})
        return "chow"
    client.send({"type": "action", "action": "pass", "tiles": [], "seq": seq})
    return "pass"


def pass_handshake(client: Client, name: str) -> dict:
    client.send({"type": "login", "name": name})
    login_ok = client.recv_frame()
    check(login_ok["type"] == "login_ok", f"expected login_ok, got {login_ok}")
    client.send({"type": "create_room"})
    joined = client.recv_frame()
    check(joined["type"] == "room_joined", f"expected room_joined, got {joined}")
    check(joined["seat"] == 0, "the room creator must sit at seat 0")
    check(len(joined["seats"]) == SEAT_COUNT, "room_joined.seats must hold 4 entries")
    check(joined["seats"][0]["is_bot"] is False, "seat 0 must be the human")
    check(all(seat["is_bot"] for seat in joined["seats"][1:]), "seats 1..3 must be bots")
    client.send({"type": "ready"})
    start = client.recv_frame()
    check(start["type"] == "game_start", f"expected game_start, got {start}")
    check(start["seat"] == 0 and start["dealer"] == 0, "the human is the dealer")
    check(len(start["hand"]) == 14, "the dealer starts with 14 tiles")
    check(is_grouped_and_sorted(start["hand"]), f"game_start.hand must be sorted (§2.2): {start['hand']}")
    return start


def play_game(client: Client, model: TableModel, verbose: bool,
              max_requests: int | None = None) -> tuple[dict | None, dict]:
    """Plays until game_end (or until |max_requests| windows were answered)."""
    stats = {"requests": 0, "discard_events": 0, "draws": 0, "action_events": [], "state_syncs": 0,
             "errors": [], "max_seq": 0, "last_state_sync": None}
    previous_request_seq = 0
    previous_event_seq = 0
    while True:
        frame = client.recv_frame()
        model.note(frame)
        kind = frame["type"]
        if verbose:
            print(f"[e2e] <- {frame.get('type')} {json.dumps(frame, ensure_ascii=False)[:160]}")
        if kind == "error":
            stats["errors"].append(frame)
            continue
        if kind == "game_end":
            return frame, stats
        if kind == "draw":
            check(frame["seat"] == model.seat, "draw must only be sent to the drawer (§5.5)")
            check("seq" in frame and frame["seq"] > 0, "draw must carry seq (§5.5 H5)")
            model.on_draw(frame["seat"], frame["tile"])
            stats["draws"] += 1
            stats["max_seq"] = max(stats["max_seq"], frame["seq"])
            continue
        if kind == "discard_event":
            check(frame["seq"] > 0, "discard_event must carry seq (H5)")
            check(frame["seq"] >= previous_event_seq, "discard_event seq must not go backwards")
            previous_event_seq = frame["seq"]
            model.on_discard_event(frame["seat"], frame["tile"])
            stats["discard_events"] += 1
            continue
        if kind == "action_event":
            check(frame["seq"] > 0, "action_event must carry seq (H5)")
            check(frame["seq"] >= previous_event_seq, "action_event seq must not go backwards")
            previous_event_seq = frame["seq"]
            model.on_action_event(frame)
            stats["action_events"].append(frame)
            continue
        if kind == "state_sync":
            model.check_and_adopt(frame)
            stats["state_syncs"] += 1
            stats["last_state_sync"] = frame
            continue
        if kind == "action_request":
            check(frame["window"] in ("discard", "claim"), f"unknown window: {frame}")
            check(frame["seq"] > previous_request_seq, "action_request.seq must strictly increase")
            check(frame.get("deadline_ms", 0) > 0, "action_request needs deadline_ms")
            previous_request_seq = frame["seq"]
            stats["max_seq"] = max(stats["max_seq"], frame["seq"])
            stats["requests"] += 1
            answer_request(client, model, frame)
            # A mid-game `sync` must answer with an authoritative snapshot (§4.7).
            if stats["requests"] == 3:
                client.send({"type": "sync"})
            if max_requests is not None and stats["requests"] >= max_requests:
                return None, stats
            continue
        raise TestFailure(f"unexpected server frame: {frame}")


def verify_game_end(end: dict, stats: dict, model: TableModel) -> str:
    check(not stats["errors"], f"the game produced error frames: {stats['errors']}")
    check(stats["requests"] > 0 and stats["draws"] > 0, "the game never drove a turn")
    check(end["reason"] in ("win_ron", "win_tsumo", "draw_exhausted"), f"bad reason: {end['reason']}")
    results = end["results"]
    check(len(results) == SEAT_COUNT, "game_end.results must hold 4 entries")
    check(sorted(entry["seat"] for entry in results) == list(range(SEAT_COUNT)), "seats must be 0..3")
    total = sum(entry["delta_score"] for entry in results)
    check(total == 0, f"Σ delta_score must be zero, got {total}")

    if end["reason"] == "draw_exhausted":
        check(end["win"] is None, "a drawn game must report win = null")
        check(all(entry["delta_score"] == 0 for entry in results), "a drawn game scores nothing")
        check(model.remaining == 0, f"draw_exhausted with remaining={model.remaining}")
        return "draw_exhausted"

    win = end["win"]
    check(isinstance(win, dict), "a winning game must report a win object")
    winner = win["seat"]
    tsumo = win["tsumo"]
    check(tsumo == (end["reason"] == "win_tsumo"), "reason and win.tsumo disagree")
    multiplier = win["multiplier"]
    check(multiplier in (1, 4, 16), f"multiplier must be 1/4/16, got {multiplier}")
    check(win["per_payer"] == (2 * multiplier if tsumo else multiplier), "per_payer mismatch (RULESET §6)")
    check(win["total"] == (6 * multiplier if tsumo else multiplier), "total mismatch (RULESET §6)")
    check(results[winner]["delta_score"] == win["total"], "the winner must receive `total`")
    if tsumo:
        check(win["from_seat"] is None, "a tsumo win must report from_seat = null (§5.10)")
        for entry in results:
            if entry["seat"] != winner:
                check(entry["delta_score"] == -win["per_payer"], "each payer pays per_payer on a tsumo")
    else:
        from_seat = win["from_seat"]
        check(isinstance(from_seat, int) and from_seat != winner, "a ron win needs the discarder's seat")
        check(results[from_seat]["delta_score"] == -win["total"], "the discarder pays `total` on a ron")

    winner_entry = results[winner]
    flat = winner_entry["hand"]
    kongs = 0
    if stats["last_state_sync"] is not None:
        for meld in stats["last_state_sync"]["seats"][winner]["melds"]:
            if meld["type"] == "kong":
                kongs += 1
    check(len(flat) == 14 + kongs, f"the winner's flat hand must hold 14+kong tiles, got {len(flat)}")
    check(Counter(flat).most_common(1)[0][1] <= 4, "the flat hand cannot hold more than 4 copies of a face")
    check(win["tile"] in flat, "the winning tile must be part of the winner's flat hand")
    check(is_grouped_and_sorted(flat), f"the flat hand must be sorted (§2.2): {flat}")

    win_type = winner_entry["win_type"]
    check(win_type and win_type[-1] == ("tsumo" if tsumo else "ron"), f"win_type must end with the mode: {win_type}")
    for seat in range(SEAT_COUNT):
        if seat != winner:
            check(results[seat]["win_type"] == [], "only the winner reports win_type")
            check(results[seat]["hand"] == [], "only the winner reports a hand")
    return end["reason"]


def run_full_game(binary: str, seed: int, name: str, verbose: bool, port: int | None = None) -> dict:
    port = port or free_port()
    log = tempfile.NamedTemporaryFile(prefix="mahjong_e2e_", suffix=".log", delete=False)
    process = subprocess.Popen([binary, "--port", str(port), "--seed", str(seed)], stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_for_server(port, process)
        client = Client(port, name)
        try:
            start = pass_handshake(client, name)
            model = TableModel(start["seat"], start["hand"], start["dealer"])
            end, stats = play_game(client, model, verbose)
            reason = verify_game_end(end, stats, model)
            # After game_end a `sync` still returns the authoritative snapshot (§4.7).
            client.send({"type": "sync"})
            sync = client.recv_frame()
            check(sync["type"] == "state_sync", f"expected state_sync after game_end, got {sync}")
            model.check_and_adopt(sync)
            print(f"[e2e] game finished: reason={reason} requests={stats['requests']} draws={stats['draws']} "
                  f"discards={stats['discard_events']} action_events={len(stats['action_events'])} "
                  f"state_syncs={stats['state_syncs']} max_seq={stats['max_seq']}")
            return {"reason": reason, "stats": stats, "end": end}
        finally:
            client.close()
    finally:
        process.send_signal(2)  # SIGINT: the server must shut down gracefully
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        log.close()
        if process.returncode != 0:
            with open(log.name, "r", encoding="utf-8", errors="replace") as handle:
                print(handle.read()[-4000:], file=sys.stderr)
            raise TestFailure(f"the server exited with code {process.returncode} (SIGINT must be graceful)")
        os.unlink(log.name)


def run_timeout_scenario(binary: str, seed: int, verbose: bool, port: int | None = None) -> None:
    """PROTOCOL.md §5.6.2: an unanswered window is played with the default action."""
    port = port or free_port()
    log = tempfile.NamedTemporaryFile(prefix="mahjong_e2e_timeout_", suffix=".log", delete=False)
    process = subprocess.Popen([binary, "--port", str(port), "--seed", str(seed), "--action-timeout-ms", "1000"],
                               stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_for_server(port, process)
        client = Client(port, "等待超时")
        try:
            start = pass_handshake(client, "等待超时")
            # Do not answer the dealer's first window at all; the server must play for us.
            deadline = time.monotonic() + 8
            discarded = None
            while time.monotonic() < deadline and discarded is None:
                frame = client.recv_frame(timeout=8)
                if verbose:
                    print(f"[e2e-timeout] <- {json.dumps(frame, ensure_ascii=False)[:160]}")
                if frame["type"] == "discard_event" and frame["seat"] == 0:
                    discarded = frame["tile"]
                elif frame["type"] == "error":
                    raise TestFailure(f"timeout scenario produced an error: {frame}")
            check(discarded is not None, "the server never played the timed-out window")
            # A dealer's first window has no draw record: the default is the largest tile.
            check(discarded == start["hand"][-1],
                  f"expected the largest tile {start['hand'][-1]} to be discarded, got {discarded}")

            # A late reply carrying the expired window seq stays silent (§6.3).
            client.send({"type": "discard", "tile": start["hand"][0], "seq": 1})
            deadline = time.monotonic() + 0.6
            while time.monotonic() < deadline:
                client.sock.settimeout(max(0.05, deadline - time.monotonic()))
                try:
                    chunk = client.sock.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                for line in chunk.split(b"\n"):
                    if line:
                        frame = json.loads(line.decode("utf-8"))
                        check(frame["type"] != "error",
                              f"a stale seq must be dropped silently, got {frame}")
            print("[e2e] timeout scenario ok: default discard =", discarded)
        finally:
            client.close()
    finally:
        process.send_signal(2)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
        os.unlink(log.name)


def run_disconnect_scenario(binary: str, seed: int, verbose: bool, port: int | None = None) -> None:
    """PROTOCOL.md §8.5: a mid-game disconnect entrusts the seat and the round keeps going."""
    port = port or free_port()
    log = tempfile.NamedTemporaryFile(prefix="mahjong_e2e_disconnect_", suffix=".log", delete=False)
    process = subprocess.Popen([binary, "--port", str(port), "--seed", str(seed)], stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_for_server(port, process)
        client = Client(port, "掉线玩家")
        start = pass_handshake(client, "掉线玩家")
        model = TableModel(start["seat"], start["hand"], start["dealer"])
        # Play two windows, checking every snapshot, and then vanish abruptly.
        end, stats = play_game(client, model, verbose, max_requests=2)
        check(end is None, "the game ended before the disconnect could be exercised")
        check(not stats["errors"], f"disconnect scenario produced an error: {stats['errors']}")
        client.sock.close()

        log_path = log.name
        # t31: the server now paces every AI decision (default 900 ms), so the remainder of a
        # round takes tens of seconds instead of ~1s. The budget must cover a whole paced round
        # (~40s measured) or the scenario reports a stall that never happened.
        deadline = time.monotonic() + 120
        text = ""
        while time.monotonic() < deadline:
            with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            if "game over" in text:
                break
            time.sleep(0.2)
        check("keeps playing (seat entrusted)" in text,
              "the room must survive a mid-game disconnect and entrust the seat")
        check("game over" in text, "the round must still reach its end after the disconnect")
        if verbose:
            print("[e2e-disconnect] server log tail:")
            print("\n".join(text.splitlines()[-6:]))
        print("[e2e] disconnect scenario ok: the round finished with the seat entrusted")
    finally:
        process.send_signal(2)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        log.close()
        os.unlink(log.name)


def run_multi_room_scenario(binary: str, seed: int, verbose: bool, port: int | None = None) -> None:
    """t27: every room of one server must deal a different wall, and the mapping must stay a
    pure function of (server seed, room creation order) so a fixed --seed is reproducible.

    It drives the real gateway/lobby: each client creates its own room (r-0001, r-0002, ...)
    and readies up, which yields that room's `game_start` hand.
    """

    def collect(server_port: int, rooms: int) -> list[tuple[str, list[str]]]:
        results: list[tuple[str, list[str]]] = []
        clients: list[Client] = []
        try:
            for index in range(rooms):
                client = Client(server_port, f"multi-room-{index}")
                clients.append(client)
                client.send({"type": "login", "name": f"多房玩家{index}"})
                login_ok = client.recv_frame()
                check(login_ok["type"] == "login_ok", f"expected login_ok, got {login_ok}")
                client.send({"type": "create_room"})
                joined = client.recv_frame()
                check(joined["type"] == "room_joined", f"expected room_joined, got {joined}")
                client.send({"type": "ready"})
                start = client.recv_frame()
                check(start["type"] == "game_start", f"expected game_start, got {start}")
                check(len(start["hand"]) == 14, "the dealer starts with 14 tiles")
                results.append((joined["room_id"], start["hand"]))
            return results
        finally:
            for client in clients:
                client.close()

    def one_server(server_seed: int) -> list[tuple[str, list[str]]]:
        server_port = port or free_port()
        log = tempfile.NamedTemporaryFile(prefix="mahjong_e2e_multiroom_", suffix=".log", delete=False)
        process = subprocess.Popen([binary, "--port", str(server_port), "--seed", str(server_seed)],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for_server(server_port, process)
            return collect(server_port, 3)
        finally:
            process.send_signal(2)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            log.close()
            os.unlink(log.name)

    first = one_server(seed)
    second = one_server(seed)

    check([room for room, _ in first] == ["r-0001", "r-0002", "r-0003"], f"unexpected room ids: {first}")
    # (1) the three rooms of one server must not deal the same wall
    for i in range(len(first)):
        for j in range(i + 1, len(first)):
            check(first[i][1] != first[j][1],
                  f"rooms {first[i][0]} and {first[j][0]} dealt identical hands (wall seed not per room): "
                  f"{first[i][1]}")
    # (2) same server seed + same creation order => the same walls, restart after restart
    for i in range(len(first)):
        check(first[i][1] == second[i][1],
              f"room index {i} is not reproducible across server runs: {first[i][1]} vs {second[i][1]}")

    # (3) a different server seed must change the walls
    other = one_server(seed + 1)
    check(first[0][1] != other[0][1], "a different --seed dealt the very same hand")

    if verbose:
        print("[e2e-multiroom] rooms:", [(room, " ".join(hand)) for room, hand in first])
    print(f"[e2e] multi-room scenario ok: 3 rooms of one server dealt 3 different walls "
          f"(reproducible across restarts; base seed {seed})")


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
        "cannot find mahjong_server; build it with `cmake --build build --target mahjong_server` or pass --server-bin"
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
    parser.add_argument("--server-bin", default=None)
    parser.add_argument("--seed", type=int, default=20251005)
    parser.add_argument("--port", type=int, default=None,
                        help="fixed listen port (default: an ephemeral free port per scenario)")
    parser.add_argument("--games", type=int, default=1, help="play this many rounds with consecutive seeds")
    parser.add_argument("--timeout-scenario", action="store_true")
    parser.add_argument("--disconnect-scenario", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = find_binary(args.server_bin)
    print(f"[e2e] binary={binary}")
    reasons = []
    for index in range(args.games):
        seed = args.seed + index
        result = run_full_game(binary, seed, "端到端玩家", args.verbose, args.port)
        reasons.append(result["reason"])
    if args.timeout_scenario:
        run_timeout_scenario(binary, args.seed, args.verbose, args.port)
    if args.disconnect_scenario:
        run_disconnect_scenario(binary, args.seed + args.games, args.verbose, args.port)
    # Always run: one server dealing several rooms the same wall was a real defect (t27).
    run_multi_room_scenario(binary, args.seed + args.games + 1, args.verbose, args.port)
    print(f"[e2e] OK: {len(reasons)} full game(s), reasons={reasons}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
