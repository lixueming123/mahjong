#!/usr/bin/env python3
"""Account and match-history end-to-end test (game/docs/ACCOUNT_DESIGN.md).

Starts the real server against a real MySQL and drives the whole feature over a socket:

  * register -> login_ok with a durable "u-<id>" player_id (register implies login, §3.2);
  * a second register of the same name -> USERNAME_TAKEN, a wrong password -> AUTH_FAILED;
  * the same account on a second connection -> ALREADY_ONLINE, and the account is free again
    once the first connection goes away (§1.3);
  * a guest login still works, and its stats_request answers a zeroed payload (§3.3);
  * a full game is played through the real room, then stats_request reports exactly that one
    game (games/wins/draws/net_score and the recent entry), i.e. the match was recorded (§4);
  * restarting the server keeps the same account's player_id and its history (§7.3).

It uses the dedicated database `mahjong_test` (created on the fly), so a run never reads or
writes the demo database. Every run registers a fresh random username, so leftover rows from
an earlier run cannot skew the assertions.

Exit code 77 (ctest: SKIP) when MySQL is unreachable or the server was built without account
storage: a visible "not run" is better than a silent pass.

Usage:
  python3 game/tests/account_e2e_test.py --server-bin build/game/mahjong_server
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import ACTION, ERROR_CODE, FRAME_TYPE, MODE, REASON, WINDOW

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BIN_CANDIDATES = (
    "build/game/mahjong_server",
    "build-release/game/mahjong_server",
    "build/game/Debug/mahjong_server",
    "build/game/Release/mahjong_server",
)
SKIP_EXIT = 77
SEAT_COUNT = 4
TEST_DB = "mahjong_test"
# The local development instance (root/12345678). Only the test defaults to it; the server
# itself ships no password (ACCOUNT_DESIGN.md §6.5).
DEFAULT_MYSQL_PASSWORD = "12345678"


class TestFailure(AssertionError):
    pass


def check(condition: bool, label: str) -> None:
    if not condition:
        raise TestFailure(label)


def skip(reason: str) -> None:
    print(f"[account-e2e] SKIP: {reason}")
    sys.exit(SKIP_EXIT)


class Client:
    """One TCP connection speaking the NDJSON protocol."""

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
                raise TestFailure(f"{self.name}: timed out waiting for a frame")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise TestFailure(f"{self.name}: timed out waiting for a frame")
            if not chunk:
                raise TestFailure(f"{self.name}: connection closed while waiting for a frame")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def recv_type(self, expected: int, timeout: float = 20.0) -> dict:
        frame = self.recv_frame(timeout)
        check(frame.get("type") == expected, f"{self.name}: expected {expected}, got {frame}")
        return frame

    def recv_error(self, code: int, timeout: float = 20.0) -> dict:
        frame = self.recv_frame(timeout)
        check(frame.get("type") == FRAME_TYPE.ERROR, f"{self.name}: expected an error, got {frame}")
        check(frame.get("code") == code, f"{self.name}: expected {code}, got {frame}")
        return frame

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_for_server(port: int, process: subprocess.Popen, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise TestFailure(f"the server exited early with code {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise TestFailure("the server did not start listening in time")


def mysql_reachable(host: str, port: int, timeout: float = 2.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def resolve_binary(explicit: str | None) -> str:
    if explicit:
        return explicit
    for candidate in DEFAULT_BIN_CANDIDATES:
        path = os.path.join(REPO_ROOT, candidate)
        if os.path.exists(path):
            return path
    raise TestFailure("could not find mahjong_server; pass --server-bin")


class Server:
    """The compiled server under test, with a log file kept for diagnostics."""

    def __init__(self, binary: str, args: argparse.Namespace, action_timeout_ms: int = 1000):
        self.binary = binary
        self.args = args
        self.port = args.port or free_port()
        self.action_timeout_ms = action_timeout_ms
        self.log = tempfile.NamedTemporaryFile(prefix="mahjong-account-", suffix=".log", delete=False)
        self.process: subprocess.Popen | None = None

    def start(self) -> None:
        command = [
            self.binary,
            "--port", str(self.port),
            "--seed", "20251005",
            "--ai-delay-ms", "0",
            "--action-timeout-ms", str(self.action_timeout_ms),
            "--mysql-host", self.args.mysql_host,
            "--mysql-port", str(self.args.mysql_port),
            "--mysql-user", self.args.mysql_user,
            "--mysql-password", self.args.mysql_password,
            "--mysql-db", self.args.mysql_db,
        ]
        self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT)
        wait_for_server(self.port, self.process)

    def stop(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.send_signal(2)  # SIGINT: the graceful path of main()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)

    def log_tail(self) -> str:
        try:
            with open(self.log.name, "r", encoding="utf-8", errors="replace") as handle:
                return handle.read()[-4000:]
        except OSError:
            return ""


def cleanup_rows(args: argparse.Namespace, usernames: list[str]) -> None:
    """Best effort: drop this run's rows so the test database does not grow forever."""
    mysql = shutil.which("mysql") or "/usr/local/mysql/bin/mysql"
    if not os.path.exists(mysql):
        return
    statement = "".join(
        "DELETE mr FROM match_results mr JOIN accounts a ON a.id = mr.account_id WHERE a.username = '{0}';"
        "DELETE m FROM matches m JOIN accounts a ON a.id = m.account_id WHERE a.username = '{0}';"
        "DELETE FROM accounts WHERE username = '{0}';".format(name)
        for name in usernames
    )
    try:
        subprocess.run(
            [mysql, "-h", args.mysql_host, "-P", str(args.mysql_port), "-u", args.mysql_user,
             f"--password={args.mysql_password}", args.mysql_db, "-e", statement],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass


def sql_query(args: argparse.Namespace, statement: str) -> str:
    """E5：真库核对用（本机 MySQL 可达时）；返回制表符分隔的查询输出（-N 去表头）。"""
    mysql = shutil.which("mysql") or "/usr/local/mysql/bin/mysql"
    result = subprocess.run(
        [mysql, "-N", "-h", args.mysql_host, "-P", str(args.mysql_port), "-u", args.mysql_user,
         f"--password={args.mysql_password}", args.mysql_db, "-e", statement],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10, check=True, text=True,
    )
    return result.stdout


def answer_request(client: Client, hand: set[str], request: dict) -> None:
    """Answers one action window with a legal move (same policy as room_full_game_test.py).

    `win` when offered, otherwise discard a tile from the tracked hand; a claim window is
    declined with `pass`, which the frozen decision table always allows (§6.3).
    """
    seq = request["seq"]
    options = request.get("options", [])
    actions = {option["action"]: option for option in options}
    if ACTION.WIN in actions:
        client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.WIN, "tiles": [], "seq": seq})
        return
    if request["window"] == WINDOW.DISCARD:
        tile = sorted(hand)[-1] if hand else None
        check(tile is not None, "the discard window arrived with an empty tracked hand")
        client.send({"type": FRAME_TYPE.DISCARD, "tile": tile, "seq": seq})
        return
    for action in (ACTION.PONG, ACTION.CHOW):
        if action in actions:
            client.send({"type": FRAME_TYPE.ACTION, "action": action, "tiles": actions[action]["tiles"], "seq": seq})
            return
    client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.PASS, "tiles": [], "seq": seq})


