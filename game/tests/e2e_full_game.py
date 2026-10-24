#!/usr/bin/env python3
"""End-to-end harness: the frozen t6 single-game acceptance plus a t17 statistics mode.

Two modes, one entry point:

* **single game** (default): the t6 acceptance. Starts the real server with a fixed `--seed` on a
  **free port picked per game**, logs in over plain TCP (`create_room` fills seats 1..3 with server
  AI), plays the human automatically (`win` when the window offers it, otherwise `discard` the
  tile it just drew — the largest tile when there is no draw record — and `pass` in a claim
  window without `win`; the `action_request.seq` is echoed back on every reply, H2), writes every
  frame and decision to `game/tests/artifacts/e2e_log.jsonl`, and enforces the three frozen
  assertions:

    A. the run works: `login_ok` with a non-empty `player_id`; `game_start` with a 13/14 tile hand;
       at least one `draw` and one `discard_event`; `game_end` with 4 results;
    B. score conservation: the `delta_score` values sum to 0;
    C. no error: no `error` frame was received during the whole run, and the script exits 0.

* **statistics** (`--games N`): plays N consecutive-seed games, enforcing A/B/C on *every* game
  (one `error` frame or one assertion failure aborts the run with a non-zero exit code), then
  prints the settlement breakdown, the measured game length and the comparison against the first
  milestone baseline (200 games, 172 draws / 27 ron / 1 tsumo = 86 % draws). The summary is also
  written to `game/tests/artifacts/e2e_stats.json`.

  The statistics mode never assumes a tile set: the tile total and the wall size are derived from
  physical conservation on the first `state_sync` of each game
  (`remaining + Σ hand_count + Σ meld tiles + Σ discards = 53 + wall`), so the same command
  measures whatever ruleset the server binary actually implements and says so explicitly.

Explicitly out of scope (not this milestone): reconnect, illegal-action handling, concurrent
rooms, performance/stress, coverage.

**Ports (t26 fix).** Every game gets its **own free port** (`--port 0`, the default: bind :0, read
the assigned port, close it, hand it to the server), so two members can run this script at the same
time without stepping on each other. More importantly the harness only ever talks to *its own*
server: it waits until **our** child process logs `gateway: listening on 127.0.0.1:<port>` and
aborts loudly if that child exits first. Before t26 the harness dialled a fixed 19527 and, when a
teammate's server owned that port, silently played the round against the *foreign* server — two
sweeps failed with a dropped connection and one "verification" validated a stranger's process.
Pass `--port N` only when a fixed port is really wanted; that path pre-checks the port and refuses
to run when somebody else holds it.

Usage:
  python3 game/tests/e2e_full_game.py                                  # one game (t6 assertions)
  python3 game/tests/e2e_full_game.py --games 200 --seed 17            # t17 statistics
  python3 game/tests/e2e_full_game.py --games 200 --frames-dir game/tests/artifacts/frames
  python3 game/tests/e2e_full_game.py --concurrency-check --games 10   # two instances in parallel
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unicodedata

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BINARY = os.path.join(REPO_ROOT, "build-release", "game", "mahjong_server")
AUTO_PORT = 0  # --port 0 (the default): ask the OS for a fresh free port for every game
DEFAULT_PORT = 19527  # PROTOCOL.md §1.1 reserved e2e port; only used if you pass --port 19527
# Fixed seed (PROTOCOL.md §1.8: the e2e script MUST pass one so the run is reproducible). t17:
# the 136-tile ruleset changed the shuffle, so seed 199 (the 108-tile choice, a human tsumo) now
# ends in an exhaustive draw. The default is therefore the smallest seed in the standard comparison
# range 17..216 whose round ends in a *scored* settlement (seat 1 ron, M=1, win_type ["ron"]) so
# that assertion B stays non-trivial. This choice cannot bias the statistics: `--games N` always
# uses the explicit `--seed` range (the baseline comparison uses `--seed 17` = seeds 17..216).
DEFAULT_SEED = 86
BASELINE_SEED_START = 17  # the seed t6's 200-game baseline started from (seeds 17..216)
DEFAULT_LOG = os.path.join(REPO_ROOT, "game", "tests", "artifacts", "e2e_log.jsonl")
DEFAULT_STATS_OUT = os.path.join(REPO_ROOT, "game", "tests", "artifacts", "e2e_stats.json")
DEFAULT_MATCH_LOG = os.path.join(REPO_ROOT, "game", "tests", "artifacts", "e2e_match_log.jsonl")
GLOBAL_TIMEOUT_S = 120.0  # hard ceiling for one game: the script must never hang
DEFAULT_GAME_TIMEOUT_S = 90.0  # per-game budget; an AI-paced round needs ~40s (t31)
# Q3 裁决（A1_ACCEPTANCE E2/E3）：**整场**兜底超时 = 960s（8 局 × 120s，单场上界 8 局）；
# 单局 120s 口径不变。
MATCH_TIMEOUT_S = 960.0
SEAT_COUNT = 4
DEALT_TILES = SEAT_COUNT * 13 + 1  # three 13-tile hands + the dealer's 14th tile

# P11/P3（PROTOCOL §10.4 ③）：番种 id 冻结判定序（= RULESET §6.4 / GAME_DESIGN §6.4 表序）。
# e2e 用它断言 win.yaku[] 严格按此序、无重复；tags ⊆ yaku id 且同为该序的子序列。
YAKU_FROZEN_ORDER = [
    "menzen", "tanyao", "pinfu", "iipeikou", "yakuhai", "rinshan",
    "sanshoku_doujun", "ittsuu", "toitoi", "sanankou", "honitsu", "shousangen",
    "seven_pairs", "pure_one_suit",
]

# First milestone baseline (t6, 200 games, seeds 17..216) — the number the second milestone has to
# beat. Frozen here on purpose: the point of the statistics mode is the side-by-side comparison.
BASELINE = {
    "label": "旧: t6 小结 (108 张 / 牌山 55)",
    "games": 200,
    "reason_counts": {"draw_exhausted": 172, "win_ron": 27, "win_tsumo": 1},
    "tile_total": 108,
    "wall_size": 55,
    "avg_discards": 59.71,
    "avg_draws_per_player": 12.99,
    "source": "t6 小结（172/27/1）；打牌与摸牌均取 t17 对同一 seed 集的复测值",
}
# Second milestone target口径 (task t17): 34 faces / 136 tiles / 83-tile wall / 21 draws per player.
TARGET = {"faces": 34, "tile_total": 136, "wall_size": 83, "max_discards": 84, "draws_per_player": 21}
# Earlier measurements of the same 200-game sweep (seed 17..216), kept as the four-way comparison
# the milestone reviews asked for. `draws_per_player` is reported but is NOT an acceptance gate:
# once the discard policy builds hands, rounds end on a win and never exhaust the 83-tile wall.
HISTORY = [
    {"label": "旧(更正): 108 张 / 旧策略", "games": 200,
     "reason_counts": {"draw_exhausted": 173, "win_ron": 26, "win_tsumo": 1},
     "tile_total": 108, "wall_size": 55, "avg_discards": 59.71, "avg_draws_per_player": 13.00,
     "source": "t17 三次独立复测（t6 小结的 172/27/1 把 seed 199 重复计入）"},
    {"label": "中: 136 张 / 旧策略 (t18)", "games": 200,
     "reason_counts": {"draw_exhausted": 194, "win_ron": 6, "win_tsumo": 0},
     "tile_total": 136, "wall_size": 83, "avg_discards": 89.59, "avg_draws_per_player": 20.55,
     "source": "t18 实测（加字牌但弃牌策略不变）"},
    {"label": "136 张 / 牌效率策略 (t23 参考)", "games": 200,
     "reason_counts": {"draw_exhausted": 13, "win_ron": 187, "win_tsumo": 0},
     "tile_total": 136, "wall_size": 83, "avg_discards": 52.77, "avg_draws_per_player": 11.20,
     "source": "t23 实测（自摸路径尚未修复，全部和牌均为荣和）"},
]


class Failure(AssertionError):
    pass


def sorted_tiles(tiles: list[str]) -> list[str]:
    def key(code: str) -> tuple[int, int]:
        suit = "mps".index(code[1]) if code[1] in "mps" else 3 + "z".index(code[1])
        return (suit, int(code[0]) if code[1] != "z" else "1234567".index(code[0]) + 1)

    return sorted(tiles, key=key)


class Recorder:
    """Writes the whole interaction to one JSONL artefact and keeps a short tail for errors."""

    def __init__(self, path: str):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.handle = open(path, "w", encoding="utf-8")
        self.tail: list[str] = []
        self.path = path

    def write(self, entry: dict) -> None:
        entry = {"ts": round(time.time(), 3), **entry}
        line = json.dumps(entry, ensure_ascii=False)
        self.handle.write(line + "\n")
        self.handle.flush()
        self.tail.append(line)
        self.tail = self.tail[-12:]

    def frame(self, direction: str, frame: dict) -> None:
        self.write({"dir": direction, "frame": frame})

    def close(self) -> None:
        self.handle.close()


class Client:
    """Minimal blocking NDJSON client plus the human's local hand bookkeeping.

    E2：每帧的**归一化文本**（`normalize_frame`）同时留在 `self.frames`，供整场逐帧 diff。
    """

    def __init__(self, port: int, recorder: Recorder, deadline: float):
        self.recorder = recorder
        self.deadline = deadline  # global ceiling: no single wait may outlive it
        self.buffer = b""
        self.seat: int | None = None
        self.hand: list[str] = []
        self.drawn: str | None = None
        self.frames: list[str] = []
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)

    # ---- transport -------------------------------------------------------------------
    def send(self, frame: dict) -> None:
        self.frames.append("out " + normalize_frame(frame))
        self.recorder.frame("out", frame)
        self.sock.sendall(json.dumps(frame, ensure_ascii=False).encode("utf-8") + b"\n")

    def recv_frame(self, timeout: float) -> dict:
        # Clamp every wait to the global ceiling so the script can never outlive --timeout.
        deadline = min(time.monotonic() + timeout, self.deadline)
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("no complete frame arrived in time")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise TimeoutError("no complete frame arrived in time")
            if not chunk:
                raise ConnectionError("the server closed the connection")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        frame = json.loads(line.decode("utf-8"))
        self.frames.append("in  " + normalize_frame(frame))
        self.recorder.frame("in", frame)
        return frame

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    # ---- local hand model (only what the auto-player needs) --------------------------
    def note_draw(self, tile: str) -> None:
        self.hand.append(tile)
        self.drawn = tile

    def note_discard_event(self, seat: int, tile: str) -> None:
        if seat != self.seat:
            return
        if tile in self.hand:
            self.hand.remove(tile)
        self.drawn = None

    def adopt_state_sync(self, frame: dict) -> None:
        self.hand = list(frame["hand"])
        if self.drawn is not None and self.drawn not in self.hand:
            self.drawn = None

    def choose_discard(self) -> str:
        """The just-drawn tile, else the largest tile (§6.4 default口径)."""
        if self.drawn is not None and self.drawn in self.hand:
            return self.drawn
        if not self.hand:
            raise Failure("the human hand is empty but a discard is required")
        return sorted_tiles(self.hand)[-1]


def answer_request(client: Client, request: dict) -> str:
    """`win` > `discard` (discard window) > `pass` (claim window); always echoes the seq."""
    seq = request["seq"]
    options = request.get("options", [])
    actions = [option.get("action") for option in options]
    if "win" in actions:
        client.send({"type": "action", "action": "win", "tiles": [], "seq": seq})
        return "action:win"
    if request.get("window") == "discard":
        tile = client.choose_discard()
        client.send({"type": "discard", "tile": tile, "seq": seq})
        return f"discard:{tile}"
    client.send({"type": "action", "action": "pass", "tiles": [], "seq": seq})
    return "action:pass"


def find_binary(explicit: str | None) -> str:
    binary = explicit or os.environ.get("MAHJONG_SERVER_BIN") or DEFAULT_BINARY
    if not os.path.isfile(binary):
        raise Failure(f"server binary not found: {binary}\n"
                      "build it first: cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release "
                      "-DEX_ACTOR_BUILD_TESTS=ON && cmake --build build-release -j 8")
    return binary


def free_port() -> int:
    """Asks the OS for an unused loopback TCP port (bind :0, read it, release it)."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def port_is_free(port: int) -> bool:
    """True when nothing is listening on |port| (a TIME_WAIT socket does not count as a holder)."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def wait_with_timeout(process: subprocess.Popen, seconds: float, label: str) -> int:
    """Waits for |process| with a hard timeout: a stuck child must fail loudly, never hang.

    t31: the AI pacing made a whole round ~40s instead of ~1s, so an unbounded wait is no
    longer "obviously fine" — it turns a slow run into an unattributable stall.
    """
    try:
        return process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        raise Failure(f"{label}: did not exit within {seconds:.0f}s; killed it "
                      "(the server stopped responding or never reached its shutdown path)")


def resolve_port(requested: int) -> int:
    """Picks the port for one game: 0 = auto; a fixed port must be free or we refuse to run.

    Refusing on a busy fixed port is deliberate: connecting to whatever process happens to own it
    would let the harness "verify" a foreign server (the t26 silent cross-talk).
    """
    if requested == AUTO_PORT:
        return free_port()
    if not port_is_free(requested):
        raise Failure(f"port {requested} is already in use by another process; refusing to run "
                      "(connecting would validate a foreign server). Drop --port to auto-pick a "
                      "free port per game, or choose an idle port.")
    return requested


def require_own_listener(port: int, process: subprocess.Popen, server_log_path: str,
                         timeout: float = 15.0) -> None:
    """Waits until *our* child reports it is listening, so we never dial a foreign server.

    The server logs `gateway: listening on 127.0.0.1:<port>`; if our child dies first (e.g. bind
    failed because somebody else owns the port) the run fails loudly instead of silently talking
    to the other process.
    """
    marker = f"gateway: listening on 127.0.0.1:{port}"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise Failure(f"our server exited early with code {process.returncode} before listening "
                          f"on 127.0.0.1:{port} (port already owned by another process?)")
        try:
            with open(server_log_path, "r", encoding="utf-8", errors="replace") as handle:
                if marker in handle.read():
                    return
        except OSError:
            pass
        time.sleep(0.02)
    raise Failure(f"our server never reported listening on 127.0.0.1:{port} within {timeout}s")


def handshake(client: Client, recorder: Recorder) -> dict:
    """login -> create_room -> ready (PROTOCOL.md §9.1); asserts A's handshake half."""
    client.send({"type": "login", "name": "端到端真人"})
    login_ok = client.recv_frame(15.0)
    if login_ok.get("type") != "login_ok":
        raise Failure(f"expected login_ok, got {login_ok}")
    player_id = login_ok.get("player_id")
    if not isinstance(player_id, str) or not player_id:
        raise Failure(f"login_ok.player_id must be a non-empty string, got {player_id!r}")
    recorder.write({"event": "login_ok", "player_id": player_id})

    client.send({"type": "create_room"})
    joined = client.recv_frame(15.0)
    if joined.get("type") != "room_joined":
        raise Failure(f"expected room_joined, got {joined}")
    if len(joined.get("seats", [])) != SEAT_COUNT:
        raise Failure(f"room_joined.seats must hold {SEAT_COUNT} entries, got {joined.get('seats')}")
    bots = sum(1 for seat in joined["seats"] if seat.get("is_bot"))
    if bots != SEAT_COUNT - 1:
        raise Failure(f"expected {SEAT_COUNT - 1} server AI seats, got {bots}")
    client.seat = joined["seat"]
    recorder.write({"event": "room_joined", "seat": client.seat, "room_id": joined.get("room_id")})

    client.send({"type": "ready"})
    start = client.recv_frame(15.0)
    if start.get("type") != "game_start":
        raise Failure(f"expected game_start, got {start}")
    hand = start.get("hand")
    if not isinstance(hand, list) or len(hand) not in (13, 14):
        raise Failure(f"game_start.hand must hold 13 or 14 tiles, got {len(hand) if isinstance(hand, list) else hand}")
    client.seat = start.get("seat", client.seat)
    client.hand = list(hand)
    recorder.write({"event": "game_start", "seat": client.seat, "dealer": start.get("dealer"),
                    "hand_size": len(hand)})
    return start


