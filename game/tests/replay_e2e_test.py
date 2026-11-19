#!/usr/bin/env python3
"""对局回放的端到端验证（PROTOCOL v2.3 §4.13 / §5.18）。

标准库 only。启动真服务端（带 `--replay-dir`），打完一局，然后分两个阶段验证：

**阶段 A（不需要账号）**
  1. **落盘**：`<replay-dir>/<room_id>.h00.json.gz` 起的分片存在（v2.7：按局一片，一局写完即
     丢，房间内存里只有当前这一局），每片是 gzip 流且自带完整头部；把分片按局序合并后，
     `format`/`version`/`seats[]`/`steps[]` 的形状与单文件时代一致，`steps[].f` 仍是
     `[[收件座位...], <帧对象>]`；
  2. **逐帧可解析**：每个 `steps[].f[][1]` 都能 JSON.parse，且 `type` 是已知帧类型；
  3. **重放一致**：按座位 0 的视角重放，最后一份 `state_sync` 的 `remaining`/`current_seat`/
     四家牌河、以及 `game_end` 的 `reason`/`delta_score` 必须与实时对局完全相同；
  4. **合并口径**：`steps[].t` 跨局单调不减（每局新建录制器时最容易退化的就是它）。

**阶段 B（需要 MySQL，因为没有账号就无法鉴权）**
  4. **取回**：用参与过该局的**账号**发 `replay_request`，拼分片，还原出的文件与「磁盘分片合并
     结果」一致、每片 <= 16 KB —— 客户端看到的仍是整场一份，分片是服务端内部布局；
  5. **鉴权**：另一个账号请求同一个 room_id → `replay{error:3}`；
  6. **旧布局回退**：删掉分片、留一份 `<room_id>.json.gz`（v2.6 整场）和一份 `<room_id>.json`
     （v2.5 明文），两者都必须仍能读出同样的内容。

MySQL 不可用（或服务端编成 db_stub）时阶段 B 打一行 SKIP 说明并以 77 退出 = ctest 显示 SKIP。
阶段 A 仍然真跑，所以「没账号」不等于「没验证」。

用法：
    python3 game/tests/replay_e2e_test.py --server-bin build/game/mahjong_server
"""

from __future__ import annotations

import argparse
import base64
import gzip
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import ACTION, FRAME_TYPE, WINDOW

SKIP_RETURN_CODE = 77
MY_SEAT = 0
#: replay 帧的单片上限（§5.18），客户端 Net 对单行有 64 KB 硬上限，所以服务端必须切片。
MAX_REPLAY_CHUNK = 16 * 1024
#: §5.18 v2.6 的载荷编码。空串 = 明文（旧服务端/旧文件）。
REPLAY_ENCODING_GZIP = "gzip"
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
        # §7.2: the server drops (and reports RATE_LIMITED for) anything over 100 inbound frames a
        # second. A human never gets close, but this driver answers every window the instant it
        # opens, so with --ai-delay-ms 0 a whole match can finish inside 100 ms of wall clock and
        # the limit fires on the harness rather than on anything under test. Pacing the writes
        # keeps the run honest: the assertions are about the recording, not about how fast a bot
        # can mash buttons.
        self.min_send_interval = 0.012
        self._last_send = 0.0

    def send(self, frame: dict) -> None:
        if self.min_send_interval > 0.0:
            wait = self._last_send + self.min_send_interval - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            self._last_send = time.monotonic()
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

    def request_stats(self) -> dict:
        """§4.10 `stats_request` → §5.13 `stats`。这里只当一条 FIFO 屏障用（见调用处）。"""
        self.send({"type": FRAME_TYPE.STATS_REQUEST})
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            frame = self.recv(timeout=20.0)
            if frame.get("type") in (FRAME_TYPE.STATS, FRAME_TYPE.ERROR):
                return frame
        return {"type": "timeout"}


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