def play_one_match(client: Client, name: str) -> tuple[str, list[dict], dict, dict]:
    """E5（ACCOUNT_DESIGN §7.2 v1.3）：打完整场（4..8 局 → match_end）。

    返回 `(room_id, ends, match_end, mine)`；`mine` 汇总本座位（seat 0）的
    得分累计 / 和牌局数 / 荒庄局数 / 坐庄手数，供落库核对。
    """
    client.send({"type": FRAME_TYPE.CREATE_ROOM})
    joined = client.recv_type(FRAME_TYPE.ROOM_JOINED)
    check(joined["seat"] == 0, f"{name}: the creator must sit at seat 0")
    check(len(joined["seats"]) == SEAT_COUNT, f"{name}: room_joined needs 4 seats")
    check(joined["seats"][0]["name"] == name, f"{name}: seat 0 must carry the account nickname")
    room_id = joined["room_id"]

    ends: list[dict] = []
    match_end: dict | None = None
    mine = {"delta_total": 0, "wins": 0, "draws": 0, "dealer_rounds": 0}
    deadline = time.monotonic() + 600
    client.send({"type": FRAME_TYPE.READY})
    while match_end is None:
        if time.monotonic() > deadline:
            raise TestFailure(f"{name}: the match did not finish in time")
        start = client.recv_type(FRAME_TYPE.GAME_START)
        if start.get("dealer") == 0:
            mine["dealer_rounds"] += 1
        hand = set(start["hand"])
        while True:
            frame = client.recv_frame()
            kind = frame.get("type")
            if kind == FRAME_TYPE.GAME_END:
                ends.append(frame)
                mine["delta_total"] += next(
                    entry["delta_score"] for entry in frame["results"] if entry["seat"] == 0)
                if isinstance(frame.get("win"), dict) and frame["win"].get("seat") == 0:
                    mine["wins"] += 1
                if frame.get("reason") == REASON.DRAW_EXHAUSTED:
                    mine["draws"] += 1
                if not frame.get("match_will_continue", False):
                    closing = client.recv_frame()
                    check(closing.get("type") == FRAME_TYPE.MATCH_END,
                          f"{name}: expected match_end after the last game_end, got {closing}")
                    match_end = closing
                break
            if kind == FRAME_TYPE.MATCH_END:
                raise TestFailure(f"{name}: match_end arrived before the round's game_end: {frame}")
            if kind == FRAME_TYPE.DRAW:
                if frame.get("seat") == 0:
                    hand.add(frame["tile"])
            elif kind == FRAME_TYPE.DISCARD_EVENT:
                if frame.get("seat") == 0:
                    hand.discard(frame["tile"])
            elif kind == FRAME_TYPE.STATE_SYNC:
                hand = set(frame["hand"])
            elif kind == FRAME_TYPE.ACTION_REQUEST:
                answer_request(client, hand, frame)
            elif kind == FRAME_TYPE.ERROR:
                raise TestFailure(f"{name}: unexpected error frame: {frame}")
    return room_id, ends, match_end, mine