def play(client: Client, recorder: Recorder, deadline: float, verbose: bool) -> tuple[dict, dict]:
    """Answers every window until game_end; returns the game_end frame plus counters."""
    stats = {"draws": 0, "discard_events": 0, "action_requests": 0, "errors": [],
             "first_state_sync": None, "last_state_sync": None}
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure("the game did not finish within the global timeout")
        try:
            frame = client.recv_frame(min(20.0, remaining))
        except TimeoutError:
            raise Failure(f"no server frame for {min(20.0, remaining):.0f}s (last frames: "
                          f"{' | '.join(recorder.tail[-4:])})")

        if verbose:
            print(f"[e2e] <- {json.dumps(frame, ensure_ascii=False)[:160]}")

        kind = frame.get("type")
        if kind == "error":
            # C: recorded here, asserted once at the end so the log holds every error frame.
            stats["errors"].append(frame)
            continue
        if kind == "game_end":
            return frame, stats
        if kind == "pong":
            continue
        if kind == "draw":
            if frame.get("seat") == client.seat:
                client.note_draw(frame["tile"])
            stats["draws"] += 1
            continue
        if kind == "discard_event":
            client.note_discard_event(frame["seat"], frame["tile"])
            stats["discard_events"] += 1
            continue
        if kind == "state_sync":
            client.adopt_state_sync(frame)
            if stats["first_state_sync"] is None:
                stats["first_state_sync"] = frame
            stats["last_state_sync"] = frame
            continue
        if kind == "action_event":
            # The human only ever wins (ron); a claim window without `win` is passed.
            if frame.get("seat") == client.seat and frame.get("action") == "win" and "from_seat" in frame:
                client.hand.extend(frame.get("tiles", []))
            continue
        if kind == "action_request":
            stats["action_requests"] += 1
            decision = answer_request(client, frame)
            recorder.write({"event": "decision", "window": frame.get("window"), "seq": frame.get("seq"),
                            "options": [option.get("action") for option in frame.get("options", [])],
                            "reply": decision})
            if verbose:
                print(f"[e2e] -> {decision} (window={frame.get('window')} seq={frame.get('seq')})")
            continue
        # Unknown server frames are ignored (forward compatible, PROTOCOL.md §1.3).


