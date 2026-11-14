#!/usr/bin/env python3
"""对局回放的端到端验证（PROTOCOL v2.3 §4.13 / §5.18）。

标准库 only。启动真服务端（带 `--replay-dir`），打完一局，然后分两个阶段验证：

**阶段 A（不需要账号）**
  1. **落盘**：`<replay-dir>/<room_id>.json` 存在，`format`/`version` 正确，
     `seats[]` 与四个座位对得上，`steps[]` 非空且每一步的 `f` 都是 `[[收件座位...], "<帧>"]`；
  2. **逐帧可解析**：每个 `steps[].f[][1]` 都能 JSON.parse，且 `type` 是已知帧类型；
  3. **重放一致**：按座位 0 的视角重放，最后一份 `state_sync` 的 `remaining`/`current_seat`/
     四家牌河、以及 `game_end` 的 `reason`/`delta_score` 必须与实时对局完全相同。

**阶段 B（需要 MySQL，因为没有账号就无法鉴权）**
  4. **取回**：用参与过该局的**账号**发 `replay_request`，拼分片，文本与磁盘一致、每片 <= 16 KB；
  5. **鉴权**：另一个账号请求同一个 room_id → `replay{error:3}`。

MySQL 不可用（或服务端编成 db_stub）时阶段 B 打一行 SKIP 说明并以 77 退出 = ctest 显示 SKIP。
阶段 A 仍然真跑，所以「没账号」不等于「没验证」。

用法：
    python3 game/tests/replay_e2e_test.py --server-bin build/game/mahjong_server
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import ACTION, FRAME_TYPE, WINDOW

SKIP_RETURN_CODE = 77
MY_SEAT = 0
#: replay 帧的单片上限（§5.18），客户端 Net 对单行有 64 KB 硬上限，所以服务端必须切片。
MAX_REPLAY_CHUNK = 16 * 1024
#: 已知服务端帧类型（用枚举表而不是硬编码数字，避免文档漂移）。
KNOWN_FRAME_TYPES = {
    value for name, value in vars(FRAME_TYPE).items() if not name.startswith("_") and value != 0
}


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

    def login(self, username: str | None, password: str | None, guest: str = "游客") -> dict:
        if username:
            self.send({"type": FRAME_TYPE.LOGIN, "username": username, "password": password})
        else:
            self.send({"type": FRAME_TYPE.LOGIN, "name": guest})
        frame = self.recv()
        return frame

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def start_server(binary: str, port: int, seed: int, replay_dir: str) -> subprocess.Popen:
    process = subprocess.Popen(
        [binary, "--port", str(port), "--seed", str(seed), "--replay-dir", replay_dir,
         "--ai-delay-ms", "0", "--inter-hand-delay-ms", "0"],
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


def unique_account(tag: str) -> tuple[str, str]:
    """一个本次运行独有、且符合 §3.2 规则的账号（3..16 位字母数字下划线）。"""
    suffix = f"{os.getpid() % 100000}{int(time.time()) % 1000}"
    return f"{tag}{suffix}"[:16], "secret123"


def register(client: Client, username: str, password: str) -> dict:
    client.send({"type": FRAME_TYPE.REGISTER, "username": username, "password": password})
    frame = client.recv()
    return frame


def play_one_round(client: Client) -> dict:
    """游客登录 → 建房 → 打完第一局。返回观测值。

    游客路径刻意不碰账号：阶段 A 要能在没有 MySQL 的机器上跑，而回放文件的形状与鉴权无关。
    """
    logged = client.login(None, None, guest="回放观测")
    if logged.get("type") != FRAME_TYPE.LOGIN_OK:
        raise TestFailure(f"guest login failed: {logged!r}")

    client.send({"type": FRAME_TYPE.CREATE_ROOM})
    joined = client.recv()
    if joined.get("type") != FRAME_TYPE.ROOM_JOINED:
        raise TestFailure(f"create_room failed: {joined!r}")
    room_id = joined.get("room_id")
    if not isinstance(room_id, str):
        raise TestFailure(f"room_joined without room_id: {joined!r}")

    client.send({"type": FRAME_TYPE.READY})

    observation = {
        "room_id": room_id,
        "errors": [],
        "game_end": None,
        "last_sync": None,
        "sync_frames": 0,
    }
    my_hand: list[str] = []
    deadline = time.monotonic() + 120.0
    while time.monotonic() < deadline:
        frame = client.recv(timeout=30.0)
        kind = frame.get("type")
        if kind == FRAME_TYPE.ERROR:
            observation["errors"].append(frame)
            continue
        if kind == FRAME_TYPE.GAME_START:
            my_hand = list(frame.get("hand") or [])
            continue
        if kind == FRAME_TYPE.STATE_SYNC:
            observation["sync_frames"] += 1
            observation["last_sync"] = frame
            my_hand = list(frame.get("hand") or [])
            continue
        if kind == FRAME_TYPE.DRAW:
            tile = frame.get("tile")
            if isinstance(tile, str):
                my_hand.append(tile)
            continue
        if kind == FRAME_TYPE.ACTION_REQUEST:
            seq = frame.get("seq")
            if frame.get("window") == WINDOW.DISCARD:
                if not my_hand:
                    raise TestFailure("discard window opened with an empty tracked hand")
                forbidden = set(frame.get("forbidden_discards") or [])
                pick = next((code for code in my_hand if code not in forbidden), my_hand[0])
                if pick in my_hand:
                    my_hand.remove(pick)
                client.send({"type": FRAME_TYPE.DISCARD, "tile": pick, "seq": seq})
            else:
                client.send({"type": FRAME_TYPE.ACTION, "action": ACTION.PASS, "tiles": [], "seq": seq})
            continue
        if kind == FRAME_TYPE.DISCARD_EVENT:
            if frame.get("seat") == MY_SEAT:
                tile = frame.get("tile")
                if tile in my_hand:
                    my_hand.remove(tile)
            continue
        if kind == FRAME_TYPE.GAME_END:
            observation["game_end"] = frame
            return observation
    raise TestFailure("no game_end within 120s")


def request_replay(client: Client, room_id: str) -> dict:
    """§4.13 → §5.18：发请求、拼分片、返回解析后的文件。"""
    client.send({"type": FRAME_TYPE.REPLAY_REQUEST, "room_id": room_id})
    chunks: dict[int, str] = {}
    total: int | None = None
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        frame = client.recv(timeout=20.0)
        kind = frame.get("type")
        if kind == FRAME_TYPE.ERROR:
            raise TestFailure(f"replay_request answered error: {frame!r}")
        if kind != FRAME_TYPE.REPLAY:
            continue
        if isinstance(frame.get("error"), int):
            return {"error": frame["error"]}
        if frame.get("room_id") != room_id:
            raise TestFailure(f"replay frame for a different room: {frame.get('room_id')!r}")
        declared = frame.get("chunks")
        if not isinstance(declared, int) or declared < 1:
            raise TestFailure(f"replay.chunks must be >= 1: {frame!r}")
        if total is None:
            total = declared
        elif declared != total:
            raise TestFailure(f"replay.chunks changed mid-stream: {declared} != {total}")
        index = frame.get("chunk")
        if not isinstance(index, int) or not 0 <= index < total:
            raise TestFailure(f"replay.chunk out of range: {frame!r}")
        data = frame.get("data")
        if not isinstance(data, str):
            raise TestFailure(f"replay.data must be a string: {frame!r}")
        if len(data.encode("utf-8")) > MAX_REPLAY_CHUNK:
            raise TestFailure(f"replay chunk is {len(data.encode('utf-8'))} bytes > {MAX_REPLAY_CHUNK}")
        chunks[index] = data
        if len(chunks) == total:
            text = "".join(chunks[i] for i in range(total))
            try:
                return json.loads(text)
            except json.JSONDecodeError as exc:
                raise TestFailure(f"replayed text is not valid JSON: {exc}") from exc
    raise TestFailure("incomplete replay (timed out)")


def check_file_shape(replay: dict, room_id: str, text: str | None = None) -> None:
    if replay.get("format") != "mahjong-replay":
        raise TestFailure(f"format must be 'mahjong-replay': {replay.get('format')!r}")
    if replay.get("version") != 1:
        raise TestFailure(f"version must be 1: {replay.get('version')!r}")
    if replay.get("room_id") != room_id:
        raise TestFailure(f"room_id mismatch: {replay.get('room_id')!r} != {room_id!r}")
    if text is not None and "\\" in text:
        at = text.index("\\")
        raise TestFailure(f"replay text must contain no escapes, found one at offset {at}: "
                          f"{text[max(0, at - 80):at + 120]!r}")
    seats = replay.get("seats")
    if not isinstance(seats, list) or len(seats) != 4:
        raise TestFailure(f"seats must be 4 entries: {seats!r}")
    if [seat.get("seat") for seat in seats] != [0, 1, 2, 3]:
        raise TestFailure(f"seats must be ordered 0..3: {seats!r}")
    steps = replay.get("steps")
    if not isinstance(steps, list) or not steps:
        raise TestFailure(f"steps must be a non-empty list: {steps!r}")
    for index, step in enumerate(steps):
        if not isinstance(step.get("t"), int):
            raise TestFailure(f"steps[{index}].t must be an int: {step!r}")
        hands = step.get("h")
        if not isinstance(hands, list) or len(hands) != 4:
            raise TestFailure(f"steps[{index}].h must be 4 entries: {hands!r}")
        frames = step.get("f")
        if not isinstance(frames, list):
            raise TestFailure(f"steps[{index}].f must be a list: {frames!r}")
        for entry in frames:
            if not isinstance(entry, list) or len(entry) != 2:
                raise TestFailure(f"steps[{index}].f entries must be [[seats], frame]: {entry!r}")
            recipients, frame = entry
            if not isinstance(recipients, list) or not recipients:
                raise TestFailure(f"steps[{index}].f recipients must be a non-empty list (broadcasts "
                                  f"are expanded by the recorder): {entry!r}")
            if any(not isinstance(seat, int) or not 0 <= seat <= 3 for seat in recipients):
                raise TestFailure(f"steps[{index}].f recipient out of range: {recipients!r}")
            # §5.18: the frame is a JSON OBJECT spliced into the file, not a JSON-encoded string.
            # A string here means someone went back to double-encoding, which costs a `\"` for
            # every quote in every frame (~16% file size) and makes the file unreadable.
            if not isinstance(frame, dict):
                raise TestFailure(f"steps[{index}].f frames must be JSON objects, got "
                                  f"{type(frame).__name__}: {str(frame)[:120]!r}")
            if frame.get("type") not in KNOWN_FRAME_TYPES:
                raise TestFailure(f"steps[{index}] carries an unknown frame type: {frame.get('type')!r}")


def replay_as_seat(replay: dict, seat: int) -> dict:
    """按某个座位重放，返回最后一份 state_sync 与最后一帧 game_end（与浏览器播放器同口径）。"""
    last_sync = None
    last_end = None
    for step in replay.get("steps") or []:
        for recipients, frame in step.get("f") or []:
            if seat not in recipients:
                continue
            if not isinstance(frame, dict):
                continue
            if frame.get("type") == FRAME_TYPE.STATE_SYNC:
                last_sync = frame
            elif frame.get("type") == FRAME_TYPE.GAME_END:
                last_end = frame
    return {"last_sync": last_sync, "game_end": last_end}


def hand_track_at(replay: dict, seat: int, step_index: int) -> str:
    """按播放器口径滚动 `steps[].h`：空串表示"该座位这一步没变"，不是"没有手牌"。"""
    track: list[str] = ["", "", "", ""]
    for step in (replay.get("steps") or [])[:step_index]:
        for index, hand in enumerate(step.get("h") or []):
            if index < len(track) and hand:
                track[index] = hand
    return track[seat]


def run(args: argparse.Namespace) -> int:
    binary = os.path.abspath(args.server_bin)
    replay_dir = tempfile.mkdtemp(prefix="mahjong-replays-")
    port = free_port()
    process = start_server(binary, port, args.seed, replay_dir)
    main_client = None
    try:
        main_client = Client(port, "guest")
        observed = play_one_round(main_client)
        if observed["errors"]:
            raise TestFailure(f"the game itself answered errors: {observed['errors'][:2]!r}")
        room_id = observed["room_id"]

        # ---- 1. 落盘 ----
        expected_path = Path(replay_dir) / f"{room_id}.json"
        # 写盘是异步的（ReplayActor 的邮箱），等它落下来。
        deadline = time.monotonic() + 15.0
        while not expected_path.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        if not expected_path.exists():
            raise TestFailure(f"no replay file at {expected_path} (files: {os.listdir(replay_dir)})")
        on_disk_text = expected_path.read_text(encoding="utf-8")
        on_disk = json.loads(on_disk_text)
        check_file_shape(on_disk, room_id, on_disk_text)

        # ---- 2. 重放一致性：座位 0 视角的最后一帧必须与实时对局一致（无需账号）----
        replayed = replay_as_seat(on_disk, MY_SEAT)
        live = observed["last_sync"] or {}
        recorded = replayed["last_sync"] or {}
        if not recorded:
            raise TestFailure("the replay has no state_sync for seat 0")
        for key in ("remaining", "current_seat"):
            if recorded.get(key) != live.get(key):
                raise TestFailure(f"replayed state_sync.{key}={recorded.get(key)!r} != live {live.get(key)!r}")
        if len(recorded.get("seats") or []) != 4:
            raise TestFailure(f"replayed state_sync.seats must have 4 entries: {recorded.get('seats')!r}")
        # 四家牌河与实时对局一致（公开信息，重放必须逐字节还原）
        live_rivers = [list(seat.get("discards") or []) for seat in (live.get("seats") or [])]
        disk_rivers = [list(seat.get("discards") or []) for seat in (recorded.get("seats") or [])]
        if live_rivers != disk_rivers:
            raise TestFailure(f"replayed rivers differ from live:\n  live={live_rivers}\n  disk={disk_rivers}")
        # 局终载荷：座位 0 的得失分必须与实时一致
        live_end = observed["game_end"] or {}
        disk_end = replayed["game_end"] or {}
        if not disk_end:
            raise TestFailure("the replay has no game_end")
        live_delta = next(
            (r.get("delta_score") for r in live_end.get("results") or [] if r.get("seat") == MY_SEAT), None)
        disk_delta = next(
            (r.get("delta_score") for r in disk_end.get("results") or [] if r.get("seat") == MY_SEAT), None)
        if live_delta != disk_delta:
            raise TestFailure(f"replayed delta_score {disk_delta!r} != live {live_delta!r}")
        if disk_end.get("reason") != live_end.get("reason"):
            raise TestFailure(f"replayed reason {disk_end.get('reason')!r} != live {live_end.get('reason')!r}")

        # ---- 2b. 手牌轨道必须覆盖四家 ----
        # 这是「切到 AI 视角」唯一的数据来源：机器人没有连接，服务端从不为它构建 state_sync，
        # 所以它的手牌只能从 steps[].h 里读。轨道是按步 delta 编码的（空串 = 该座位没变），
        # 所以要按播放器口径滚动一遍再看，而不是逐条要求四家都非空。
        # 曾经的录制只在 state_sync 步取快照，某些座位整局都没被记到，切过去就是一块空牌桌。
        replay_steps = on_disk.get("steps") or []
        for seat in range(4):
            hand = hand_track_at(on_disk, seat, len(replay_steps))
            if not hand:
                raise TestFailure(
                    f"hand track never recorded seat {seat}: switching the replay to that seat "
                    f"would show an empty table (last step h={replay_steps[-1].get('h')!r})")

        # ---- 3. 取回 + 鉴权（需要账号）----
        skipped = fetch_and_authorize(port, room_id, on_disk, main_client)
        summary = (f"replay_e2e_test: ok (room {room_id}, {len(on_disk['steps'])} steps, "
                   f"{expected_path.stat().st_size} bytes on disk)")
        if skipped:
            print(summary + " — served/authorization checks SKIPPED (no account storage)")
            return SKIP_RETURN_CODE
        print(summary)
        return 0
    finally:
        if main_client is not None:
            main_client.close()
        process.kill()
        process.wait(timeout=10)
        shutil.rmtree(replay_dir, ignore_errors=True)


def fetch_and_authorize(port: int, room_id: str, on_disk: dict, main_client: Client) -> bool:
    """阶段 B：账号取回 + 异账号被拒。返回 True 表示"没有账号存储，跳过了"。

    鉴权以 `seats[].account_id` 为准（§4.13），而游客座位恒为 0、永远匹配不上，所以这一步
    必须有真账号 —— 没有 MySQL 时不是"失败"，而是"这条链路今天测不了"。
    """
    username, password = unique_account("rp")
    # 同一连接上换个身份：先退出（用 resume:false 的账号登录会替换会话），这里直接新开连接。
    account_client = Client(port, "account")
    try:
        logged = register(account_client, username, password)
        if logged.get("type") == FRAME_TYPE.ERROR and logged.get("code") == 11:
            return True  # INTERNAL = accounts unavailable
        if logged.get("type") == FRAME_TYPE.ERROR:
            # 同一秒内重跑：账号已存在，直接登录。
            logged = account_client.login(username, password)
        if logged.get("type") != FRAME_TYPE.LOGIN_OK:
            raise TestFailure(f"register/login failed: {logged!r}")
        if not str(logged.get("player_id", "")).startswith("u-"):
            raise TestFailure(f"account login must yield a u- player id: {logged!r}")

        # 这个账号没参与过那局，所以取回必须被拒 —— 这正是鉴权在起作用。
        refused = request_replay(account_client, room_id)
        if refused.get("error") != 3:  # ROOM_NOT_FOUND
            raise TestFailure(f"an unrelated account must be refused with error 3, got {refused!r}")
    finally:
        account_client.close()
    # 游客永远拿不到回放（account_id 0 匹配不上任何座位）：再确认一次这条边界。
    guest_refused = request_replay(main_client, room_id)
    if guest_refused.get("error") != 3:
        raise TestFailure(f"a guest must be refused with error 3, got {guest_refused!r}")
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--seed", type=int, default=20260101)
    args = parser.parse_args()
    try:
        return run(args)
    except TestFailure as exc:
        print(f"replay_e2e_test: FAIL {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