def stats_for(client: Client, label: str) -> dict:
    client.send({"type": FRAME_TYPE.STATS_REQUEST})
    return client.recv_type(FRAME_TYPE.STATS)


def run(args: argparse.Namespace) -> None:
    binary = resolve_binary(args.server_bin)
    if not mysql_reachable(args.mysql_host, args.mysql_port):
        skip(f"no MySQL at {args.mysql_host}:{args.mysql_port} (start it, or pass --mysql-port)")

    username = "e2e_{}_{}".format(os.getpid() % 100000, random.randint(1000, 9999))
    password = "pw_" + username  # 6..64 printable ASCII (§3.1)
    server = Server(binary, args)
    server.start()
    try:
        # --- register implies login (§3.2) ------------------------------------------------
        first = Client(server.port, "register")
        first.send({"type": FRAME_TYPE.REGISTER, "username": username, "password": password})
        login_ok = first.recv_frame()
        if login_ok.get("type") == FRAME_TYPE.ERROR and login_ok.get("code") == ERROR_CODE.INTERNAL:
            skip(f"the server has no account storage: {login_ok.get('message')}")
        check(login_ok.get("type") == FRAME_TYPE.LOGIN_OK, f"register must answer login_ok, got {login_ok}")
        player_id = login_ok.get("player_id", "")
        check(player_id.startswith("u-"), f"an account's player_id must be durable, got {player_id}")
        check(login_ok.get("name") == username, f"the nickname defaults to the username: {login_ok}")
        print(f"[account-e2e] registered {username} -> {player_id}")

        # --- the account name is unique (§3.2) --------------------------------------------
        second = Client(server.port, "duplicate")
        second.send({"type": FRAME_TYPE.REGISTER, "username": username, "password": password})
        second.recv_error(ERROR_CODE.USERNAME_TAKEN)

        # --- a wrong password, and the account-not-found case, look identical (§3.1) ------
        # (The unknown name must still satisfy §3.1's 3..16 rule, otherwise the server is right
        # to answer BAD_REQUEST before it ever looks the account up.)
        second.send({"type": FRAME_TYPE.LOGIN, "username": username, "password": "definitely_wrong"})
        second.recv_error(ERROR_CODE.AUTH_FAILED)
        second.send({"type": FRAME_TYPE.LOGIN, "username": "e2e_absent", "password": password})
        second.recv_error(ERROR_CODE.AUTH_FAILED)

        # --- one account, one live connection (§1.3) --------------------------------------
        second.send({"type": FRAME_TYPE.LOGIN, "username": username, "password": password})
        second.recv_error(ERROR_CODE.ALREADY_ONLINE)

        # --- a second handshake frame while the first is still in flight -------------------
        # §3.1/§3.2: the check runs before the account query finishes, so the answer is
        # BAD_REQUEST in both orderings — the pending guard when the reply has not arrived yet,
        # and "already logged in" once it has. Without that guard the second frame would reach
        # the database and come back USERNAME_TAKEN, which is not what the spec promises.
        pipelined = Client(server.port, "pipelined")
        pipelined.send({"type": FRAME_TYPE.REGISTER, "username": username + "_b", "password": password})
        pipelined.send({"type": FRAME_TYPE.REGISTER, "username": username + "_b", "password": password})
        answers = [pipelined.recv_frame(), pipelined.recv_frame()]
        kinds = sorted(frame.get("type") for frame in answers)
        check(kinds == [FRAME_TYPE.LOGIN_OK, FRAME_TYPE.ERROR], f"exactly one handshake may win: {answers}")
        rejected = next(frame for frame in answers if frame.get("type") == FRAME_TYPE.ERROR)
        check(rejected.get("code") == ERROR_CODE.BAD_REQUEST and rejected.get("message") == "already logged in",
              f"the second handshake frame must be refused as a duplicate: {rejected}")
        pipelined.close()

        # --- the account is free again once that connection goes away (§1.3) --------------
        first.close()
        third = Client(server.port, "relogin")
        third.send({"type": FRAME_TYPE.LOGIN, "username": username, "password": password})
        deadline = time.monotonic() + 10
        while True:
            frame = third.recv_frame()
            if frame.get("type") == FRAME_TYPE.LOGIN_OK:
                break
            check(frame.get("code") == ERROR_CODE.ALREADY_ONLINE, f"unexpected answer while waiting: {frame}")
            if time.monotonic() > deadline:
                raise TestFailure("the account was never released by the closed connection")
            third.send({"type": FRAME_TYPE.LOGIN, "username": username, "password": password})
        check(frame["player_id"] == player_id, f"the durable id must not change: {frame}")

        # --- a fresh account has an empty history (§3.4) ----------------------------------
        empty = stats_for(third, "relogin")
        check(empty["games"] == 0 and empty["net_score"] == 0 and empty["recent"] == [],
              f"a fresh account must have no history: {empty}")

        # --- guests keep working and get a zeroed payload (§3.3) --------------------------
        guest = Client(server.port, "guest")
        guest.send({"type": FRAME_TYPE.LOGIN, "name": "游客_测试"})
        guest_ok = guest.recv_type(FRAME_TYPE.LOGIN_OK)
        check(guest_ok["player_id"].startswith("p-"), f"a guest keeps a per-process id: {guest_ok}")
        guest_stats = stats_for(guest, "guest")
        check(guest_stats["games"] == 0, f"a guest has no history: {guest_stats}")

        # --- play one WHOLE match and check it was recorded (§4, E5) ----------------------
        room_id, ends, match_end, mine = play_one_match(third, username)
        for end in ends:
            check(end["reason"] in (REASON.WIN_RON, REASON.WIN_TSUMO, REASON.DRAW_EXHAUSTED), f"bad reason: {end}")
            check(len(end["results"]) == SEAT_COUNT, f"game_end must carry 4 results: {end}")
            check(sum(entry["delta_score"] for entry in end["results"]) == 0,
                  f"the results must stay zero-sum: {end}")
        ends_count = len(ends)
        check(4 <= ends_count <= 8, f"一场必须 4..8 局（C15 双闸），got {ends_count}")
        check(match_end["hands_played"] == ends_count,
              f"M2/D2: hands_played {match_end['hands_played']} != game_end 次数 {ends_count}")
        check(match_end["renchan_count"] == ends[-1]["renchan_count"],
              "M2: renchan_count 必须与最后一次 game_end 同值")
        my_total = next(entry["match_total"] for entry in ends[-1]["results"] if entry["seat"] == 0)
        check(my_total == mine["delta_total"],
              f"累计分必须等于各局 delta 之和: {my_total} vs {mine['delta_total']}")
        my_rank = next(entry["rank"] for entry in match_end["rankings"] if entry["seat"] == 0)
        check(next(entry["final_score"] for entry in match_end["rankings"] if entry["seat"] == 0)
              == my_total, "M10: final_score 必须与 match_total 同一份数据")
        print(f"[account-e2e] played one match: rounds={ends_count} my_total={my_total} "
              f"wins={mine['wins']} draws={mine['draws']} dealer_rounds={mine['dealer_rounds']} "
              f"rank={my_rank}")

        deadline = time.monotonic() + 10
        while True:
            stats = stats_for(third, "after-match")
            if stats["games"] == ends_count:
                break
            if time.monotonic() > deadline:
                raise TestFailure(f"the finished match was never recorded: {stats}")
            time.sleep(0.1)  # the write is asynchronous by design (§4.1)
        check(stats["net_score"] == my_total, f"net_score must match the match: {stats} vs {my_total}")
        check(stats["wins"] == mine["wins"], f"wins must match the match: {stats}")
        check(stats["draws"] == mine["draws"], f"draws must match the match: {stats}")
        check(len(stats["recent"]) == ends_count,
              f"matches 按局一行：recent 应有 {ends_count} 行: {len(stats['recent'])}")
        check(all(entry["room_id"] == room_id for entry in stats["recent"]),
              f"recent must name the room: {stats['recent'][:2]}")
        # 日麻接入：统计按玩法分桶，且每行带 mode（两套玩法的分数不可比，不能混算）
        check(isinstance(stats.get("classic"), dict) and isinstance(stats.get("riichi"), dict),
              f"stats must carry classic/riichi buckets: {sorted(stats)}")
        check(stats["riichi"]["games"] == 0,
              f"this match is classic, so the riichi bucket must be empty: {stats['riichi']}")
        check(all(entry.get("mode") == MODE.CLASSIC for entry in stats["recent"]),
              f"recent entries must carry mode=classic: {stats['recent'][:2]}")
        print(f"[account-e2e] match history recorded: games={stats['games']} "
              f"net_score={stats['net_score']}")

        # --- E5 落库核对（ACCOUNT_DESIGN v1.3 §4 第 7 条 / §7.2）：matches 每局一行、
        # match_results 一场每账号一行、字段与 match_end 同源 ------------------------------
        matches_rows = sql_query(
            args, f"SELECT COUNT(*) FROM matches m JOIN accounts a ON a.id = m.account_id "
                  f"WHERE a.username = '{username}' AND m.room_id = '{room_id}';").strip()
        check(matches_rows == str(ends_count),
              f"matches 表必须每局一行（{ends_count}），got {matches_rows!r}")
        result_rows = sql_query(
            args, f"SELECT hands_played, dealer_hands, final_score, final_rank FROM match_results mr "
                  f"JOIN accounts a ON a.id = mr.account_id "
                  f"WHERE a.username = '{username}' AND mr.room_id = '{room_id}';").strip().splitlines()
        check(len(result_rows) == 1,
              f"match_results 必须恰 1 行（一场每账号一行 + UNIQUE 约束），got {len(result_rows)}")
        fields = result_rows[0].split("\t")
        db_hands, db_dealer, db_score, db_rank = (int(value) for value in fields)
        check(db_hands == ends_count, f"match_results.hands_played 必须等于 game_end 次数: {db_hands}")
        check(db_dealer == mine["dealer_rounds"],
              f"match_results.dealer_hands 必须等于坐庄手数: {db_dealer} vs {mine['dealer_rounds']}")
        check(db_score == my_total, f"match_results.final_score 必须等于本场累计: {db_score} vs {my_total}")
        check(db_rank == my_rank, f"match_results.final_rank 必须与 rankings 同源: {db_rank} vs {my_rank}")
        print(f"[account-e2e] match_results row verified: hands={db_hands} dealer={db_dealer} "
              f"score={db_score} rank={db_rank}")
        third.close()
        guest.close()
        second.close()

        # --- 再来一局 (UI_UX_SPEC §9.5 第 7 条): a new connection right after a finished game
        # gets a brand-new room — that reconnect-and-replay handshake is all the client's
        # rematch button does, and the server needs no new frame for it.
        rematch = Client(server.port, "rematch")
        rematch.send({"type": FRAME_TYPE.LOGIN, "username": username, "password": password, "resume": False})
        check(rematch.recv_frame().get("type") == FRAME_TYPE.LOGIN_OK, "rematch must be able to log in again")
        rematch.send({"type": FRAME_TYPE.CREATE_ROOM})
        rejoined = rematch.recv_type(FRAME_TYPE.ROOM_JOINED)
        check(rejoined["room_id"] != room_id,
              f"a rematch must open a NEW room, not the finished one: {rejoined} vs {room_id}")
        rematch.send({"type": FRAME_TYPE.READY})
        check(rematch.recv_type(FRAME_TYPE.GAME_START)["seat"] == 0, "the rematch room must start normally")
        second_room_id = rejoined["room_id"]
        rematch.close()
        print(f"[account-e2e] rematch opened a fresh room {second_room_id} after {room_id}")

        # --- the account outlives the process (§7.3) --------------------------------------
        server.stop()
        server.start()
        after_restart = Client(server.port, "after-restart")
        after_restart.send({"type": FRAME_TYPE.LOGIN, "username": username, "password": password})
        again = after_restart.recv_type(FRAME_TYPE.LOGIN_OK)
        check(again["player_id"] == player_id, f"the player_id must survive a restart: {again}")
        check(stats_for(after_restart, "after-restart")["games"] == ends_count,
              "the match history must survive a restart")
        after_restart.close()
        print("[account-e2e] account and history survived a server restart")
    except TestFailure:
        print("[account-e2e] server log:\n" + server.log_tail(), file=sys.stderr)
        raise
    finally:
        server.stop()
        cleanup_rows(args, [username, username + "_b"])


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
        print(f"[account-e2e] FAIL: {failure}", file=sys.stderr)
        return 1
    print("[account-e2e] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