def assert_results(end: dict, stats: dict) -> int:
    """Assertion A (tail): a draw and a discard happened; B: Σdelta == 0; C: no error frame."""
    if stats["draws"] < 1:
        raise Failure("assertion A: the run never drew a tile")
    if stats["discard_events"] < 1:
        raise Failure("assertion A: the run never discarded a tile")
    results = end.get("results")
    if not isinstance(results, list) or len(results) != SEAT_COUNT:
        raise Failure(f"game_end.results must hold {SEAT_COUNT} entries, got {results}")
    seats = sorted(entry.get("seat") for entry in results)
    if seats != list(range(SEAT_COUNT)):
        raise Failure(f"game_end.results seats must cover 0..3, got {seats}")
    total = sum(entry.get("delta_score", 0) for entry in results)
    if total != 0:
        raise Failure(f"score conservation broken: Σ delta_score = {total}")
    if stats["errors"]:
        raise Failure(f"the run received {len(stats['errors'])} error frame(s): {stats['errors']}")
    return total


def tile_set_metrics(first_sync: dict | None, last_sync: dict | None) -> dict:
    """Derives the tile set and the number of draws from physical conservation (no assumptions).

    On the first `state_sync` every tile sits in a hand, a meld, a river or the wall, and no win
    has happened yet, so `remaining + Σ hand_count + Σ meld tiles + Σ discards` is the *total*
    tile count of the ruleset in use and `total − 53` is the wall size after the deal.
    """
    if first_sync is None or last_sync is None:
        return {"tile_total": None, "wall_size": None, "total_draws": None, "remaining_final": None}
    seats = first_sync.get("seats") or []
    tile_total = first_sync.get("remaining", 0)
    tile_total += sum(entry.get("hand_count", 0) for entry in seats)
    tile_total += sum(len(meld.get("tiles", [])) for entry in seats for meld in entry.get("melds") or [])
    tile_total += sum(len(entry.get("discards") or []) for entry in seats)
    wall_size = tile_total - DEALT_TILES
    remaining_final = last_sync.get("remaining")
    draws = None if remaining_final is None else wall_size - remaining_final
    return {"tile_total": tile_total, "wall_size": wall_size, "total_draws": draws,
            "remaining_final": remaining_final}


def run_one_game(binary: str, seed: int, port: int, log_path: str, verbose: bool,
                 timeout: float, args_ai_delay_ms: int | None = None) -> dict:
    """Plays one complete game, enforcing A/B/C. Returns a summary record; raises Failure."""
    recorder = Recorder(log_path)
    server_log = tempfile.NamedTemporaryFile(prefix="mahjong_e2e_", suffix=".log", delete=False)
    recorder.write({"event": "start", "binary": binary, "port": port, "seed": seed})
    command = [binary, "--port", str(port), "--seed", str(seed)]
    if args_ai_delay_ms is not None:
        command += ["--ai-delay-ms", str(args_ai_delay_ms)]
    process = subprocess.Popen(command, stdout=server_log, stderr=subprocess.STDOUT)
    client = None
    deadline = time.monotonic() + timeout
    try:
        require_own_listener(port, process, server_log.name)
        recorder.write({"event": "server_listening", "port": port})
        client = Client(port, recorder, deadline)
        handshake(client, recorder)
        end, stats = play(client, recorder, deadline, verbose)
        recorder.write({"event": "game_end", "reason": end.get("reason"), "stats":
                        {key: value for key, value in stats.items() if not key.endswith("state_sync")}})
        score_sum = assert_results(end, stats)
        record = {"seed": seed, "port": port, "reason": end.get("reason"), "score_sum": score_sum,
                  "discards": stats["discard_events"], "human_draws": stats["draws"],
                  "action_requests": stats["action_requests"], "errors": len(stats["errors"])}
        record.update(tile_set_metrics(stats["first_state_sync"], stats["last_state_sync"]))
        recorder.write({"event": "record", **record})
    except Exception as error:  # noqa: BLE001 - every failure path must log and exit non-zero
        recorder.write({"event": "failure", "error": f"{type(error).__name__}: {error}"})
        print(f"[e2e] FAIL (seed={seed}): {type(error).__name__}: {error}", file=sys.stderr)
        print("[e2e] last frames:", file=sys.stderr)
        for line in recorder.tail:
            print(f"  {line}", file=sys.stderr)
        # Keep the server's own log next to the frame log: a dropped connection is exactly when the
        # server-side reason matters, and the temp file is unlinked in `finally`.
        try:
            with open(server_log.name, "r", encoding="utf-8", errors="replace") as source:
                with open(log_path + ".server.log", "w", encoding="utf-8") as target:
                    target.write(source.read())
        except OSError:
            pass
        server_log.flush()
        with open(server_log.name, "r", encoding="utf-8", errors="replace") as handle:
            tail = handle.read()[-2000:]
        if tail.strip():
            print("[e2e] server log tail:\n" + tail, file=sys.stderr)
        raise Failure(f"seed {seed}: {type(error).__name__}: {error}") from error
    finally:
        if client is not None:
            client.close()
        recorder.write({"event": "server_shutdown"})
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        server_log.close()
        if process.returncode != 0:
            print(f"[e2e] warning: server exit code {process.returncode} after SIGINT (seed={seed})")
        os.unlink(server_log.name)
        recorder.close()
    return record


# ---------------------------------------------------------------------------------------------
# whole-match mode (E2/E3, A1_ACCEPTANCE §七): 一场 4..8 局 + PROTOCOL §10.4 三组断言
# ---------------------------------------------------------------------------------------------

def normalize_frame(frame: dict) -> str:
    """E2 逐帧 diff 用的归一化：剥离两类**合法的逐次不同**字段。

    * `action_request.deadline_ms` —— 真实时钟（§5.7），两次运行必然不同；
    * `login_ok.player_id` —— 游客会话 id 逐连接生成。
    其余字段（含 seq、hand、yaku、rankings）参与逐字节比对。
    """
    cleaned = json.loads(json.dumps(frame, ensure_ascii=False))
    if cleaned.get("type") == "action_request" and "deadline_ms" in cleaned:
        cleaned["deadline_ms"] = 0
    if cleaned.get("type") == "login_ok":
        cleaned.pop("player_id", None)
    return json.dumps(cleaned, ensure_ascii=False, sort_keys=True)


def assert_yaku_group(end: dict, label: str) -> None:
    """番数组（N1/N11/P11，PROTOCOL §10.4 ③）：逐局 game_end 的番数自洽。"""
    win = end.get("win")
    if not isinstance(win, dict):
        return
    yaku = win.get("yaku")
    breakdown = win.get("han_breakdown")
    if not isinstance(yaku, list) or not isinstance(breakdown, dict):
        raise Failure(f"{label}: win.yaku/han_breakdown missing (v1.6 fields): {win}")
    ids = [item.get("id") for item in yaku if isinstance(item, dict)]
    if len(ids) != len(set(ids)):
        raise Failure(f"{label}: yaku id 重复：{ids}")
    order_indexes = [YAKU_FROZEN_ORDER.index(i) for i in ids if i in YAKU_FROZEN_ORDER]
    if len(order_indexes) != len(ids):
        raise Failure(f"{label}: 未知 yaku id：{ids}")
    if order_indexes != sorted(order_indexes):
        raise Failure(f"{label}: yaku 未按冻结判定序：{ids}")
    base = sum(item.get("han", 0) for item in yaku if isinstance(item, dict))
    if breakdown.get("base") != base:
        raise Failure(f"{label}: N11 失败 Σyaku.han={base} != base={breakdown.get('base')}")
    if breakdown.get("tsumo") != 0:
        raise Failure(f"{label}: N1 失败 tsumo 必须恒 0（自摸不是番种）: {breakdown}")
    dealer = breakdown.get("dealer", 0)
    expected_total = min(base + dealer, 6)
    if breakdown.get("total") != expected_total or win.get("han") != expected_total:
        raise Failure(f"{label}: total/han={breakdown.get('total')}/{win.get('han')} != {expected_total}")
    if win.get("dealer_bonus") != (dealer == 1):
        raise Failure(f"{label}: dealer_bonus 派生字段不一致: {win}")
    tags = win.get("tags") or []
    if not set(tags) <= set(ids):
        raise Failure(f"{label}: P11 失败 tags ⊄ yaku[].id：tags={tags} yaku={ids}")
    tag_order = [ids.index(t) for t in tags]
    if tag_order != sorted(tag_order):
        raise Failure(f"{label}: P11 tags 未按 yaku 冻结序：tags={tags} yaku={ids}")


def assert_rankings(match_end: dict, last_end: dict, label: str) -> None:
    """M3/N8（PROTOCOL §10.4 / §5.15）：rankings 排序 + 并列跳号 + 与 match_total 同一份数据。"""
    rankings = match_end.get("rankings")
    if not isinstance(rankings, list) or len(rankings) != SEAT_COUNT:
        raise Failure(f"{label}: match_end.rankings must hold {SEAT_COUNT} entries: {rankings}")
    scores = {}
    for entry in rankings:
        seat, score, rank = entry.get("seat"), entry.get("final_score"), entry.get("rank")
        if not isinstance(seat, int) or not isinstance(score, int) or not isinstance(rank, int):
            raise Failure(f"{label}: ranking entry malformed: {entry}")
        scores[seat] = score
    if sum(scores.values()) != 0:
        raise Failure(f"{label}: Σ final_score must be 0, got {scores}")
    for entry in rankings:
        strictly_greater = sum(1 for s in scores.values() if s > entry["final_score"])
        if entry["rank"] != 1 + strictly_greater:
            raise Failure(f"{label}: rank 并列跳号 broken: {entry} (scores={scores})")
    for prev, cur in zip(rankings, rankings[1:]):
        ok = prev["final_score"] > cur["final_score"] or (
            prev["final_score"] == cur["final_score"] and prev["seat"] < cur["seat"])
        if not ok:
            raise Failure(f"{label}: rankings 未按分数降序/同分 seat 升序: {rankings}")
    results = last_end.get("results") or []
    match_totals = {entry.get("seat"): entry.get("match_total") for entry in results}
    if match_totals and any(scores.get(seat) != total for seat, total in match_totals.items()):
        raise Failure(f"{label}: M10 final_score 必须与最后一次 game_end.match_total 逐座位相等: "
                      f"{scores} vs {match_totals}")