def play_one_round(client: Client, username: str | None = None, password: str | None = None,
                   already_logged_in: bool = False, whole_match: bool = False) -> dict:
    """登录 → 建房 → 打完第一局（或整场）。返回观测值。

    不传账号就是游客：阶段 A 要能在没有 MySQL 的机器上跑，而回放文件的形状与鉴权无关。
    传账号则这一局的座位带真实 `account_id`，阶段 B 用它验证「参与者能取回」。

    |whole_match| 一直打到 `match_end`（4..8 局）：录制按局分片，只有整场才有多片可合并，
    而「多片合并」正是分片方案唯一可能静默出错的环节（片序、尾部字段、`t` 的连续性）。
    |already_logged_in| 用于 register 之后（§3.2：注册成功即登录，再 login 会被拒）。
    """
    if already_logged_in:
        logged = {"type": FRAME_TYPE.LOGIN_OK}
    else:
        logged = client.login(username, password, guest="回放观测")
    if logged.get("type") != FRAME_TYPE.LOGIN_OK:
        raise TestFailure(f"login failed: {logged!r}")

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
        # 局终帧全部留下：整场时最后一份 `game_end` 才是"当前局"的那个，而分片数与它相等。
        "game_ends": [],
    }
    my_hand: list[str] = []
    # 整场是 4..8 局，每局一手的窗口都要等；实测（ai_delay=0）一整场约 15 秒，留足余量给慢机器。
    deadline = time.monotonic() + (300.0 if whole_match else 120.0)
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
            observation["game_ends"].append(frame)
            if whole_match:
                continue  # 下一局在同一连接上继续；整场以 match_end 结束
            return observation
        if kind == FRAME_TYPE.MATCH_END:
            if not whole_match:
                raise TestFailure("match_end arrived when only one round was requested")
            observation["match_end"] = frame
            return observation
    raise TestFailure("no game_end/match_end within 120s")


def request_replay(client: Client, room_id: str) -> dict:
    """§4.13 → §5.18：发请求、拼分片、还原载荷、返回解析后的文件。"""
    client.send({"type": FRAME_TYPE.REPLAY_REQUEST, "room_id": room_id})
    chunks: dict[int, str] = {}
    total: int | None = None
    encoding = ""
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
            # §5.18 v2.6：编码对一次应答恒定，只在第一片读；缺省 = 明文（旧服务端）。
            declared_encoding = frame.get("encoding")
            if declared_encoding is not None and not isinstance(declared_encoding, str):
                raise TestFailure(f"replay.encoding must be a string: {frame!r}")
            encoding = declared_encoding or ""
            if encoding and encoding != REPLAY_ENCODING_GZIP:
                raise TestFailure(f"unexpected replay.encoding: {encoding!r}")
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
            joined = "".join(chunks[i] for i in range(total))
            try:
                text = decode_replay_payload(joined, encoding)
                return json.loads(text)
            except (ValueError, zlib.error) as exc:
                raise TestFailure(f"replayed payload did not decode: {exc}") from exc
            except json.JSONDecodeError as exc:
                raise TestFailure(f"replayed text is not valid JSON: {exc}") from exc
    raise TestFailure("incomplete replay (timed out)")


def decode_replay_payload(joined: str, encoding: str) -> str:
    """§5.18 的载荷还原。空编码 = 明文；`gzip` = base64 的 gzip 流（v2.6）。

    客户端用的是 `DecompressionStream('gzip')`（见 game/web/js/replay_payload.js），这里用
    标准库的 gzip 做同一件事 —— 两边解的是同一份字节，所以这条断言同时也守住了
    「浏览器真的能解开」。
    """
    if not encoding:
        return joined
    if encoding == "gzip":
        return gzip.decompress(base64.b64decode(joined, validate=True)).decode("utf-8")
    raise TestFailure(f"unsupported replay encoding: {encoding!r}")


def check_file_shape(replay: dict, room_id: str, text: str | None = None) -> None:
    if replay.get("format") != "mahjong-replay":
        raise TestFailure(f"format must be 'mahjong-replay': {replay.get('format')!r}")
    if replay.get("version") != 2:
        raise TestFailure(f"version must be 2: {replay.get('version')!r}")
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
        # `v` 只在某座位私有视图变化时出现；出现时四家各占一位（空串 = 该座位没变）。
        views = step.get("v")
        if views is not None and (not isinstance(views, list) or len(views) != 4):
            raise TestFailure(f"steps[{index}].v must be 4 entries: {views!r}")
        for seat, view in enumerate(views or []):
            if not isinstance(view, (dict, str)):
                raise TestFailure(f"steps[{index}].v[{seat}] must be an object or \"\": {view!r}")
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