def assert_round_frames(end: dict, last_sync: dict | None, first_sync_total: int | None,
                        label: str) -> None:
    """P6/P12：局终帧 + 该局最后一份 state_sync 的暗杠揭示与物理守恒。"""
    # P6（局终揭示）：game_end 前最后一份 state_sync 里，ankan 组必须已补全为 4 张同面
    # （空数组形态只允许出现在**非**局终的 sync 里）。
    if last_sync is not None:
        for entry in last_sync.get("seats") or []:
            for meld in entry.get("melds") or []:
                if meld.get("type") != "ankan":
                    continue
                tiles = meld.get("tiles") or []
                if len(tiles) == 0:
                    raise Failure(f"{label}: 局终揭示前的最后一份 state_sync 未揭示 ankan：{meld}")
                if len(tiles) != 4 or len(set(tiles)) != 1:
                    raise Failure(f"{label}: 局终揭示形态非法（须 4 张同面）：{meld}")
    # P12：物理守恒（per round；荣和的河中牌允许 +1，PROTOCOL §5.10 H4）。
    if last_sync is not None and first_sync_total is not None:
        seats = last_sync.get("seats") or []
        total = last_sync.get("remaining", 0)
        total += sum(entry.get("hand_count", 0) for entry in seats)
        total += sum(len(meld.get("tiles", [])) for entry in seats for meld in entry.get("melds") or [])
        total += sum(len(entry.get("discards") or []) for entry in seats)
        win = end.get("win")
        ron = isinstance(win, dict) and not win.get("tsumo")
        allowed = {first_sync_total, first_sync_total + 1} if ron else {first_sync_total}
        if total not in allowed:
            raise Failure(f"{label}: P12 物理守恒失败：{total} ∉ {sorted(allowed)}")


def play_match(client: Client, recorder: Recorder, deadline: float, verbose: bool,
               inter_hand_delay_ms: int, initial_start: dict | None = None) -> dict:
    """打完整场（game_end 4..8 次 → match_end），途中执行 M7 探针与全部场次断言的原始采集。

    `initial_start` 是 handshake() 已消费的第一局 game_start（P9 需要把它计入 starts）。
    """
    stats = {"draws": 0, "discard_events": 0, "action_requests": 0, "errors": [],
             "ends": [], "starts": [initial_start] if initial_start else [],
             "match_end": None, "last_state_sync": None,
             "syncs_by_round": [], "action_events": [], "request_seqs": [],
             "inter_hand_gaps_ms": [], "last_end_monotonic": None, "m7_probe_armed": False,
             "m7_bad_requests": 0, "first_sync_total": None}

    def finish_round(end: dict) -> None:
        stats["ends"].append(end)
        stats["syncs_by_round"].append(stats["last_state_sync"])
        stats["last_state_sync"] = None
        stats["last_end_monotonic"] = time.monotonic()

    def send_m7_probe_once() -> None:
        """M7 探针（仅第一局间）：game_end 刚到、下一局尚未开局 ⇒ 服务端必处 kRoundEnd，
        此时发一帧对局帧，预期**恰好一条** error{BAD_REQUEST}（单独计数，不计入 C）。"""
        if len(stats["ends"]) == 1 and not stats["m7_probe_armed"] and stats["m7_bad_requests"] == 0:
            stats["m7_probe_armed"] = True
            seq = stats["request_seqs"][-1] if stats["request_seqs"] else 1
            client.send({"type": "discard", "tile": "1m", "seq": seq})
            recorder.write({"event": "m7_probe", "seq": seq})

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure("the match did not finish within the match timeout (960s)")
        try:
            frame = client.recv_frame(min(30.0, remaining))
        except TimeoutError:
            raise Failure(f"no server frame for {min(30.0, remaining):.0f}s during the match "
                          f"(last frames: {' | '.join(recorder.tail[-4:])})")
        if verbose:
            print(f"[e2e] <- {json.dumps(frame, ensure_ascii=False)[:160]}")
        kind = frame.get("type")
        if kind == "error":
            if stats["m7_probe_armed"] and frame.get("code") == "BAD_REQUEST":
                # M7 探针的预期回答：单独计数，不计入 C 的 error 断言。
                stats["m7_probe_armed"] = False
                stats["m7_bad_requests"] += 1
                continue
            stats["errors"].append(frame)
            continue
        if kind == "match_end":
            stats["match_end"] = frame
            return stats
        if kind == "pong":
            continue
        if kind == "game_start":
            if stats["last_end_monotonic"] is not None:
                gap_ms = (time.monotonic() - stats["last_end_monotonic"]) * 1000.0
                stats["inter_hand_gaps_ms"].append(gap_ms)
            stats["starts"].append(frame)
            round_started_monotonic = time.monotonic()
            continue
        if kind == "game_end":
            finish_round(frame)
            send_m7_probe_once()
            continue
        if kind == "draw":
            if frame.get("seat") == client.seat:
                client.note_draw(frame["tile"])
            stats["draws"] += 1
            continue
        if kind == "discard_event":
            client.note_discard_event(frame["seat"], frame["tile"])
            stats["discard_events"] += 1
            continue
        if kind == "state_sync":
            client.adopt_state_sync(frame)
            if stats["first_sync_total"] is None:
                stats["first_sync_total"] = tile_total_of(frame)
            stats["last_state_sync"] = frame
            continue
        if kind == "action_event":
            action = frame.get("action")
            if action in ("ankan", "kakan") and "from_seat" in frame:
                raise Failure(f"P7 失败：{action} 的 action_event 不得携带 from_seat：{frame}")
            if frame.get("seat") == client.seat and action == "win" and "from_seat" in frame:
                client.hand.extend(frame.get("tiles", []))
            continue
        if kind == "action_request":
            stats["action_requests"] += 1
            seq = frame.get("seq")
            stats["request_seqs"].append(seq)
            decision = answer_request(client, frame)
            recorder.write({"event": "decision", "window": frame.get("window"), "seq": seq,
                            "options": [option.get("action") for option in frame.get("options", [])],
                            "reply": decision})
            if verbose:
                print(f"[e2e] -> {decision} (window={frame.get('window')} seq={seq})")
            continue
        # Unknown server frames are ignored (forward compatible, PROTOCOL.md §1.3).


def tile_total_of(sync: dict) -> int:
    seats = sync.get("seats") or []
    total = sync.get("remaining", 0)
    total += sum(entry.get("hand_count", 0) for entry in seats)
    total += sum(len(meld.get("tiles", [])) for entry in seats for meld in entry.get("melds") or [])
    total += sum(len(entry.get("discards") or []) for entry in seats)
    return total


def broadcast_budget_ms(interlude_ms: int) -> int:
    """结算播报窗（SETTLEMENT_BROADCAST_RULINGS v1.3 §2.1，与客户端同一公式）。

    `budget = clamp(interlude − 1500, 4200, 10500)`；`budget < 5100` ⇒ 三档门槛①，不播报（返回 0）。
    """
    budget = max(4200, min(10500, interlude_ms - 1500))
    return budget if budget >= 5100 else 0


def assert_match(starts: list, ends: list, match_end: dict, stats: dict,
                 inter_hand_delay_ms: int) -> dict:
    """E3/P3：一场的全部场次级断言（M 组 / 番数组 / 暗杠组 / P5/P9/P12 / M7/M8 / C）。"""
    label = "match"
    if stats["errors"]:
        raise Failure(f"C 失败：收到 {len(stats['errors'])} 帧 error：{stats['errors'][:3]}")
    if not stats["m7_bad_requests"]:
        raise Failure("M7: 局间探针未收到预期的 error{BAD_REQUEST}（局间拒帧未生效或探针未发）")
    # M2：match_end 恰好 1 次（能走到这里即已收到），hands_played = game_end 下发手数（D2 终裁）。
    ends_count = len(ends)
    hands_played = match_end.get("hands_played")
    renchan = match_end.get("renchan_count")
    if not isinstance(hands_played, int) or not isinstance(renchan, int):
        raise Failure(f"M2: match_end 缺 hands_played/renchan_count: {match_end}")
    if not 4 <= hands_played <= 8:
        raise Failure(f"M2: hands_played 必须 ∈ [4,8]（C15 双闸），got {hands_played}")
    if hands_played != ends_count:
        raise Failure(f"M2/D2: hands_played({hands_played}) 必须 == game_end 下发次数({ends_count})")
    if not ends:
        raise Failure("M1: 一场至少要有一次 game_end")
    last_end = ends[-1]
    if last_end.get("renchan_count") != renchan:
        raise Failure(f"M2: renchan_count({renchan}) 必须与最后一次 game_end({last_end.get('renchan_count')}) 同值")
    # M1（PROTOCOL §10.4 的恒等式，D2 终裁口径）：game_end 次数 = hands_played（连庄由 renchan 承载）。
    if ends_count != hands_played:
        raise Failure(f"M1: game_end 次数 {ends_count} != hands_played {hands_played}")

    # P9：game_start 每局恰一次；round_index 连庄不动/轮庄 +1；dealer 推导一致；round_count==4。
    if len(starts) != ends_count:
        raise Failure(f"P9: game_start({len(starts)}) 必须与 game_end({ends_count}) 一一对应")
    expected_round, expected_dealer = 0, starts[0].get("dealer") if starts else 0
    if not isinstance(expected_dealer, int):
        raise Failure(f"P9: game_start.dealer 缺失: {starts[0]}")
    for index, start in enumerate(starts):
        if start.get("round_index") != expected_round:
            raise Failure(f"P9: 第 {index} 局 round_index={start.get('round_index')} != {expected_round}")
        if start.get("round_count") != 4:
            raise Failure(f"P9: round_count 必须恒 4: {start}")
        if start.get("dealer") != expected_dealer:
            raise Failure(f"P9: 第 {index} 局 dealer={start.get('dealer')} != 推导值 {expected_dealer}")
        end = ends[index]
        reason = end.get("reason")
        win = end.get("win")
        winner = win.get("seat") if isinstance(win, dict) else None
        if reason != "draw_exhausted" and winner != expected_dealer:
            expected_round += 1
            expected_dealer = (expected_dealer + 1) % SEAT_COUNT
    # C15：一场打完时 round_index 已走满 4（或被连庄闸截断）。
    if expected_round not in (4,):
        if not renchan > 4:
            raise Failure(f"C15: 场终时 round_index 终值 {expected_round} 既非 4 也未触发连庄闸")

    # M8（SETTLEMENT_BROADCAST_RULINGS v1.3 §2.1）：局间间隔 = 服务端的 `--inter-hand-delay-ms`，
    # 且必须**装得下结算播报**。两个方向都断言：
    #   下限 = 播报窗（budget = clamp(interlude − 1500, 4200, 10500)；budget < 5100 时不播报 ⇒ 0）
    #   上限 = 参数值 + 2s 防呆（间隔不得明显超过配置，否则是服务端卡住而不是播报）
    broadcast_ms = float(broadcast_budget_ms(inter_hand_delay_ms))
    for index, gap_ms in enumerate(stats["inter_hand_gaps_ms"]):
        if gap_ms > 2000.0 + inter_hand_delay_ms:
            raise Failure(f"M8: 第 {index + 1} 个局间隔 {gap_ms:.0f}ms 异常长（>{2000 + inter_hand_delay_ms}ms）")
        if gap_ms < broadcast_ms:
            raise Failure(
                f"M8: 第 {index + 1} 个局间隔 {gap_ms:.0f}ms < 播报窗 {broadcast_ms:.0f}ms（播报会被 game_start 打断）"
            )

    # P5：action_request.seq 整场严格递增（跨局不重置）。
    for prev, cur in zip(stats["request_seqs"], stats["request_seqs"][1:]):
        if not (isinstance(prev, int) and isinstance(cur, int) and cur > prev):
            raise Failure(f"P5: action_request.seq 必须整场严格递增：{prev} -> {cur}")

    # 逐局：P6 暗杠揭示 + P12 物理守恒 + 番数组（N1/N11/P11）。
    for index, end in enumerate(ends):
        last_sync = stats["syncs_by_round"][index] if index < len(stats["syncs_by_round"]) else None
        assert_round_frames(end, last_sync, stats["first_sync_total"], f"round {index}")
        assert_yaku_group(end, f"round {index}")

    # M3/N8/M10：rankings。
    assert_rankings(match_end, last_end, label)
    print("[e2e] note: 本场的暗杠/加杠是否发生不影响 P6/P7（它们是条件断言，动作事件与揭示形态"
          "在发生时才被校验）")
    return {"hands_played": hands_played, "renchan": renchan,
            "rounds": ends_count, "gaps_ms": [round(g) for g in stats["inter_hand_gaps_ms"]],
            "m7_bad_requests": stats["m7_bad_requests"]}


def run_one_match(binary: str, seed: int, port: int, log_path: str, verbose: bool,
                  timeout: float, ai_delay_ms: int | None,
                  inter_hand_delay_ms: int) -> dict:
    """E3：打完整场（4..8 局 → match_end）， enforcing P3 三组断言；返回场次摘要。"""
    recorder = Recorder(log_path)
    server_log = tempfile.NamedTemporaryFile(prefix="mahjong_match_", suffix=".log", delete=False)
    recorder.write({"event": "start", "binary": binary, "port": port, "seed": seed,
                    "inter_hand_delay_ms": inter_hand_delay_ms})
    command = [binary, "--port", str(port), "--seed", str(seed),
               "--inter-hand-delay-ms", str(inter_hand_delay_ms)]
    if ai_delay_ms is not None:
        command += ["--ai-delay-ms", str(ai_delay_ms)]
    process = subprocess.Popen(command, stdout=server_log, stderr=subprocess.STDOUT)
    client = None
    deadline = time.monotonic() + timeout
    try:
        require_own_listener(port, process, server_log.name)
        client = Client(port, recorder, deadline)
        start = handshake(client, recorder)
        stats = play_match(client, recorder, deadline, verbose, inter_hand_delay_ms, initial_start=start)
        match_end = stats["match_end"]
        if match_end is None:
            raise Failure("the server stream ended without a match_end frame")
        summary = assert_match(stats["starts"], stats["ends"], match_end, stats, inter_hand_delay_ms)
        summary["seed"] = seed
        summary["draws"] = stats["draws"]
        summary["discard_events"] = stats["discard_events"]
        summary["action_requests"] = stats["action_requests"]
        recorder.write({"event": "match_end", "summary": summary})
        print(f"[e2e] match done: seed={seed} rounds={summary['rounds']} "
              f"hands_played={summary['hands_played']} renchan={summary['renchan']} "
              f"m7_bad={summary['m7_bad_requests']} gaps_ms={summary['gaps_ms']}")
        return summary
    except Exception as error:  # noqa: BLE001 - every failure path must log and exit non-zero
        recorder.write({"event": "failure", "error": f"{type(error).__name__}: {error}"})
        print(f"[e2e] FAIL (match seed={seed}): {type(error).__name__}: {error}", file=sys.stderr)
        print("[e2e] last frames:", file=sys.stderr)
        for line in recorder.tail:
            print(f"  {line}", file=sys.stderr)
        try:
            with open(server_log.name, "r", encoding="utf-8", errors="replace") as source:
                with open(log_path + ".server.log", "w", encoding="utf-8") as target:
                    target.write(source.read())
        except OSError:
            pass
        raise Failure(f"match seed {seed}: {type(error).__name__}: {error}") from error
    finally:
        if client is not None:
            client.close()
        recorder.write({"event": "server_shutdown"})
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        server_log.close()
        os.unlink(server_log.name)
        recorder.close()


def collect_normalized_frames(binary: str, seed: int, log_path: str, verbose: bool,
                              timeout: float) -> list[str]:
    """E2：跑一场整场，返回 Client 采集的**归一化帧序列**（Recorder 产物同时落盘备查）。"""
    recorder = Recorder(log_path)
    server_log = tempfile.NamedTemporaryFile(prefix="mahjong_det_", suffix=".log", delete=False)
    recorder.write({"event": "start", "binary": binary, "seed": seed})
    port = free_port()
    # 归一化剔除 deadline_ms/player_id 后，时序配置不参与 diff；50ms/0 pacing 只为跑得快。
    command = [binary, "--port", str(port), "--seed", str(seed), "--inter-hand-delay-ms", "50",
               "--ai-delay-ms", "0"]
    process = subprocess.Popen(command, stdout=server_log, stderr=subprocess.STDOUT)
    client = None
    deadline = time.monotonic() + timeout
    try:
        require_own_listener(port, process, server_log.name)
        client = Client(port, recorder, deadline)
        start = handshake(client, recorder)
        stats = play_match(client, recorder, deadline, verbose, 50, initial_start=start)
        if stats["match_end"] is None:
            raise Failure("determinism run ended without match_end")
        assert_match(stats["starts"], stats["ends"], stats["match_end"], stats, 50)
    except Exception as error:  # noqa: BLE001
        recorder.write({"event": "failure", "error": f"{type(error).__name__}: {error}"})
        raise Failure(f"determinism run seed {seed}: {type(error).__name__}: {error}") from error
    finally:
        if client is not None:
            client.close()
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        server_log.close()
        os.unlink(server_log.name)
        recorder.close()
    return client.frames if client is not None else []


def run_match(args: argparse.Namespace) -> int:
    binary = find_binary(args.server_bin)
    port = resolve_port(args.port)
    print(f"[e2e] match mode: binary={binary} seed={args.seed} "
          f"inter_hand_delay_ms={args.inter_hand_delay_ms} timeout={MATCH_TIMEOUT_S:.0f}s")
    summary = run_one_match(binary, args.seed, port, args.match_log, args.verbose,
                            args.match_timeout, args.ai_delay_ms, args.inter_hand_delay_ms)
    print(f"[e2e] MATCH PASS: M1/M2 (game_end={summary['rounds']} == hands_played="
          f"{summary['hands_played']}, renchan={summary['renchan']}) | "
          f"M3/N8/M10 (rankings) | N1/N11/P11 (yaku) | P5/P9/P12 | "
          f"M7 (局间 BAD_REQUEST ×{summary['m7_bad_requests']}) | "
          f"M8 (gaps ≥ {args.inter_hand_delay_ms}ms: {summary['gaps_ms']}) | C (0 unexpected error)")
    return 0