def seat_view_at(replay: dict, seat: int, step_index: int) -> dict:
    """按播放器口径滚动 `steps[].v`：缺席/空串表示"该座位这一步没变"，不是"没有视图"。"""
    track: list[dict] = [{}, {}, {}, {}]
    for step in (replay.get("steps") or [])[:step_index]:
        for index, view in enumerate(step.get("v") or []):
            if index < len(track) and isinstance(view, dict):
                track[index] = view
    return track[seat]


def hand_track_at(replay: dict, seat: int, step_index: int) -> list[str]:
    """该座位在第 `step_index` 步之前最后一份视图里的门前手牌。"""
    hand = seat_view_at(replay, seat, step_index).get("hand")
    return hand if isinstance(hand, list) else []


def part_paths(replay_dir: str, room_id: str) -> list[Path]:
    """该房间已落盘的每一局分片，按局序（`<room_id>.h00.json.gz` 起）。"""
    paths = sorted(Path(replay_dir).glob(f"{room_id}.h??.json.gz"))
    return paths


def wait_for_parts(replay_dir: str, room_id: str, timeout: float = 15.0) -> list[Path]:
    """等第一片落下来（写盘是 ReplayActor 邮箱里的异步操作）。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        paths = part_paths(replay_dir, room_id)
        if paths:
            return paths
        time.sleep(0.05)
    raise TestFailure(f"no replay part for {room_id} (files: {os.listdir(replay_dir)})")


def merge_parts(paths: list[Path], room_id: str) -> tuple[dict, list[str]]:
    """把分片按局序合并成一份完整回放（与服务端 replay.cc MergeReplayParts 同口径）。

    服务端是纯文本拼接（`steps` 数组内文原样串起来，其余字段取最后一片），这里按 JSON 语义
    做同一件事：`steps` 顺序拼接，`finished`/`match_over`/`totals`/`final_rank` 取最后一片 ——
    只有它知道整场是否结束。两边等价，所以既能验"合并后的东西是对的"，又能顺便守住"分片
    里没有任何字段丢失"。返回合并结果与每一片的原始文本（后者用来查"片内无转义"这类字节级
    性质，JSON 解析之后就看不到了）。
    """
    texts = []
    parts = []
    for path in paths:
        raw = path.read_bytes()
        if raw[:2] != b"\x1f\x8b":
            raise TestFailure(f"{path.name} is not a gzip stream: {raw[:8]!r}")
        text = gzip.decompress(raw).decode("utf-8")
        texts.append(text)
        parts.append(json.loads(text))
    for path, part in zip(paths, parts):
        if part.get("format") != "mahjong-replay" or part.get("room_id") != room_id:
            raise TestFailure(f"{path.name} is not a part of {room_id}: {str(part)[:120]!r}")
        if not isinstance(part.get("steps"), list):
            raise TestFailure(f"{path.name} has no steps list: {str(part)[:120]!r}")
    merged = dict(parts[-1])
    merged["steps"] = [step for part in parts for step in part["steps"]]
    return merged, texts


def run(args: argparse.Namespace) -> int:
    binary = os.path.abspath(args.server_bin)
    replay_dir = tempfile.mkdtemp(prefix="mahjong-replays-")
    port = free_port()
    process = start_server(binary, port, args.seed, replay_dir)
    main_client = None
    try:
        main_client = Client(port, "guest")
        # 整场（4..8 局）而不是一局：分片方案唯一的"静默错"是合并 —— 片序、尾部取哪一片、
        # `t` 是否跨局连续。只录一局的话这三条一条都测不到（一片合并 = 原样返回）。
        observed = play_one_round(main_client, whole_match=True)
        if observed["errors"]:
            raise TestFailure(f"the game itself answered errors: {observed['errors'][:2]!r}")
        room_id = observed["room_id"]
        hands_played = len(observed["game_ends"])
        if hands_played < 4:
            raise TestFailure(f"a match must settle 4..8 hands, got {hands_played}")

        # ---- 1. 落盘 ----
        # 录制按**局**分片（`<room_id>.h00.json.gz` 起，v2.7）：一局写完就丢，房间内存里只有
        # 当前这一局的帧；取回时服务端把分片合并回一份完整文件，所以 §5.18 的形状没变。
        # 每片都是 gzip 流，且都带完整的头部（format/room_id/seats），能单独读懂。
        parts = wait_for_parts(replay_dir, room_id)
        deadline = time.monotonic() + 20.0
        while len(parts) < hands_played and time.monotonic() < deadline:
            time.sleep(0.05)
            parts = part_paths(replay_dir, room_id)
        if len(parts) != hands_played:
            raise TestFailure(f"expected one part per settled hand ({hands_played}), got "
                              f"{len(parts)}: {[p.name for p in parts]}")
        on_disk, part_texts = merge_parts(parts, room_id)
        check_file_shape(on_disk, room_id)
        if any(p.stat().st_size == 0 for p in parts):
            raise TestFailure(f"a replay part is empty: {[p.name for p in parts]}")
        # 每片只含它那一局：新局以 `game_start` 开头，所以逐片数一遍应该正好等于局数。
        # 这条也是"分片边界真的落在局边界上"的断言，而不只是"文件数对得上"。
        starts = 0
        for text in part_texts:
            for step in json.loads(text).get("steps") or []:
                if any(isinstance(frame, dict) and frame.get("type") == FRAME_TYPE.GAME_START
                       for _seats, frame in step.get("f") or []):
                    starts += 1
                    break
        if starts != hands_played:
            raise TestFailure(f"expected {hands_played} hand openings across the parts, found {starts}")
        # §5.18: frames are spliced into the file as JSON objects, so neither a part nor the merged
        # document may contain a single escaped quote. Asserted on the parts' own bytes because the
        # merged text is assembled by the server (stage B compares what it serves against them).
        for path, text in zip(parts, part_texts):
            if "\\\"" in text:
                at = text.index("\\\"")
                raise TestFailure(f"{path.name} contains an escape at offset {at}: "
                                  f"{text[max(0, at - 80):at + 120]!r}")
        # `t` is anchored once per match, not per part: a merge that restarted it would make the
        # player's timeline jump backwards. This is the one field a per-hand recorder can break.
        marks = [step.get("t") for step in on_disk["steps"]]
        if marks != sorted(marks):
            raise TestFailure(f"steps[].t must be non-decreasing across hands, got {marks[:40]!r}")
        # 尾部字段取最后一片：整场结束时 `match_over` 必须为 true（每片都带一份尾部，只有最后
        # 一片的 `match_over` 是 true —— 合并取错片的话这里就会是 false）。
        if on_disk.get("match_over") is not True:
            raise TestFailure(f"the merged match must be finished: match_over={on_disk.get('match_over')!r}")
        if on_disk.get("finished") is not True:
            raise TestFailure("the merged match must be marked finished")
        # `totals` 也只能来自最后一片：中途各片的 totals 是那一局当时的累计值。
        match_end = observed.get("match_end") or {}
        rankings = {entry.get("seat"): entry for entry in match_end.get("rankings") or []}
        if rankings:
            for seat in range(4):
                expected_total = (rankings.get(seat) or {}).get("final_score")
                if expected_total is None:
                    continue
                if on_disk["totals"][seat] != expected_total:
                    raise TestFailure(f"merged totals[{seat}]={on_disk['totals'][seat]!r} "
                                      f"!= match_end final_score {expected_total!r}")
                expected_rank = (rankings.get(seat) or {}).get("rank")
                if expected_rank is not None and on_disk["final_rank"][seat] != expected_rank:
                    raise TestFailure(f"merged final_rank[{seat}]={on_disk['final_rank'][seat]!r} "
                                      f"!= match_end rank {expected_rank!r}")

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

        # ---- 2b. 视图轨道必须覆盖四家 ----
        # 这是「切到 AI 视角」唯一的数据来源：没有 endpoint 的座位，服务端从不为它构建
        # state_sync，所以它的私有视图只能从 steps[].v 里读。轨道是按步 delta 编码的
        # （缺席 = 该座位没变），所以要按播放器口径滚动一遍再看，而不是逐条要求四家都非空。
        # 曾经的录制只在 state_sync 步取快照，某些座位整局都没被记到，切过去就是一块空牌桌。
        replay_steps = on_disk.get("steps") or []
        for seat in range(4):
            hand = hand_track_at(on_disk, seat, len(replay_steps))
            if not hand:
                raise TestFailure(
                    f"view track never recorded seat {seat}: switching the replay to that seat "
                    f"would show an empty table (last step v={replay_steps[-1].get('v')!r})")
        # 每局新建录制器意味着视图基线也要重来：新局的第一次快照必须记下四家的视图，否则
        # 播放器在 `game_start` 清空轨道（replay.js）之后会有座位整局空着 —— 只有"该座位的
        # 视图恰好和上一局末尾一样"时才会暴露，所以这里逐局核对，而不是只在结尾看一次。
        for path, text in zip(parts, part_texts):
            part_steps = json.loads(text).get("steps") or []
            for step in part_steps:
                views = step.get("v") or []
                if len(views) >= 4 and all(isinstance(view, dict) and view for view in views[:4]):
                    break
            else:
                raise TestFailure(f"{path.name} never recorded a full four-seat view snapshot; "
                                  "a fresh recorder per hand must not reuse the previous baseline")

        # ---- 3. 取回 + 鉴权（需要账号）----
        skipped = fetch_and_authorize(port, room_id, replay_dir, main_client)
        on_disk_bytes = sum(path.stat().st_size for path in parts)
        summary = (f"replay_e2e_test: ok (room {room_id}, {len(on_disk['steps'])} steps, "
                   f"{len(parts)} parts / {on_disk_bytes} bytes on disk)")
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


def fetch_and_authorize(port: int, room_id: str, replay_dir: str, main_client: Client) -> bool:
    """阶段 B：参与者能取回、异账号与游客被拒。返回 True 表示"没有账号存储，跳过了"。

    鉴权问的是「这个账号在那局里坐过吗」（§4.13），答案来自 `matches` 表而不是回放文件的
    `seats[]` —— v2.6 起文件是 gzip，读不出座位表了。没有 MySQL 时不是"失败"，而是
    "这条链路今天测不了"。
    """
    username, password = unique_account("rp")

    # 0. 用这个账号真打一局，这样 `matches` 里就有它的行。**必须走这条路**：鉴权查的是
    #    「坐过这个房间」，凭空注册一个账号是查不到的。局终写 `matches` 与随后读它是两条
    #    消息，路由键都是 account_id，所以 §4.1 的 FIFO 保证「刚打完立刻取回」能查到。
    participant = Client(port, "participant")
    try:
        logged = register(participant, username, password)
        if logged.get("type") == FRAME_TYPE.ERROR and logged.get("code") == 11:
            return True  # INTERNAL = accounts unavailable
        if logged.get("type") == FRAME_TYPE.ERROR:
            # 同一秒内重跑：账号已存在，直接登录。
            logged = participant.login(username, password)
        if logged.get("type") != FRAME_TYPE.LOGIN_OK:
            raise TestFailure(f"register/login failed: {logged!r}")
        if not str(logged.get("player_id", "")).startswith("u-"):
            raise TestFailure(f"account login must yield a u- player id: {logged!r}")
        own = play_one_round(participant, username, password, already_logged_in=True)
        if own["errors"]:
            raise TestFailure(f"the participant's game answered errors: {own['errors'][:2]!r}")
        own_room = own["room_id"]

        # 参与者取回自己的回放：成功，且还原出来的内容与磁盘上的完全一致。这条断言一次说清
        # 三件事：DB 鉴权放行、gzip+base64 能还原、切片没切坏字节。
        #
        # 先发一次 `stats_request`：局终写 `matches` 是 fire-and-forget，而 `game_end` 是房间
        # 直接下发给座位的，所以「刚看到 game_end 就点回放」可能跑在那一行落库之前。stats 与
        # RecordMatch 都按 account_id 路由到同一个 DbActor 邮箱（§4.1 的 FIFO），所以 stats
        # 一旦回来，`matches` 里的行就一定在了 —— 这也正是真实客户端的顺序：玩家是从战绩列表
        # 点进回放的，那条列表只有在这一行存在之后才会有。
        stats = participant.request_stats()
        if stats.get("type") != FRAME_TYPE.STATS:
            raise TestFailure(f"stats_request failed: {stats!r}")
        served = request_replay(participant, own_room)
        if "error" in served:
            raise TestFailure(f"the participant must be allowed to read its own replay, got {served!r}")
        own_parts = wait_for_parts(replay_dir, own_room)
        on_disk, _ = merge_parts(own_parts, own_room)
        # 服务端下发的是**合并后**的整场文件，不是分片本身：客户端（§5.18）看到的仍然是一份
        # 完整时间轴。这条断言同时守住三件事：DB 鉴权放行、gzip+base64 能还原、分片合并与
        # 磁盘内容一致 —— 分片是服务端内部布局，不能泄漏到线上形状里。
        check_file_shape(served, own_room)
        if json.dumps(served, ensure_ascii=False, sort_keys=True) != json.dumps(
            on_disk, ensure_ascii=False, sort_keys=True
        ):
            raise TestFailure("the served replay differs from the merged parts on disk")

        # 旧布局回退：一份 v2.6 的整场文件（`<room_id>.json.gz`）和一份 v2.5 的明文文件
        # （`<room_id>.json`）都必须仍然能读。分片方案把读路径变成"分片优先"，没有这条回退，
        # 一次升级就会让磁盘上所有已有回放变成 ROOM_NOT_FOUND。
        own_path = Path(replay_dir) / f"{own_room}.json.gz"
        legacy_path = Path(replay_dir) / f"{own_room}.json"
        for path in own_parts:
            path.unlink()
        own_path.write_bytes(gzip.compress(json.dumps(
            on_disk, ensure_ascii=False, separators=(",", ":")).encode("utf-8")))
        legacy_served = request_replay(participant, own_room)
        if "error" in legacy_served:
            raise TestFailure(
                f"a whole-match recording must still be readable, got {legacy_served!r}"
            )
        if json.dumps(legacy_served, ensure_ascii=False, sort_keys=True) != json.dumps(
            on_disk, ensure_ascii=False, sort_keys=True
        ):
            raise TestFailure("the whole-match recording served different content")
        # v2.5：明文（未压缩），服务端必须原样下发并省掉 `encoding`。
        own_path.unlink()
        legacy_path.write_text(json.dumps(on_disk, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        plain_served = request_replay(participant, own_room)
        if "error" in plain_served:
            raise TestFailure(f"a pre-v2.6 plain recording must still be readable, got {plain_served!r}")
        if json.dumps(plain_served, ensure_ascii=False, sort_keys=True) != json.dumps(
            on_disk, ensure_ascii=False, sort_keys=True
        ):
            raise TestFailure("the plain recording served different content")
        legacy_path.unlink()

        # 另一个账号没参与过那局 → 拒绝（同一个 ROOM_NOT_FOUND，不泄露房间是否存在）。
        stranger = Client(port, "stranger")
        try:
            stranger_name, stranger_password = unique_account("rs")
            stranger_logged = register(stranger, stranger_name, stranger_password)
            if stranger_logged.get("type") == FRAME_TYPE.ERROR:
                stranger_logged = stranger.login(stranger_name, stranger_password)
            if stranger_logged.get("type") != FRAME_TYPE.LOGIN_OK:
                raise TestFailure(f"stranger login failed: {stranger_logged!r}")
            refused = request_replay(stranger, own_room)
            if refused.get("error") != 3:  # ROOM_NOT_FOUND
                raise TestFailure(f"an unrelated account must be refused with error 3, got {refused!r}")
        finally:
            stranger.close()
    finally:
        participant.close()

    # 游客永远拿不到回放（account_id 0，服务端连查都不查）：再确认一次这条边界。
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