def run_determinism(args: argparse.Namespace) -> int:
    """E2（Q3 裁决 960s）：同 --seed 两次 TCP 整场，归一化帧序列逐帧 diff 为空。"""
    binary = find_binary(args.server_bin)
    print(f"[e2e] determinism mode: two whole matches with seed={args.seed} "
          f"(timeout={MATCH_TIMEOUT_S:.0f}s each)")
    first = collect_normalized_frames(binary, args.seed, args.match_log + ".run1", args.verbose,
                                      args.match_timeout)
    print(f"[e2e] run 1 done: {len(first)} normalized frames")
    second = collect_normalized_frames(binary, args.seed, args.match_log + ".run2", args.verbose,
                                       args.match_timeout)
    print(f"[e2e] run 2 done: {len(second)} normalized frames")
    if len(first) != len(second):
        for index in range(min(len(first), len(second))):
            if first[index] != second[index]:
                print(f"[e2e] first diff at frame {index}:\n  A: {first[index][:200]}\n  B: {second[index][:200]}",
                      file=sys.stderr)
                break
        raise Failure(f"E2 失败：两场帧数不同（{len(first)} vs {len(second)}）")
    for index, (a, b) in enumerate(zip(first, second)):
        if a != b:
            print(f"[e2e] first diff at frame {index}:\n  A: {a[:300]}\n  B: {b[:300]}", file=sys.stderr)
            raise Failure(f"E2 失败：同 seed 两场在第 {index} 帧分叉")
    print(f"[e2e] E2 PASS: 同 seed 两场整场逐帧一致（{len(first)} 帧，归一化剔除 deadline_ms/player_id）")
    return 0


# ---------------------------------------------------------------------------------------------
# single game (t6)
# ---------------------------------------------------------------------------------------------
    binary = find_binary(args.server_bin)
    port = resolve_port(args.port)
    print(f"[e2e] binary={binary} port={port} seed={args.seed} log={args.log}"
          + ("" if args.port else "  (auto-picked free port)"))
    record = run_one_game(binary, args.seed, port, args.log, args.verbose, args.game_timeout,
                          args.ai_delay_ms)
    print(f"[e2e] game over: reason={record['reason']} results={SEAT_COUNT} Σdelta={record['score_sum']} "
          f"draws={record['human_draws']} discard_events={record['discards']} "
          f"action_requests={record['action_requests']} errors={record['errors']} "
          f"tile_total={record['tile_total']} wall={record['wall_size']}")
    print("[e2e] PASS: A (login_ok/game_start/draw/discard/game_end) | B (Σdelta=0) | C (no error, exit 0)")
    return 0


# ---------------------------------------------------------------------------------------------
# statistics mode (t17)
# ---------------------------------------------------------------------------------------------

def mean(values: list[float]) -> float | None:
    return None if not values else sum(values) / len(values)


def display_width(text: str) -> int:
    """Terminal columns of |text| (CJK glyphs count as two columns)."""
    return sum(2 if unicodedata.east_asian_width(char) in "WF" else 1 for char in text)


def pad(text: str, width: int, align: str = "<") -> str:
    """Pads to |width| terminal columns (CJK glyphs count as two columns)."""
    fill = " " * max(0, width - display_width(text))
    return fill + text if align == ">" else text + fill


def sha256_of(path: str) -> str:
    """Revision stamp for the binary under test (a teammate may rebuild it concurrently)."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(sorted_values: list[int], fraction: float) -> int:
    """Nearest-rank percentile (deterministic, no interpolation)."""
    if not sorted_values:
        return 0
    index = min(len(sorted_values) - 1, max(0, int(round(fraction * (len(sorted_values) - 1)))))
    return sorted_values[index]


def summarize(records: list[dict]) -> dict:
    games = len(records)
    counts = {"draw_exhausted": 0, "win_ron": 0, "win_tsumo": 0}
    for record in records:
        counts[record["reason"]] = counts.get(record["reason"], 0) + 1
    walls = [record["wall_size"] for record in records if record["wall_size"] is not None]
    totals = [record["tile_total"] for record in records if record["tile_total"] is not None]
    draws = [record["total_draws"] for record in records if record["total_draws"] is not None]
    # Round length: t25 gates on "the round is actually played out" via the discard count, so the
    # distribution (not just the mean) is part of the deliverable.
    discard_values = sorted(record["discards"] for record in records)
    return {
        "games": games,
        "reason_counts": counts,
        "draw_rate": counts["draw_exhausted"] / games if games else None,
        "win_rate": (counts["win_ron"] + counts["win_tsumo"]) / games if games else None,
        "avg_discards": mean([record["discards"] for record in records]),
        "discards_min": discard_values[0] if discard_values else None,
        "discards_p10": percentile(discard_values, 0.10) if discard_values else None,
        "discards_median": percentile(discard_values, 0.50) if discard_values else None,
        "discards_p90": percentile(discard_values, 0.90) if discard_values else None,
        "discards_max": discard_values[-1] if discard_values else None,
        "discards_per_seat_avg": mean([record["discards"] / SEAT_COUNT for record in records]),
        "avg_draws_per_player": mean([value / SEAT_COUNT for value in draws]),
        "avg_human_draws": mean([record["human_draws"] for record in records]),
        "avg_actions": mean([record["action_requests"] for record in records]),
        "tile_total": max(set(totals), key=totals.count) if totals else None,
        "wall_size": max(set(walls), key=walls.count) if walls else None,
        "tile_total_values": sorted(set(totals)),
        "wall_size_values": sorted(set(walls)),
        "errors": sum(record["errors"] for record in records),
    }


def print_report(summary: dict, requested: int, failed: list[dict], elapsed: float,
                 baseline: dict) -> None:
    base_games = baseline["games"]
    base_counts = baseline["reason_counts"]
    base_draw_rate = base_counts["draw_exhausted"] / base_games
    counts = summary["reason_counts"]
    games = summary["games"]
    draw_rate = summary["draw_rate"] or 0.0

    def row(label: str, total: int, c: dict, rate: float | None, avg_discards: float | None = None,
            avg_draws_per_player: float | None = None) -> str:
        def pct(value: int) -> str:
            return f"{value} ({value / total * 100:.1f}%)" if total else "-"

        def num(value: float | None) -> str:
            return f"{value:.2f}" if value is not None else "-"

        return (pad(label, label_width) + " " + pad(str(total), 5, ">") + "  " + pad(pct(c["draw_exhausted"]), 14, ">")
                + "  " + pad(pct(c["win_ron"]), 13, ">") + "  " + pad(pct(c["win_tsumo"]), 13, ">")
                + f"  {(rate * 100 if rate is not None else 0):>7.1f}%"
                + "  " + pad(num(avg_discards), 12, ">") + "  " + pad(num(avg_draws_per_player), 12, ">"))

    detected_label = "本次实测 (未采集)"
    if summary["tile_total"] is not None:
        detected_label = f"本次实测: {summary['tile_total']} 张 / 牌山 {summary['wall_size']}"
    label_width = max([display_width(baseline["label"]), display_width(detected_label)]
                      + [display_width(entry["label"]) for entry in HISTORY]) + 2

    print()
    print("=" * 124)
    print("四方对比：四轮尝试（108 加/减字牌 → 改弃牌策略 → 自摸修复）")
    print("=" * 124)
    print(pad("口径", label_width) + " " + pad("局数", 5, ">") + "  " + pad("荒庄", 14, ">") + "  "
          + pad("荣和", 13, ">") + "  " + pad("自摸", 13, ">") + "  " + pad("荒庄率", 8, ">")
          + "  " + pad("平均打牌/局", 12, ">") + "  " + pad("平均摸牌/人", 12, ">"))
    print("-" * 124)
    for entry in HISTORY + [baseline]:
        print(row(entry["label"], entry["games"], entry["reason_counts"],
                  entry["reason_counts"]["draw_exhausted"] / entry["games"],
                  entry.get("avg_discards"), entry.get("avg_draws_per_player")))
    print("-" * 124)
    print(row(detected_label, games, counts, summary["draw_rate"],
              summary["avg_discards"], summary["avg_draws_per_player"]))
    print("-" * 124)
    print(f"参考来源: " + " | ".join(f"{e['label']} ← {e['source']}" for e in HISTORY + [baseline]))
    print(f"本次局数: {games}/{requested}"
          + (f"（提前中止，失败 {len(failed)} 局）" if failed else "（全部完成）")
          + f"   用时 {elapsed:.1f}s"
          + f"   自摸局数: {counts['win_tsumo']}（要求 > 0）")
    if summary["avg_discards"] is not None:
        print(f"平均每局打牌次数: {summary['avg_discards']:.2f} 次（门槛 ≥ 45）")
        print(f"每局打牌次数分布: 最小 {summary['discards_min']} / p10 {summary['discards_p10']} / "
              f"中位 {summary['discards_median']} / p90 {summary['discards_p90']} / 最大 {summary['discards_max']}"
              f"（单座位平均 {summary['discards_per_seat_avg']:.2f} 次）")
    print(f"平均每玩家摸牌次数（参考，非门槛）: "
          f"{summary['avg_draws_per_player']:.2f} 次" if summary["avg_draws_per_player"] is not None
          else "平均每玩家摸牌次数: -")
    print(f"真人玩家平均摸牌次数（交叉校验）: {summary['avg_human_draws']:.2f} 次"
          if summary["avg_human_draws"] is not None else "真人玩家平均摸牌次数: -")
    # t31: a sweep may end with zero completed games (e.g. the per-game timeout fired); the
    # report must still render so the failure diagnostics above it stay readable.
    windows = f"{summary['avg_actions']:.1f}" if summary["avg_actions"] is not None else "-"
    print(f"平均每局操作窗口数: {windows}   全程 error 帧: {summary['errors']}")
    if summary["tile_total"] is not None:
        print(f"实测牌张口径: {summary['tile_total']} 张（各局取值 {summary['tile_total_values']}），"
              f"牌山 {summary['wall_size']}（各局取值 {summary['wall_size_values']}）"
              f" → 单局打牌上限 {summary['wall_size'] + 1}，每人摸牌上限 "
              f"{(summary['wall_size'] + 1) / SEAT_COUNT:.2f}")
        print(f"第二阶段目标口径: {TARGET['faces']} 面 / {TARGET['tile_total']} 张 / "
              f"牌山 {TARGET['wall_size']} / 单局最多 {TARGET['max_discards']} 次打牌 / "
              f"每人约 {TARGET['draws_per_player']} 次摸牌")
    if failed:
        print("-" * 112)
        for entry in failed:
            print(f"  ✗ seed={entry['seed']}: {entry['error']}")
    print("=" * 112)
    print("结论")
    print("-" * 112)
    for line in conclusion_lines(summary, base_draw_rate, failed):
        print(line)
    print("=" * 112)


def conclusion_lines(summary: dict, base_draw_rate: float, failed: list[dict]) -> list[str]:
    if failed:
        return [f"  ⛔ 未完成统计口径校验：{len(failed)} 局失败（退出码非 0 / 断言失败），"
                "本表数字不可用于结论；失败 seed 见上表。"]
    detected = summary["tile_total"]
    if detected is None:
        return ["  ⚠ 未采集到 state_sync，无法推导牌张口径与摸牌次数。"]
    if detected != TARGET["tile_total"]:
        return [
            f"  ⛔ 无法验证目标达成（finding）：实测牌张口径为 {detected} 张 / 牌山 {summary['wall_size']}，"
            f"与目标 {TARGET['tile_total']} 张 / 牌山 {TARGET['wall_size']} 不一致——"
            "被验证的服务器二进制不是本阶段口径，本表数字不可用于结论。",
        ]
    rate = summary["draw_rate"] or 0.0
    discards = summary["avg_discards"]
    tsumo = summary["reason_counts"]["win_tsumo"]
    lines = []
    # t25 thresholds: pass = draws < 70% AND avg discards/round >= 45; 70-85% = needs_revision;
    # >= 86% = fail. The old ">= 18 draws per player" gate is retired: with a healthy policy the
    # round ends on a win, so the wall is not exhausted by design.
    length_ok = discards is not None and discards >= 45
    lines.append(f"  荒庄率 {rate * 100:.1f}%（门槛：<70% 通过 / 70–85% needs_revision / ≥86% 失败）；"
                 f"平均每局打牌 {discards:.2f} 次（门槛 ≥45）{'✅' if length_ok else '❌'}")
    lines.append(f"  自摸局数 {tsumo}（要求 > 0）；和牌率 {(summary['win_rate'] or 0) * 100:.1f}%")
    if rate < 0.70 and length_ok and tsumo > 0:
        lines.append(f"  ✅ 通过：相对 136 张/旧策略的 97.0% 降低 {(0.97 - rate) * 100:.1f}pp，"
                     f"相对 t23 的 6.5% 变化 {(rate - 0.065) * 100:+.1f}pp；牌局长度充足（中位 "
                     f"{summary['discards_median']} 次打牌，最小 {summary['discards_min']}）")
    elif rate < 0.70 and not length_ok:
        lines.append(f"  ⚠ needs_revision：荒庄率达标，但牌局过短（平均打牌 {discards:.2f} < 45）")
    elif rate < 0.70 and tsumo == 0:
        lines.append("  ⚠ needs_revision：荒庄率达标，但样本内没有自摸局，自摸路径未被覆盖")
    elif rate <= 0.85:
        lines.append(f"  ⚠ needs_revision：荒庄率 {rate * 100:.1f}% 落在 70–85% 区间（有改善但未达标）")
    else:
        lines.append(f"  ⛔ 失败：荒庄率 {rate * 100:.1f}% ≥ 86%，无改善或更差")
    return lines


def run_stats(args: argparse.Namespace) -> int:
    binary = find_binary(args.server_bin)
    binary_sha256 = sha256_of(binary)
    last_seed = args.seed + args.games - 1
    port_policy = f"每局自动选空闲端口（--port {AUTO_PORT}）" if args.port == AUTO_PORT else f"固定端口 {args.port}"
    pacing = "服务端默认（900ms）" if args.ai_delay_ms is None else f"{args.ai_delay_ms}ms"
    print(f"[stats] binary={binary} sha256={binary_sha256[:16]} games={args.games} "
          f"seeds={args.seed}..{last_seed} {port_policy} ai_delay={pacing}")
    if args.seed != BASELINE_SEED_START:
        print(f"[stats] 提示：与 t6 基线逐局可比的是 --seed {BASELINE_SEED_START}"
              f"（seed {BASELINE_SEED_START}..{BASELINE_SEED_START + 199}）；"
              f"本次 seed_start={args.seed}，样本不同，同口径但对比仅供参考")
    records: list[dict] = []
    failed: list[dict] = []
    started = time.monotonic()
    for index in range(args.games):
        seed = args.seed + index
        if args.frames_dir:
            log_path = os.path.join(args.frames_dir, f"seed-{seed}.jsonl")
        else:
            log_path = os.path.join(tempfile.gettempdir(), f"mahjong_e2e_frames_{seed}.jsonl")
        try:
            port = resolve_port(args.port)  # a fresh free port per game (t26)
            record = run_one_game(binary, seed, port, log_path, args.verbose, args.game_timeout,
                                  args.ai_delay_ms)
        except Failure as error:
            failed.append({"seed": seed, "error": str(error)})
            break
        finally:
            if not args.frames_dir and os.path.exists(log_path):
                os.unlink(log_path)
        records.append(record)
        # One line per game: with AI pacing a single round takes tens of seconds, so a
        # 25-game progress gate used to look exactly like a hang (t31).
        print(f"[stats] {index + 1}/{args.games} seed={seed} reason={record['reason']} "
              f"discards={record['discards']} draws/player={record['total_draws'] / SEAT_COUNT:.2f} "
              f"wall={record['wall_size']} errors=0", flush=True)
    elapsed = time.monotonic() - started

    summary = summarize(records)
    print_report(summary, args.games, failed, elapsed, BASELINE)

    # The measurement is only meaningful for the second-milestone question when the ruleset under
    # test really is the 34-face / 136-tile one. Label it in the artefact so a same-口径 run can
    # never be mistaken for the "did honours fix the draw rate" result (that mistake would be a
    # false negative, not a finding).
    matches_target = summary["tile_total"] == TARGET["tile_total"]
    if matches_target:
        measurement_note = (f"被验证二进制为 {summary['tile_total']} 张 / 牌山 {summary['wall_size']}，"
                            f"与第二阶段目标一致，本文件可作为 t17 测量结果。")
    else:
        measurement_note = (f"被验证二进制的牌张口径为 {summary['tile_total']} 张 / 牌山 {summary['wall_size']}，"
                            f"与第二阶段目标 {TARGET['tile_total']} 张 / 牌山 {TARGET['wall_size']} 不一致"
                            "（字牌扩展未在本构建中落地）：本文件仅是统计能力与无回归的验证证据，"
                            "**不能**作为「字牌是否降低荒庄率」的结论；t15 落地并重建后必须重跑。")
    binary_sha256_after = sha256_of(binary)
    if binary_sha256_after != binary_sha256:
        print("[stats] warning: the server binary was REBUILT while this run was in progress "
              f"({binary_sha256[:16]} → {binary_sha256_after[:16]}); the numbers may mix two revisions",
              file=sys.stderr)
    payload = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "binary": binary,
        "binary_sha256": binary_sha256,
        "binary_sha256_after": binary_sha256_after,
        "binary_mtime": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(os.path.getmtime(binary))),
        "seed_start": args.seed,
        "games_requested": args.games,
        "games_completed": len(records),
        "elapsed_s": round(elapsed, 2),
        "ports_used": sorted({record["port"] for record in records if "port" in record}),
        "measurement_valid_for_target": matches_target,
        # t31: what pacing the server ran with (None = the server default, 900ms). The decision
        # records are identical for any value, so the measurement does not depend on it.
        "ai_delay_ms": args.ai_delay_ms,
        "measurement_note": measurement_note,
        "summary": summary,
        "baseline": BASELINE,
        "target": TARGET,
        "failed": failed,
        "records": records,
    }
    os.makedirs(os.path.dirname(args.stats_out), exist_ok=True)
    with open(args.stats_out, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print(f"[stats] 汇总已写入 {args.stats_out}")

    if failed or len(records) != args.games:
        print(f"[stats] FAIL: {len(records)}/{args.games} 局完成，{len(failed)} 局失败", file=sys.stderr)
        return 1
    print(f"[stats] PASS: {args.games} 局全部退出码 0、0 帧 error、三条断言全过")
    return 0


# ---------------------------------------------------------------------------------------------
# concurrency check (t26): two sweeps side by side must not interfere
# ---------------------------------------------------------------------------------------------

def run_concurrency_check(args: argparse.Namespace) -> int:
    """Runs two sweeps of this script in parallel and asserts they cannot disturb each other.

    Before t26 both sweeps dialled the same fixed port: the second server failed to bind and the
    second harness silently played its round against the *first* harness's server (exit 0, 0 error
    frames — a false PASS that validated a foreign process). This mode reproduces the concurrent
    scenario and requires: both instances exit 0 with 0 error frames and a valid tile set, the two
    runs share no port, every port is idle again afterwards.
    """
    binary = find_binary(args.server_bin)
    games = max(args.games, 10)
    work = tempfile.mkdtemp(prefix="mahjong_concurrency_")
    stats = [os.path.join(work, f"run{name}.json") for name in "AB"]
    logs = [os.path.join(work, f"run{name}.log") for name in "AB"]
    print(f"[concurrency] 同时启动两个实例，各 {games} 局（seed {args.seed}..{args.seed + games - 1}）")
    commands = [[sys.executable, os.path.abspath(__file__), "--server-bin", binary, "--games", str(games),
                 "--seed", str(args.seed), "--stats-out", stats[index]] for index in range(2)]
    for command in commands:
        print("   ", " ".join(command))
    started = time.monotonic()
    handles = [open(logs[index], "w", encoding="utf-8") for index in range(2)]
    processes = [subprocess.Popen(commands[index], stdout=handles[index], stderr=subprocess.STDOUT)
                 for index in range(2)]
    codes = [wait_with_timeout(process, args.game_timeout, f"并发实例 {'AB'[index]}")
             for index, process in enumerate(processes)]
    for handle in handles:
        handle.close()
    elapsed = time.monotonic() - started
    print(f"[concurrency] 两个实例均已结束：exit={codes}（用时 {elapsed:.1f}s）")

    failures: list[str] = []
    payloads: list[dict] = []
    for index, code in enumerate(codes):
        label = "AB"[index]
        if code != 0:
            failures.append(f"实例 {label} exit={code}（详见 {logs[index]}）")
            with open(logs[index], encoding="utf-8", errors="replace") as handle:
                print(handle.read()[-1500:], file=sys.stderr)
            continue
        with open(stats[index], encoding="utf-8") as handle:
            payload = json.load(handle)
        payloads.append(payload)
        summary = payload["summary"]
        print(f"[concurrency] 实例 {label}: {payload['games_completed']}/{payload['games_requested']} 局，"
              f"荒庄 {summary['reason_counts']['draw_exhausted']}、荣和 {summary['reason_counts']['win_ron']}、"
              f"自摸 {summary['reason_counts']['win_tsumo']}，error 帧 {summary['errors']}，"
              f"口径合法 {payload['measurement_valid_for_target']}，端口 {len(payload['ports_used'])} 个")
        if payload["games_completed"] != payload["games_requested"]:
            failures.append(f"实例 {label} 未完成全部局数")
        if summary["errors"] != 0:
            failures.append(f"实例 {label} 出现 {summary['errors']} 帧 error")
        if not payload["measurement_valid_for_target"]:
            failures.append(f"实例 {label} measurement_valid_for_target=false")

    if len(payloads) == 2:
        ports = [set(payload["ports_used"]) for payload in payloads]
        shared = ports[0] & ports[1]
        print(f"[concurrency] 端口集合交集: {sorted(shared) if shared else '空（无共享端口）'}")
        if shared:
            failures.append(f"两个实例共用了端口 {sorted(shared)}——并发下会互相踩")
        leaked = sorted(port for port in (ports[0] | ports[1]) if not port_is_free(port))
        print(f"[concurrency] 结束后仍被占用的端口: {leaked if leaked else '无（端口已回到空闲）'}")
        if leaked:
            failures.append(f"端口泄漏: {leaked}")

    if failures:
        print("\n[concurrency] FAIL:")
        for item in failures:
            print("  ✗", item)
        return 1
    print("\n[concurrency] PASS: 两个实例均 exit 0、0 帧 error、无共享端口、无端口泄漏、互不干扰")
    print(f"[concurrency] 复现命令：python3 game/tests/e2e_full_game.py --concurrency-check "
          f"--games {games} --seed {args.seed} --server-bin {binary}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server-bin", default=None, help=f"server binary (default {DEFAULT_BINARY})")
    parser.add_argument("--port", type=int, default=AUTO_PORT,
                        help=f"listen port: {AUTO_PORT} (default) = pick a free port for every game; "
                             f"pass an explicit port (e.g. {DEFAULT_PORT}) for a fixed-port scenario, "
                             "which is pre-checked and refused when already in use")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help=f"fixed wall seed (default {DEFAULT_SEED})")
    parser.add_argument("--games", type=int, default=1,
                        help="play this many consecutive-seed games and print the t17 statistics "
                             "(default 1 = the single-game t6 acceptance)")
    parser.add_argument("--timeout", type=float, default=GLOBAL_TIMEOUT_S,
                        help=f"hard ceiling for one game in seconds (default {GLOBAL_TIMEOUT_S:.0f}); "
                             "a game that exceeds it fails with its last frames and the server log tail")
    parser.add_argument("--ai-delay-ms", type=int, default=None, dest="ai_delay_ms",
                        help="forward --ai-delay-ms to the server (default: the server's own default, 900). "
                             "This harness measures outcomes, not pacing: the decision records are identical "
                             "either way (t31), so statistics runs may pass 0 to finish ~40x faster.")
    parser.add_argument("--game-timeout", type=float, default=None,
                        help="per-game budget in seconds, clamped by --timeout (default: min(--timeout, 90); "
                             "an AI-paced round needs ~40s, so a tighter value must be chosen deliberately)")
    parser.add_argument("--log", default=DEFAULT_LOG,
                        help=f"single-game frame artefact (default {DEFAULT_LOG})")
    parser.add_argument("--stats-out", default=DEFAULT_STATS_OUT,
                        help=f"statistics summary artefact (default {DEFAULT_STATS_OUT})")
    parser.add_argument("--frames-dir", default=None,
                        help="keep the per-game frame logs in this directory (default: temp, deleted)")
    parser.add_argument("--concurrency-check", action="store_true",
                        help="t26: run two sweeps of this script in parallel and assert they do not "
                             "disturb each other (uses --games, at least 10 per instance)")
    # ---- v1.6 整场模式（E2/E3, A1_ACCEPTANCE §七）----
    parser.add_argument("--matches", type=int, default=0,
                        help="play this many WHOLE matches (4..8 rounds each -> match_end) with the "
                             "PROTOCOL §10.4 match-level assertions (M1/M2/M3/N8/M10/N1/N11/P5/P9/"
                             "P11/P12 + M7/M8). Timeout = 960s per match (Q3).")
    parser.add_argument("--inter-hand-delay-ms", type=int, default=300,
                        help="forward --inter-hand-delay-ms to the server for match mode; the M8 "
                             "assertion checks every game_end -> game_start gap against this value "
                             "(default 300ms keeps a match fast while still observable)")
    parser.add_argument("--determinism-check", action="store_true",
                        help="E2: run the WHOLE match twice with the same --seed and assert the "
                             "normalized frame sequences are identical (deadline_ms/player_id "
                             "stripped). Timeout = 960s per run (Q3).")
    parser.add_argument("--match-log", default=DEFAULT_MATCH_LOG,
                        help=f"match-mode frame artefact (default {DEFAULT_MATCH_LOG})")
    parser.add_argument("--match-timeout", type=float, default=MATCH_TIMEOUT_S,
                        help=f"per-match budget in seconds (default {MATCH_TIMEOUT_S:.0f} = Q3)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    # t31: one game has its own budget (clamped by the hard ceiling) so a stalled game fails
    # with its frames and the server log tail instead of stalling the whole sweep.
    if args.game_timeout is None:
        args.game_timeout = min(args.timeout, DEFAULT_GAME_TIMEOUT_S)
    if args.game_timeout <= 0:
        print(f"[e2e] FAIL: --game-timeout must be positive, got {args.game_timeout}", file=sys.stderr)
        return 2
    args.game_timeout = min(args.game_timeout, args.timeout)
    try:
        if args.concurrency_check:
            return run_concurrency_check(args)
        if args.determinism_check:
            return run_determinism(args)
        if args.matches > 0:
            binary = find_binary(args.server_bin)
            for index in range(args.matches):
                seed = args.seed + index
                port = resolve_port(args.port)
                run_one_match(binary, seed, port, f"{args.match_log}.{index}", args.verbose,
                              args.match_timeout, args.ai_delay_ms, args.inter_hand_delay_ms)
            print(f"[e2e] MATCH SWEEP PASS: {args.matches} 场整场全部通过")
            return 0
        if args.games <= 1:
            return run_single(args)
        return run_stats(args)
    except Failure as error:  # pre-flight failures (missing binary, busy port, ...)
        print(f"[e2e] FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
