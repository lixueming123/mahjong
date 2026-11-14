#!/usr/bin/env python3
"""t5 无头验收测试（SDL_VIDEODRIVER=dummy）。

验证内容（全部在本文件内完成，退出码 0 即通过）：

1. 登录界面渲染 1 帧 → `game/tests/artifacts/client_login.png`
2. 牌桌界面（含操作栏）渲染 1 帧 → `game/tests/artifacts/client_table.png`
3. 帧 → 状态机的机械断言（手牌 / 牌河 / 剩余 / 窗口 / 按钮可见性 / H1 打牌窗口无「过」）
4. 结算界面与打牌窗口渲染不崩溃（不落盘）
5. NDJSON 收发：本地 stub 服务器完成 `login → login_ok → create_room → room_joined → ready →
   game_start`，并断言主循环单次 `update()` 不因网络阻塞

运行：
    ./.venv/bin/python game/tests/client_headless_test.py
"""

from __future__ import annotations

import hashlib
import json
import os
import socket
import subprocess
import sys
import threading
import time
import types

os.environ["SDL_VIDEODRIVER"] = "dummy"
os.environ["SDL_AUDIODRIVER"] = "dummy"
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")

import pygame  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(GAME_DIR)
ARTIFACTS = os.path.join(HERE, "artifacts")
if GAME_DIR not in sys.path:
    sys.path.insert(0, GAME_DIR)

from client import theme as T  # noqa: E402
from client.app import HEARTBEAT_INTERVAL, GameApp  # noqa: E402
from client.game_state import SCREEN_LOBBY, SCREEN_LOGIN, SCREEN_RESULT, SCREEN_TABLE  # noqa: E402
from client.render import actionbar, result as result_view, table as table_view  # noqa: E402
from client.render import widgets as widgets_view  # noqa: E402
from client.tiles import button_label, honor_glyph, is_valid_tile, sort_tiles, tile_label  # noqa: E402
from protocol.enums import (  # noqa: E402
    ACTION,
    ERROR_CODE,
    FRAME_TYPE,
    LIMIT,
    MELD_TYPE,
    REASON,
    WINDOW,
    YAKU,
    value_of,
)

FAILURES: list[str] = []


def check(condition: bool, label: str) -> None:
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {label}")
    if not condition:
        FAILURES.append(label)


def make_app() -> GameApp:
    return GameApp(host="127.0.0.1", port=9527, auto_scale=False)


# ---------------------------------------------------------------- 合成状态

ROOM_JOINED = {
    "type": FRAME_TYPE.ROOM_JOINED,
    "room_id": "r-0001",
    "seat": 0,
    "seats": [
        {"seat": 0, "name": "玩家_", "is_bot": False},
        {"seat": 1, "name": "AI-1", "is_bot": True},
        {"seat": 2, "name": "AI-2", "is_bot": True},
        {"seat": 3, "name": "AI-3", "is_bot": True},
    ],
}

GAME_START = {
    "type": FRAME_TYPE.GAME_START,
    "seat": 0,
    "dealer": 0,
    # v2.0（PROTOCOL v1.6 §5.4）：场次进度；0 基 round_index + 恒 4 的 round_count
    "round_index": 0,
    "round_count": 4,
    # 庄家 14 张（含字牌 1z/1z/5z/7z；按 §2.2 排序：数牌在前、字牌 1z..7z）
    "hand": ["1m", "1m", "2m", "3m", "4m", "5m", "6p", "6p", "7p", "9s",
             "1z", "1z", "5z", "7z"],
}

# 136 口径（UI v1.3 §0.1）：牌山初始 83；自带 2 组副露（数牌碰 + 字牌杠）= 7 张门前手牌
STATE_SYNC = {
    "type": FRAME_TYPE.STATE_SYNC,
    "seq": 5,
    "remaining": 83,
    "current_seat": 1,
    "hand": ["1m", "1m", "2m", "9s", "1z", "5z", "5z"],
    "melds": [
        {"type": MELD_TYPE.PONG, "tiles": ["6p", "6p", "6p"]},
        {"type": MELD_TYPE.KONG, "tiles": ["7z", "7z", "7z", "7z"]},
    ],
    "seats": [
        {"seat": 0, "name": "玩家_", "is_bot": False, "hand_count": 7,
         "discards": ["9s", "4p", "1z"], "melds": [{"type": MELD_TYPE.PONG, "tiles": ["6p", "6p", "6p"]}],
         "disconnected": False},
        {"seat": 1, "name": "AI-1", "is_bot": True, "hand_count": 13,
         "discards": ["3m", "5p", "7z"],
         "melds": [{"type": MELD_TYPE.PONG, "tiles": ["3z", "3z", "3z"]}], "disconnected": False},
        {"seat": 2, "name": "AI-2", "is_bot": True, "hand_count": 11,
         "discards": ["7m", "2s", "2s", "5z"],
         "melds": [{"type": MELD_TYPE.CHOW, "tiles": ["3p", "4p", "5p"]},
                   {"type": MELD_TYPE.KONG, "tiles": ["1z", "1z", "1z", "1z"]}],
         "disconnected": False},
        {"seat": 3, "name": "AI-3", "is_bot": True, "hand_count": 13,
         "discards": ["1s", "6z"],
         "melds": [{"type": MELD_TYPE.KONG, "tiles": ["5z", "5z", "5z", "5z"]}], "disconnected": True},
    ],
}

CLAIM_REQUEST = {
    "type": FRAME_TYPE.ACTION_REQUEST,
    "seq": 6,
    "deadline_ms": int(time.time() * 1000) + 15000,
    "window": WINDOW.CLAIM,
    "options": [
        {"action": ACTION.WIN, "tiles": []},
        {"action": ACTION.PONG, "tiles": ["2s"]},
        {"action": ACTION.PASS, "tiles": []},
    ],
}

DISCARD_REQUEST = {
    "type": FRAME_TYPE.ACTION_REQUEST,
    "seq": 7,
    "deadline_ms": int(time.time() * 1000) + 15000,
    "window": WINDOW.DISCARD,
    "options": [{"action": ACTION.WIN, "tiles": []}],
}

GAME_END = {
    "type": FRAME_TYPE.GAME_END,
    "reason": REASON.WIN_RON,
    # v2.0（PROTOCOL v1.6 §5.10）：局终帧新增字段
    "round_index": 0,
    "dealer": 0,
    "renchan_count": 0,
    "match_will_continue": True,
    "results": [
        {"seat": 0, "delta_score": -4, "match_total": -4, "hand": [], "win_type": []},
        {"seat": 1, "delta_score": 4, "match_total": 4,
         "hand": ["1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p", "6p", "8p", "8p"],
         "win_type": [YAKU.SEVEN_PAIRS, YAKU.WIN_RON]},
        {"seat": 2, "delta_score": 0, "match_total": 0, "hand": [], "win_type": []},
        {"seat": 3, "delta_score": 0, "match_total": 0, "hand": [], "win_type": []},
    ],
    "win": {"seat": 1, "tile": "8p", "from_seat": 0, "tsumo": False,
            "tags": [YAKU.SEVEN_PAIRS], "multiplier": 8, "per_payer": 8, "total": 8,
            "yaku": [{"id": YAKU.MENZEN, "han": 1}, {"id": YAKU.SEVEN_PAIRS, "han": 2}],
            "han": 3,
            "han_breakdown": {"base": 3, "tsumo": 0, "dealer": 0, "total": 3},
            "dealer_bonus": False, "rinshan": False},
}

# v2.0（PROTOCOL v1.6 §5.15）：场终帧（每场恰好 1 次）
MATCH_END = {
    "type": FRAME_TYPE.MATCH_END,
    "hands_played": 5,   # D2 终裁：= game_end 下发手数（4..8，含连庄）
    "renchan_count": 1,
    "rankings": [
        {"seat": 1, "final_score": 24, "rank": 1},
        {"seat": 0, "final_score": -8, "rank": 2},
        {"seat": 3, "final_score": -8, "rank": 2},   # 并列跳号：两行「第 2 名」，无第 3 名
        {"seat": 2, "final_score": -8, "rank": 2},
    ],
}

# 战绩用例（ACCOUNT_DESIGN §3.4 / UI_UX_SPEC v1.7）：四个汇总数字 + 最近 ≤10 局，倒序。
STATS_FRAME = {
    "type": FRAME_TYPE.STATS,
    "games": 12,
    "wins": 5,
    "draws": 3,
    "net_score": -14,
    "recent": [
        {"room_id": f"r-{1000 + i:04d}", "reason": reason, "delta_score": delta, "win": delta > 0,
         "ended_at": 1759900000 + i * 600}
        for i, (reason, delta) in enumerate([
            (REASON.WIN_RON, 12), (REASON.DRAW_EXHAUSTED, 0), (REASON.WIN_TSUMO, 16), (REASON.WIN_RON, -1), (REASON.WIN_RON, -1),
            (REASON.DRAW_EXHAUSTED, 0), (REASON.WIN_RON, -1), (REASON.WIN_TSUMO, 32), (REASON.WIN_RON, -1), (REASON.DRAW_EXHAUSTED, 0),
        ])
    ],
}

# ------------------------------------------------------------------ 字牌 fixtures
# 结算用例：和牌者 = seat 1，含**字牌杠**（1z×4）、**字牌刻子**（2z×3）+ **字牌做将**（5z×2）
# 物理持有 = 14 + 杠数 1 = 15 张（PROTOCOL §5.10 / UI §9.3）
HONOR_WIN_HAND = ["7m", "7m", "7m", "3p", "4p", "5p",
                  "1z", "1z", "1z", "1z", "2z", "2z", "2z", "5z", "5z"]

HONOR_MELDS_SEAT1 = [
    {"type": MELD_TYPE.KONG, "tiles": ["1z", "1z", "1z", "1z"]},
    {"type": MELD_TYPE.PONG, "tiles": ["2z", "2z", "2z"]},
    {"type": MELD_TYPE.CHOW, "tiles": ["3p", "4p", "5p"]},
    {"type": MELD_TYPE.PONG, "tiles": ["7m", "7m", "7m"]},
]

HONOR_STATE_SYNC = {
    "type": FRAME_TYPE.STATE_SYNC,
    "seq": 9,
    "remaining": 40,
    "current_seat": 1,
    "hand": ["1m", "1m", "2m", "9s", "1z", "5z", "5z"],
    "melds": [
        {"type": MELD_TYPE.PONG, "tiles": ["6p", "6p", "6p"]},
        {"type": MELD_TYPE.KONG, "tiles": ["7z", "7z", "7z", "7z"]},
    ],
    "seats": [
        {"seat": 0, "name": "玩家_", "is_bot": False, "hand_count": 7,
         "discards": ["9s", "4p", "1z"],
         "melds": [{"type": MELD_TYPE.PONG, "tiles": ["6p", "6p", "6p"]},
                   {"type": MELD_TYPE.KONG, "tiles": ["7z", "7z", "7z", "7z"]}],
         "disconnected": False},
        {"seat": 1, "name": "AI-1", "is_bot": True, "hand_count": 2,
         "discards": ["5z"], "melds": HONOR_MELDS_SEAT1, "disconnected": False},
        {"seat": 2, "name": "AI-2", "is_bot": True, "hand_count": 13,
         "discards": [], "melds": [], "disconnected": False},
        {"seat": 3, "name": "AI-3", "is_bot": True, "hand_count": 13,
         "discards": [], "melds": [], "disconnected": False},
    ],
}

HONOR_GAME_END = {
    "type": FRAME_TYPE.GAME_END,
    "reason": REASON.WIN_RON,
    "results": [
        {"seat": 0, "delta_score": -16, "hand": [], "win_type": []},
        {"seat": 1, "delta_score": 16, "hand": HONOR_WIN_HAND, "win_type": [YAKU.WIN_RON]},
        {"seat": 2, "delta_score": 0, "hand": [], "win_type": []},
        {"seat": 3, "delta_score": 0, "hand": [], "win_type": []},
    ],
    "win": {"seat": 1, "tile": "5z", "from_seat": 0, "tsumo": False,
            "tags": [], "multiplier": 1, "per_payer": 16, "total": 16},
}

# 字牌的 6 档冻结字号（UI_UX_SPEC v1.3 §4.4「字牌字体」列）与对应牌位尺寸
HONOR_SIZE_SLOTS = [
    ("本家手牌", T.FS_TILE_Z_HAND, (T.HAND_TILE_W, T.HAND_TILE_H)),
    ("中央牌河", T.FS_TILE_Z_RIVER_CENTER, T.RIVER_CENTER_TILE),
    ("左右牌河", T.FS_TILE_Z_RIVER_SIDE, T.RIVER_SIDE_TILE),
    ("副露", T.FS_TILE_Z_MELD, T.MELD_TILE),
    ("结算和牌张", T.FS_TILE_Z_RESULT, T.RESULT_TILE),
    ("结算手牌", T.FS_TILE_Z_RESULT_HAND, T.RESULT_HAND_TILE),
]


def build_table_state(app: GameApp) -> None:
    app.now = time.monotonic()
    app.state.apply(GAME_START)
    app._cache_room_joined(ROOM_JOINED)
    app.state.apply(STATE_SYNC)
    app.state.apply(CLAIM_REQUEST)
    app.now = time.monotonic()


# ---------------------------------------------------------------- stub 服务器


class StubServer(threading.Thread):
    """最小 NDJSON 服务端。

    `enforce_login=True`（默认）时按 PROTOCOL.md §4.1 复刻真实网关的关键行为：
    **登录登记完成（服务端已回 `login_ok`）之前收到任何其它帧 → 回 `error{NOT_LOGGED_IN}`**。
    注意这里刻意不用「收到 login 帧」作为判据：真实网关中 `login` 被**接收**与**生效**
    （登记完成、可接受其它帧）之间存在窗口，captain 实测的 NOT_LOGGED_IN 正落在该窗口内
    （客户端在同一 tick 里先发 login 后发 ping）。这是 t28 竞态用例能真正捕获该缺陷的前提。
    收到 `ping` 时回 `pong`（用于登录后 RTT 断言）。
    """

    def __init__(self, enforce_login: bool = True, auto_pong: bool = True) -> None:
        super().__init__(daemon=True, name="stub-server")
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(5.0)
        self.port = self.listener.getsockname()[1]
        self.conn: socket.socket | None = None
        self.received: list[dict] = []
        self.raw_lines: list[bytes] = []
        self.enforce_login = enforce_login
        self.auto_pong = auto_pong
        self.logged_in = False
        self.login_received = False
        self.pre_login_frames: list[int] = []   # 登录登记完成前收到的非 login 帧（应为空）
        self._stop = threading.Event()

    def run(self) -> None:
        try:
            self.conn, _ = self.listener.accept()
        except (OSError, socket.timeout):
            return
        self.conn.settimeout(0.2)
        buffer = b""
        while not self._stop.is_set():
            try:
                chunk = self.conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                self.raw_lines.append(line + b"\n")
                try:
                    frame = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    frame = {"type": "<bad-frame>"}
                self.received.append(frame)
                self._react(frame)

    def _react(self, frame: dict) -> None:
        """按 PROTOCOL §4.1 语义回应（登录登记前保护 + pong）。"""
        ftype = frame.get("type")
        if ftype == FRAME_TYPE.LOGIN:
            self.login_received = True     # 仅表示「收到」，登录登记以发出 login_ok 为准
            return
        if not self.logged_in:
            self.pre_login_frames.append(ftype)
            if self.enforce_login:
                self.send({"type": FRAME_TYPE.ERROR, "code": ERROR_CODE.NOT_LOGGED_IN, "message": "send login first"})
            return
        if ftype == FRAME_TYPE.PING and self.auto_pong:
            self.send({"type": FRAME_TYPE.PONG})

    def send(self, frame: dict) -> None:
        if frame.get("type") == FRAME_TYPE.LOGIN_OK:
            self.logged_in = True          # 登录登记完成（与服务端回 login_ok 等价）
        if self.conn is None:
            return
        try:
            self.conn.sendall((json.dumps(frame, ensure_ascii=False) + "\n").encode("utf-8"))
        except OSError:
            pass

    def types(self) -> list[int]:
        return [f.get("type") for f in list(self.received)]

    def type_count(self, ftype: int) -> int:
        return sum(1 for f in list(self.received) if f.get("type") == ftype)

    def wait_for(self, ftype: int, timeout: float = 3.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if ftype in self.types():
                return True
            time.sleep(0.01)
        return False

    def close(self) -> None:
        self._stop.set()
        for sock in (self.conn, self.listener):
            try:
                if sock is not None:
                    sock.close()
            except OSError:
                pass


def pump(app: GameApp, seconds: float, until=None, max_step: list[float] | None = None) -> None:
    """驱动主循环的 update（不渲染），并记录单步耗时用于「不阻塞」断言。"""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        start = time.monotonic()
        app.now = start
        app.update()
        if max_step is not None:
            max_step.append(time.monotonic() - start)
        if until is not None and until():
            return
        time.sleep(0.01)


def _legacy_heartbeat_ping(app: GameApp, now: float) -> None:
    """**修复前** `GameApp._heartbeat` 的 ping 分支（逐字保留，仅用于反事实复现断言）。

    原实现只判断 `connected`，而 `network` 一连上即为真、`last_ping` 初始为 None，
    于是**第一次心跳 tick 立即发 ping** —— 请求/响应落在 `connect()` 与
    `send_frame(FRAME_TYPE.LOGIN)` 之间的十几毫秒窗口内，服务端按 PROTOCOL §4.1 回
    `NOT_LOGGED_IN`，破坏 GD-C1「全程零 error 帧」。
    """
    if not app.connected or app.net is None:
        return
    if app.last_ping is None or now - app.last_ping >= HEARTBEAT_INTERVAL:
        app.last_ping = now
        app.ping_sent_at = now
        app.send_frame(FRAME_TYPE.PING)


def _free_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def _wait_port(port: int, timeout: float = 15.0) -> bool:
    """等待真实服务端开始监听。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def _sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _measure_frame_ms(app: GameApp, frames: int = 30, warmup: int = 3) -> list[float]:
    """测量「update + render 一轮」的单帧耗时（毫秒），返回升序列表。

    预热 `warmup` 帧并在测量期间暂停 GC：字体缓存首次构建、smoothscale 首次分配、
    GC 停顿等一次性开销不应被计入渲染性能（否则机器负载会造成假失败）。
    """
    import gc

    for _ in range(warmup):
        app.now = time.monotonic()
        app.update()
        app.render_frame(present=True)
    gc.collect()
    samples: list[float] = []
    gc.disable()
    try:
        for _ in range(frames):
            t0 = time.perf_counter()
            app.now = time.monotonic()
            app.update()
            app.render_frame(present=True)
            samples.append((time.perf_counter() - t0) * 1000.0)
    finally:
        gc.enable()
    samples.sort()
    return samples


def handshake(app: GameApp, server: "StubServer", step_times: list[float]) -> None:
    """按 PROTOCOL §9.1 完成 login → create_room → ready → game_start。

    `start_game()` 是「再来一局」和这条旧握手保留的路径：`create_room` 之后自动 `ready`。
    大厅按钮「新建房间」不走这里，也不会自动 ready。
    """
    app.connect()
    pump(app, 3.0, until=lambda: server.wait_for(FRAME_TYPE.LOGIN, 0.01), max_step=step_times)
    server.send({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "玩家_"})
    pump(app, 2.0, until=lambda: app.state.screen == SCREEN_LOBBY, max_step=step_times)
    check(app.state.screen == SCREEN_LOBBY, "login_ok → 停在大厅（v1.8 §12）")
    check(not server.wait_for(FRAME_TYPE.CREATE_ROOM, 0.2), "大厅里没有玩家指令前不发 create_room")
    app.start_game()
    pump(app, 2.0, until=lambda: server.wait_for(FRAME_TYPE.CREATE_ROOM, 0.01), max_step=step_times)
    server.send(ROOM_JOINED)
    pump(app, 2.0, until=lambda: server.wait_for(FRAME_TYPE.READY, 0.01), max_step=step_times)
    server.send(GAME_START)
    pump(app, 2.0, until=lambda: app.state.screen == SCREEN_TABLE, max_step=step_times)


# ---------------------------------------------------------------- 主流程


def main() -> int:
    os.makedirs(ARTIFACTS, exist_ok=True)
    app = make_app()

    print("[1] 登录界面渲染")
    login_path = os.path.join(ARTIFACTS, "client_login.png")
    app.now = time.monotonic()
    app.nickname = "玩家_"
    login_surface = app.render_frame(present=True)
    import pygame

    pygame.image.save(login_surface, login_path)
    check(os.path.getsize(login_path) > 1000, f"登录界面 PNG 已保存：{login_path}")
    check(
        login_surface.get_size() == (T.LOGICAL_W, T.LOGICAL_H),
        f"逻辑分辨率 = {T.LOGICAL_W}×{T.LOGICAL_H}",
    )

    print("[2] 牌桌 + 操作栏渲染（claim 窗口）")
    build_table_state(app)
    check(app.state.screen == SCREEN_TABLE, "game_start 后进入牌桌界面")
    check(len(app.state.hand) == 7, f"state_sync 覆盖手牌：len={len(app.state.hand)}（13 − 3×2 组副露）")
    check(not app.state.hand_warning, "手牌张数交叉校验通过（len(hand) + 3×melds = 13）")
    check(
        [m.type for m in app.state.melds] == [MELD_TYPE.PONG, MELD_TYPE.KONG],
        f"本家副露分组可还原（含字牌杠 4 张）：{[m.type for m in app.state.melds]}",
    )
    check(app.state.remaining == 83, f"牌山剩余 = {app.state.remaining}（136 口径）")
    check(app.state.current_seat == 1, f"当前行动座位 = {app.state.current_seat}")
    check(
        len(app.state.seat_view(2).melds) == 2 and app.state.seat_view(2).melds[0].type == MELD_TYPE.CHOW,
        "他家副露分组可还原（chow）",
    )
    check(
        not any(hasattr(v, "hand_tiles") for v in app.state.seats),
        "SeatView 无任何他家牌面字段（私有信息隔离）",
    )
    buttons = actionbar.build_buttons(app.state, app._remaining_ms())
    kinds = [b.kind for b in buttons]
    check(kinds == [ACTION.WIN, ACTION.PONG, ACTION.PASS], f"claim 窗口按钮 = {kinds}（打牌/杠/吃 不可见）")
    check(all(b.enabled for b in buttons), "claim 窗口按钮均可点击")

    table_path = os.path.join(ARTIFACTS, "client_table.png")
    app.now = time.monotonic()
    table_surface = app.render_frame(present=True)
    pygame.image.save(table_surface, table_path)
    check(os.path.getsize(table_path) > 1000, f"牌桌界面 PNG 已保存：{table_path}")

    print("[2b] 日麻赤 5 在牌桌上的观感（贴图染色，不是程序化重绘）")
    aka_app = make_app()
    aka_app.state.apply(ROOM_JOINED)
    aka_app.state.apply(dict(GAME_START, mode="riichi",
                             points=[25000, 25000, 24800, 25200],
                             hand=["0m", "5m", "1m", "2m", "3m", "0p",
                                   "5p", "6p", "0s", "5s", "7s", "1z", "1z"]))
    aka_app.state.apply({
        "type": FRAME_TYPE.STATE_SYNC,
        "seq": 9,
        "remaining": 45,
        "current_seat": 0,
        "hand": ["0m", "5m", "1m", "2m", "3m", "0p", "5p", "6p", "0s", "5s", "7s", "1z", "1z"],
        "melds": [],
        "points": [25000, 25000, 24800, 25200],
        "dora_indicators": ["3p"],
        "seats": [
            {"seat": 0, "name": "玩家_", "is_bot": False, "hand_count": 13,
             "discards": ["0p", "5s", "1z"], "melds": [], "disconnected": False},
            {"seat": 1, "name": "AI-1", "is_bot": True, "hand_count": 13,
             "discards": ["3m", "0s", "7z"], "melds": [], "disconnected": False},
            {"seat": 2, "name": "AI-2", "is_bot": True, "hand_count": 13,
             "discards": ["7m", "2s", "0m"], "melds": [], "disconnected": False},
            {"seat": 3, "name": "AI-3", "is_bot": True, "hand_count": 13,
             "discards": ["1s", "5m"], "melds": [], "disconnected": False},
        ],
    })
    aka_path = os.path.join(ARTIFACTS, "client_table_aka.png")
    aka_app.now = time.monotonic()
    pygame.image.save(aka_app.render_frame(present=False), aka_path)
    check(os.path.getsize(aka_path) > 1000, f"日麻赤 5 牌桌 PNG 已保存：{aka_path}")

    print("[2c] 场况卡玩法徽标：经典房 / 日麻房一眼可分（v1.22）")
    # 玩法不在协议里（game_start 无 mode 键，classic 帧逐字节冻结），只能靠日麻独有字段反推。
    check(app.state.mode == "classic", f"classic game_start（无 points）⇒ state.mode = {app.state.mode!r}")
    check(table_view.hud_mode_label(app.state) == "经典", "经典房徽标文案 = 「经典」")
    check(aka_app.state.mode == "riichi", f"日麻 game_start（带 points）⇒ state.mode = {aka_app.state.mode!r}")
    check(table_view.hud_mode_label(aka_app.state) == "日麻", "日麻房徽标文案 = 「日麻」")
    mode_path = os.path.join(ARTIFACTS, "client_hud_mode.png")
    app.now = time.monotonic()
    pygame.image.save(app.render_frame(present=False).subsurface((0, 0, 260, 140)).copy(), mode_path)
    check(os.path.getsize(mode_path) > 500, f"场况卡（经典徽标）PNG 已保存：{mode_path}")

    mode_riichi_path = os.path.join(ARTIFACTS, "client_hud_mode_riichi.png")
    aka_app.now = time.monotonic()
    pygame.image.save(
        aka_app.render_frame(present=False).subsurface((0, 0, 260, 140)).copy(), mode_riichi_path
    )
    check(os.path.getsize(mode_riichi_path) > 500, f"场况卡（日麻徽标）PNG 已保存：{mode_riichi_path}")

    print("[3] 打牌窗口：手牌可点选、无「过」按钮（H1）")
    app.state.apply(DISCARD_REQUEST)
    app.now = time.monotonic()
    kinds = [b.kind for b in actionbar.build_buttons(app.state, app._remaining_ms())]
    check(kinds == [FRAME_TYPE.DISCARD, ACTION.WIN], f"discard 窗口按钮 = {kinds}")
    rects = table_view.hand_layout(app.state)
    check(len(rects) == len(app.state.hand), "手牌区逐张计算 rect")
    hit = table_view.hand_hit_test(app.state, (rects[0][0] + 2, rects[0][1] + 2))
    check(hit == 0, f"手牌命中测试：点第一张 → index={hit}")
    app.state.selected_index = 0
    app.render_frame(present=False)
    remaining = app._remaining_ms()
    check(remaining is not None and remaining > 0, f"倒计时剩余 = {remaining:.0f} ms")
    check(
        actionbar.countdown_color(15000) == T.POS and actionbar.countdown_color(5000) == T.WARN
        and actionbar.countdown_color(1000) == T.NEG and actionbar.countdown_color(0) == T.NEG,
        "倒计时颜色分级：>10s 绿 / 3-10s 黄 / <3s 红 / 0 红",
    )

    print("[3b] 立直：按钮直接可点 → 宣言模式标记能打的牌 → 点高亮牌即宣言")
    riichi_app = make_app()
    riichi_app.state.apply(ROOM_JOINED)
    riichi_app.state.apply(
        dict(GAME_START, mode="riichi", points=[25000, 25000, 25000, 25000])
    )
    riichi_app.state.apply({
        "type": FRAME_TYPE.ACTION_REQUEST,
        "seq": 8,
        "deadline_ms": int(time.time() * 1000) + 15000,
        "window": WINDOW.DISCARD,
        "options": [{"action": ACTION.RIICHI, "tiles": []}],
        # 服务端下发「能作宣言牌」的牌面；协议里恒写普通牌面码（赤 5 也写 5m）
        "riichi_tiles": ["1m", "2m", "0p"],
    })
    riichi_app.now = time.monotonic()
    window = riichi_app.state.window
    check(
        window is not None and window.riichi_tiles == ["1m", "2m", "5p"],
        f"解析 riichi_tiles（0p 归一为 5p）：{getattr(window, 'riichi_tiles', None)}",
    )
    specs = actionbar.build_buttons(riichi_app.state, riichi_app._remaining_ms())
    riichi_spec = next((b for b in specs if b.kind == ACTION.RIICHI), None)
    check(riichi_spec is not None, "日麻打牌窗口的 riichi 选项生成「立直」按钮")
    check(
        riichi_spec is not None and riichi_spec.enabled,
        "立直按钮**不需要先选牌**就点亮（v1.21：点它进入宣言模式）",
    )
    assert riichi_spec is not None

    sent: list[tuple[int, dict]] = []

    def _record_send(kind, **kwargs):
        sent.append((kind, kwargs))
        return True

    riichi_app.send_frame = _record_send  # type: ignore[assignment]

    riichi_app.toast_text = ""
    riichi_app._table_click((riichi_spec.rect[0] + 4, riichi_spec.rect[1] + 4))
    check(riichi_app.state.riichi_arming, "点「立直」进入宣言模式（手牌开始标记）")
    check(not sent and not riichi_app.state.pending, "进入宣言模式本身不发帧")
    check("高亮" in riichi_app.toast_text, f"点立直后的提示：{riichi_app.toast_text!r}")
    hint_text, _, _ = actionbar._status_text(riichi_app.state, riichi_app.build_ui())
    check("高亮的牌" in hint_text, f"宣言模式的状态提示：{hint_text!r}")

    rects = table_view.hand_layout(riichi_app.state)
    check(riichi_app.state.hand[3] == "3m", f"测试手牌 index=3 应为 3m，实际 {riichi_app.state.hand[3]}")
    riichi_app.toast_text = ""
    riichi_app._table_click((rects[3][0] + 2, rects[3][1] + 2))
    check(
        not sent and not riichi_app.state.pending and "不听牌" in riichi_app.toast_text,
        f"宣言模式点非候选牌只提示、不发帧：{riichi_app.toast_text!r}",
    )
    riichi_app._table_click((rects[0][0] + 2, rects[0][1] + 2))
    check(
        bool(sent) and sent[0][0] == FRAME_TYPE.ACTION and sent[0][1].get("action") == ACTION.RIICHI
        and sent[0][1].get("tiles") == [riichi_app.state.hand[0]],
        f"点高亮的牌即宣言并打出它：{sent[:1]}",
    )
    check(riichi_app.state.pending and riichi_app.state.pending_label == "立直", "宣言后进入 PENDING「已提交：立直」")

    # 老服务端（v1.20 及以前）不下发 riichi_tiles：必须退回「先选牌再立直」，且不做标记
    legacy_app = make_app()
    legacy_app.state.apply(ROOM_JOINED)
    legacy_app.state.apply(dict(GAME_START, mode="riichi", points=[25000, 25000, 25000, 25000]))
    legacy_app.state.apply({
        "type": FRAME_TYPE.ACTION_REQUEST,
        "seq": 11,
        "deadline_ms": int(time.time() * 1000) + 15000,
        "window": WINDOW.DISCARD,
        "options": [{"action": ACTION.RIICHI, "tiles": []}],
    })
    legacy_app.now = time.monotonic()
    legacy_spec = next(
        b for b in actionbar.build_buttons(legacy_app.state, legacy_app._remaining_ms())
        if b.kind == ACTION.RIICHI
    )
    legacy_app.toast_text = ""
    legacy_app._table_click((legacy_spec.rect[0] + 4, legacy_spec.rect[1] + 4))
    check(
        not legacy_app.state.riichi_arming and "先点选" in legacy_app.toast_text,
        f"老服务端（无 riichi_tiles）退回「先选牌」：{legacy_app.toast_text!r}",
    )

    # 再开一个窗口用于出图：宣言模式 + 候选标记 + 其余牌压暗
    riichi_app.state.apply({
        "type": FRAME_TYPE.ACTION_REQUEST,
        "seq": 9,
        "deadline_ms": int(time.time() * 1000) + 15000,
        "window": WINDOW.DISCARD,
        "options": [{"action": ACTION.RIICHI, "tiles": []}],
        "riichi_tiles": ["1m", "2m", "5m"],
    })
    riichi_app.state.riichi_arming = True
    riichi_png = os.path.join(ARTIFACTS, "client_riichi_prompt.png")
    riichi_app.now = time.monotonic()
    pygame.image.save(riichi_app.render_frame(present=False), riichi_png)
    check(os.path.getsize(riichi_png) > 1000, f"立直标记产物：{riichi_png}")
    # 服务端仍可能拒绝（例如振听口径差异）→ 错误文案必须中文化，不能只留英文 message
    riichi_app._on_error(
        {"code": ERROR_CODE.ILLEGAL_ACTION, "message": "cannot declare riichi now"}, time.monotonic()
    )
    check(
        "换一张" in (riichi_app.error_message or ""),
        f"立直被拒：错误条中文化 = {riichi_app.error_message!r}",
    )
    check("不听牌" in riichi_app.toast_text, f"立直被拒：同时弹 toast = {riichi_app.toast_text!r}")

    print("[3c] 喰い替え禁止（v2.5 §12.15）：压暗的牌点不动、不发帧")
    # 刚碰完 1m 当巡：禁区 = {1m}，手牌里 index 0/1 都是 1m。
    kuikae_app = make_app()
    kuikae_app.state.apply(ROOM_JOINED)
    kuikae_app.state.apply(GAME_START)
    kuikae_app.state.apply({
        "type": FRAME_TYPE.ACTION_REQUEST,
        "seq": 12,
        "deadline_ms": int(time.time() * 1000) + 15000,
        "window": WINDOW.DISCARD,
        "options": [{"action": ACTION.WIN, "tiles": []}],
        # 服务端只写「牌面」，赤 5 也写 5m（与 riichi_tiles 同口径）
        "forbidden_discards": ["1m", "0p"],
    })
    kuikae_app.now = time.monotonic()
    window = kuikae_app.state.window
    check(
        window is not None and window.forbidden_discards == ["1m", "5p"],
        f"解析 forbidden_discards（0p 归一为 5p）：{getattr(window, 'forbidden_discards', None)}",
    )
    hint_text, _, _ = actionbar._status_text(kuikae_app.state, kuikae_app.build_ui())
    check("压暗" in hint_text, f"有禁区时的状态提示：{hint_text!r}")

    sent: list[tuple[int, dict]] = []

    def _record_send(kind, **kwargs):
        sent.append((kind, kwargs))
        return True

    kuikae_app.send_frame = _record_send  # type: ignore[assignment]
    rects = table_view.hand_layout(kuikae_app.state)
    check(kuikae_app.state.hand[0] == "1m", f"测试手牌 index=0 应为 1m，实际 {kuikae_app.state.hand[0]}")
    kuikae_app.toast_text = ""
    kuikae_app._table_click((rects[0][0] + 2, rects[0][1] + 2))
    check(
        not sent and kuikae_app.state.selected_index is None and "喰い替え" in kuikae_app.toast_text,
        f"点禁区牌只提示、不选中也不发帧：selected={kuikae_app.state.selected_index}",
    )
    # 数字键同口径。
    kuikae_app.toast_text = ""
    kuikae_app._table_key(pygame.event.Event(pygame.KEYDOWN, key=pygame.K_1))
    check(
        kuikae_app.state.selected_index is None and "喰い替え" in kuikae_app.toast_text,
        f"数字键选禁区牌同样被拦：{kuikae_app.toast_text!r}",
    )
    # 非禁区牌照常可选、可打。
    kuikae_app.toast_text = ""
    kuikae_app._table_click((rects[2][0] + 2, rects[2][1] + 2))
    check(kuikae_app.state.selected_index == 2, f"非禁区牌照常选中：index={kuikae_app.state.selected_index}")
    kuikae_app._table_click((rects[2][0] + 2, rects[2][1] + 2))
    check(
        bool(sent) and sent[0][0] == FRAME_TYPE.DISCARD and sent[0][1].get("tile") == kuikae_app.state.hand[2],
        f"非禁区牌照常发出 discard：{sent[:1]}",
    )
    kuikae_png = os.path.join(ARTIFACTS, "client_kuikae_ban.png")
    kuikae_app.now = time.monotonic()
    pygame.image.save(kuikae_app.render_frame(present=False), kuikae_png)
    check(os.path.getsize(kuikae_png) > 1000, f"喰い替え压暗产物：{kuikae_png}")

    print("[4] 结算界面渲染（不落盘）")
    app.state.apply(GAME_END)
    check(app.state.screen == SCREEN_RESULT, "game_end 后进入结算界面")
    groups, concealed = result_view.group_winner_hand(app.state, 1)
    check(len(groups) == 0 and len(concealed) == 14, "结算手牌分组：无副露 → 扁平 14 张")
    app.now = time.monotonic()
    app.render_frame(present=False)

    print("[5] 字牌 1z..7z：编码 / 字形 / 排序 / 渲染 / 副露 / 结算")
    check(is_valid_tile("1z") and is_valid_tile("7z"), "字牌编码 1z/7z 合法（PROTOCOL §2.1 合取正则）")
    check(
        not any(is_valid_tile(t) for t in ("8z", "0z", "zz", "1Z", "z")),
        "越界字牌编码 8z/0z/zz/1Z/z 一律非法",
    )
    glyphs = "".join(honor_glyph(f"{i}z") or "?" for i in range(1, 8))
    check(glyphs == "東南西北白發中", f"繁體字形映射（UI v1.3 §1.3 冻结）= {glyphs}")
    check(
        sort_tiles(["7z", "1z", "5z", "1m", "9s", "3z", "2p"]) == ["1m", "2p", "9s", "1z", "3z", "5z", "7z"],
        "本地排序 = 数牌在前（m<p<s）、字牌 1z..7z 升序（PROTOCOL §2.2）",
    )

    # v4.2：牌面改为 CC0 外部贴图；6 档尺寸都必须加载到真实牌面，不得为空/占位
    # v1.18：+3 张赤 5（`0m`/`0p`/`0s`）
    face_dir = widgets_view._TILE_ASSET_DIR
    asset_count = len(list(face_dir.glob("*.png")))
    check(asset_count == 38, f"CC0 牌面贴图齐全：34 种牌 + 3 张赤 5 + 牌背，共 {asset_count} 张")
    # v5.3：back.png 自带大红 (255,55,55)，必须统一染成 T.BACK 橙金；
    # 暗杠占位 / 3D 牌墙顶 / 旋转牌背都走这条路径，别再漏掉染色。
    back_surf = widgets_view.tile_back_surface(28, 36, plain=True)
    back_pixel = back_surf.get_at((14, 18))[:3] if back_surf is not None else (0, 0, 0)
    check(
        all(abs(channel - target) <= 12 for channel, target in zip(back_pixel, T.BACK)),
        f"牌背贴图染色为橙金 {back_pixel} ≈ {T.BACK}（不得用素材原色 255,55,55）",
    )
    ink_ok, slots_note = True, []
    for label, size, (tw, th) in HONOR_SIZE_SLOTS:
        tile_surf = pygame.Surface((tw, th))
        tile_surf.fill((0, 0, 0))
        widgets_view.draw_tile(tile_surf, app.fonts, (0, 0, tw, th), "1z", honor_size=size)
        pixels = [tile_surf.get_at((x, y))[:3] for x in range(tw) for y in range(th)]
        ink = sum(1 for p in pixels if max(p) > 25)
        if ink <= 20:
            ink_ok = False
            slots_note.append(f"{label}@{size}px 非空像素={ink}")
    check(ink_ok, f"6 档字号均加载到 CC0 字牌贴图 {slots_note or ''}")

    # 字牌在桌面上不被过滤：手牌 / 牌河 / 四家副露（横排与竖排）
    check(all(t in app.state.hand for t in ("1z", "5z")), f"字牌保留在本家手牌：{app.state.hand}")
    check("1z" in app.state.seat_view(0).discards, f"字牌保留在本家牌河：{app.state.seat_view(0).discards}")
    check("7z" in app.state.seat_view(1).discards, f"字牌保留在他家牌河：{app.state.seat_view(1).discards}")
    check(
        [len(m.tiles) for m in app.state.melds] == [3, 4],
        f"本家副露 = 数牌碰 3 张 + 字牌杠 4 张 {[m.tiles for m in app.state.melds]}",
    )
    seat1_meld = app.state.seat_view(1).melds[0]
    seat2_melds = app.state.seat_view(2).melds
    seat3_meld = app.state.seat_view(3).melds[0]
    check(
        len(seat1_meld.tiles) == 3 and all(t.endswith("z") for t in seat1_meld.tiles),
        f"下家（右列竖排）字牌刻子 3 张：{seat1_meld.tiles}",
    )
    check(
        len(seat3_meld.tiles) == 4 and all(t.endswith("z") for t in seat3_meld.tiles),
        f"上家（左列竖排）字牌杠 4 张：{seat3_meld.tiles}",
    )
    check(
        any(len(m.tiles) == 4 and all(t.endswith("z") for t in m.tiles) for m in seat2_melds),
        f"对家（横排）字牌杠 4 张：{[m.tiles for m in seat2_melds]}",
    )

    # 副露布局不溢出：本家横排、对家两列网格（v5.3）、侧家一组一行竖排（v5.1）
    overflows: list[str] = []
    for area, melds, vertical, per_row, tag in (
        (T.R_SELF_MELD, app.state.melds, False, 0, "本家横排"),
        (T.R_OPP_MELD, seat2_melds, False, T.OPP_MELD_GROUPS_PER_ROW, "对家两列网格"),
        (T.R_SIDE_L_MELD, app.state.seat_view(3).melds, True, 0, "上家竖排"),
        (T.R_SIDE_R_MELD, app.state.seat_view(1).melds, True, 0, "下家竖排"),
    ):
        for group_rect, tiles in table_view.meld_layout(
            area, melds, vertical=vertical, groups_per_row=per_row,
        ):
            if group_rect.x < area[0] or group_rect.right > area[0] + area[2]:
                overflows.append(f"{tag}组框 {group_rect}")
            for (tx, ty, tw, th), face in tiles:
                if tx < area[0] or tx + tw > area[0] + area[2] or ty < area[1] or ty + th > area[1] + area[3]:
                    overflows.append(f"{tag}{face}@({tx},{ty},{tw},{th})")
    check(not overflows, f"字牌副露 3 张 / 4 张布局内均不溢出 {overflows or ''}")

    # v5.1：侧家副露一组一行竖排 —— 即使吃满 4 组，也必须留在牌河外侧、不越界。
    # 旧版把 4 组全压一行，pitch 触到 16px 下限后横向溢出，正好压上中央牌河。
    four_melds = [
        types.SimpleNamespace(tiles=["m1", "m2", "m3"]),
        types.SimpleNamespace(tiles=["p5", "p5", "p5"]),
        types.SimpleNamespace(tiles=["s7", "s8", "s9"]),
        types.SimpleNamespace(tiles=["z1", "z1", "z1", "z1"]),
    ]
    side_intrusions: list[str] = []
    for area, river, tag, left in (
        (T.R_SIDE_L_MELD, T.R_SIDE_L_RIVER, "上家", True),
        (T.R_SIDE_R_MELD, T.R_SIDE_R_RIVER, "下家", False),
    ):
        groups = table_view.meld_layout(area, four_melds, vertical=True)
        if len(groups) != 4:
            side_intrusions.append(f"{tag}只排了 {len(groups)} 行")
        for group_rect, tiles in groups:
            if left and group_rect.right >= river[0]:
                side_intrusions.append(f"{tag}组框右沿 {group_rect.right} ≥ 牌河左沿 {river[0]}")
            if not left and group_rect.x <= river[0] + river[2]:
                side_intrusions.append(f"{tag}组框左沿 {group_rect.x} ≤ 牌河右沿 {river[0] + river[2]}")
            if group_rect.bottom > area[1] + area[3]:
                side_intrusions.append(f"{tag}组框下沿 {group_rect.bottom} 超出副露区")
    check(not side_intrusions, f"侧家 4 组副露竖排不侵入牌河 {side_intrusions or ''}")

    # v5.3：对家副露「每行最多 2 组、多的换行」——4 组不溢出，也不压别家副露 / 牌河 / 中央板。
    # 旧区域 (756,168,300,32) 与下家副露列 (918,176,124,160) 相交，横排 4 组还会缩到糊掉。
    opp_groups = table_view.meld_layout(
        T.R_OPP_MELD, four_melds, vertical=False,
        groups_per_row=T.OPP_MELD_GROUPS_PER_ROW,
    )
    opp_intrusions: list[str] = []
    if len(opp_groups) != 4:
        opp_intrusions.append(f"只排了 {len(opp_groups)} 组")
    if len({int(rect.y) for rect, _ in opp_groups}) != 2:
        opp_intrusions.append("没有换成两行")

    def intersects(rect, box: tuple[int, int, int, int]) -> bool:
        return (rect.x < box[0] + box[2] and rect.right > box[0]
                and rect.y < box[1] + box[3] and rect.bottom > box[1])

    forbidden = (
        ("下家副露", T.R_SIDE_R_MELD),
        ("右侧牌河", T.R_SIDE_R_RIVER),
        ("对家牌河", T.R_OPP_RIVER),
        ("中央情报板", T.R_CENTER_BOARD),
    )
    for group_rect, tiles in opp_groups:
        for label, box in forbidden:
            if intersects(group_rect, box):
                opp_intrusions.append(f"组框 {group_rect} 压到{label} {box}")
        if group_rect.bottom > T.R_OPP_MELD[1] + T.R_OPP_MELD[3]:
            opp_intrusions.append(f"组框 {group_rect} 超出副露区下沿")
        for (tx, ty, tw, th), face in tiles:
            if tx < T.R_OPP_MELD[0] or tx + tw > T.R_OPP_MELD[0] + T.R_OPP_MELD[2]:
                opp_intrusions.append(f"牌 {face} 横向溢出")
    check(not opp_intrusions, f"对家 4 组副露两行网格不重叠 {opp_intrusions or ''}")

    # 含字牌场景的单帧耗时（update + render 一轮，30 次取最大）
    # 说明：测量前做 3 帧预热并暂停 GC，避免把「冷启动/GC 停顿」误判为渲染劣化
    #       （阈值与断言判据本身未改动：仍是 30 帧的**最大**值 ≤ 10 ms）。
    table_frame_ms = _measure_frame_ms(app, frames=30)
    check(
        table_frame_ms[-1] <= 10.0,
        f"含字牌牌桌单帧耗时 中位 {table_frame_ms[len(table_frame_ms) // 2]:.2f} ms / 最大 {table_frame_ms[-1]:.2f} ms（≤ 10 ms）",
    )

    print("[6] 字牌结算：分组还原（字牌杠 + 字牌刻子 + 字牌做将）")
    app.state.apply(HONOR_STATE_SYNC)
    app.state.apply(HONOR_GAME_END)
    check(app.state.screen == SCREEN_RESULT, "含字牌的 game_end 进入结算界面")
    honor_groups, honor_concealed = result_view.group_winner_hand(app.state, 1)
    check(
        [m.type for m in honor_groups] == [MELD_TYPE.KONG, MELD_TYPE.PONG, MELD_TYPE.CHOW, MELD_TYPE.PONG],
        f"结算分组还原 = {[m.type for m in honor_groups]}（含字牌杠/字牌刻子）",
    )
    check(honor_concealed == ["5z", "5z"], f"结算门前手牌 = {honor_concealed}（字牌做将）")
    check(
        sorted([t for m in honor_groups for t in m.tiles] + honor_concealed) == sorted(HONOR_WIN_HAND),
        "分组 + 门前手牌的多重集 == results[].hand（无丢牌/无重牌）",
    )
    result_frame_ms = _measure_frame_ms(app, frames=30)
    check(
        result_frame_ms[-1] <= 10.0,
        f"含字牌结算单帧耗时 中位 {result_frame_ms[len(result_frame_ms) // 2]:.2f} ms / 最大 {result_frame_ms[-1]:.2f} ms（≤ 10 ms）",
    )
    result_path = os.path.join(ARTIFACTS, "client_result.png")
    app.now = time.monotonic()
    pygame.image.save(app.render_frame(present=True), result_path)
    check(os.path.getsize(result_path) > 1000, f"含字牌的结算 PNG 已保存：{result_path}")

    print("[7] NDJSON over TCP：握手链路 + 主循环不阻塞")
    server = StubServer()
    server.start()
    step_times: list[float] = []
    app.nickname = "玩家_"
    app.host, app.port = "127.0.0.1", server.port
    handshake(app, server, step_times)
    check(server.wait_for(FRAME_TYPE.LOGIN, 0.01), f"客户端发出 login；已收到 {server.types()}")
    login_frame = next((f for f in server.received if f.get("type") == FRAME_TYPE.LOGIN), {})
    check(login_frame.get("name") == "玩家_", f"login.name = {login_frame.get('name')!r}")
    if server.raw_lines:
        raw = server.raw_lines[0]
        check(raw.endswith(b"\n"), "出站帧以单个 \\n 结尾（NDJSON）")
        check(raw.decode("utf-8").strip().startswith("{"), "出站帧为 UTF-8 JSON 对象")
    check(server.type_count(FRAME_TYPE.CREATE_ROOM) == 1, "整条握手链只发过一次 create_room（大厅点一次）")
    check(server.wait_for(FRAME_TYPE.READY, 0.01), "room_joined → 自动 ready")
    check(app.state.room_id == "r-0001" and app.state.my_seat == 0, "room_joined 已缓存 room_id/seat")
    check(app.state.screen == SCREEN_TABLE, "game_start → 进入牌桌界面")
    check(len(app.state.hand) == 14, f"庄家手牌 14 张：len={len(app.state.hand)}")
    check(app.state.dealer == 0, f"庄家 = {app.state.dealer}")

    server.send({"type": "future_frame", "note": "未知帧类型应被忽略"})
    pump(app, 0.6, max_step=step_times)
    check(app.state.screen == SCREEN_TABLE, "未知服务端帧类型被忽略且不崩溃")

    print("[8] 出站帧守卫（§5.5 / H1 / H2）")
    window = dict(DISCARD_REQUEST)
    window["deadline_ms"] = int(time.time() * 1000) + 15000
    server.send(window)
    pump(app, 1.0, until=lambda: app.state.window is not None and app.state.window.window == WINDOW.DISCARD,
         max_step=step_times)
    check(app.state.window is not None, "收到打牌窗口 action_request")
    app.state.selected_index = 1
    tile_to_discard = app.state.hand[1]
    seq_before = app.state.window.seq
    check(app._confirm_discard(), f"确认打牌 → 发送 discard（tile={tile_to_discard}）")
    pump(app, 1.0, until=lambda: server.wait_for(FRAME_TYPE.DISCARD, 0.01), max_step=step_times)
    sent = next((f for f in server.received if f.get("type") == FRAME_TYPE.DISCARD), None)
    check(sent is not None, "服务端已收到 discard")
    if sent is not None:
        check(sent.get("tile") == tile_to_discard, f"discard.tile = {sent.get('tile')}")
        check(sent.get("seq") == seq_before, f"discard.seq = {sent.get('seq')}（回带 current_seq）")
    check(app.state.pending and app.state.pending_label == "打牌", "提交后进入 PENDING（按钮禁用）")

    # 字牌同样可被点选并打出（§4.5 命中测试 → §4.6 discard 载荷）
    honor_index = app.state.hand.index("1z")
    honor_rects = table_view.hand_layout(app.state)
    honor_hit = table_view.hand_hit_test(
        app.state, (honor_rects[honor_index][0] + 2, honor_rects[honor_index][1] + 2)
    )
    check(honor_hit == honor_index, f"字牌手牌命中测试：点第 {honor_index + 1} 张（1z）→ index={honor_hit}")
    honor_window = dict(DISCARD_REQUEST)
    honor_window["seq"] = seq_before + 1
    honor_window["deadline_ms"] = int(time.time() * 1000) + 15000
    server.send(honor_window)
    pump(app, 1.0, until=lambda: app.state.window is not None and app.state.window.seq == seq_before + 1,
         max_step=step_times)
    app.state.selected_index = honor_index
    check(app._confirm_discard(), "字牌可选并确认打出（发送 discard）")
    pump(app, 1.0, until=lambda: any(f.get("type") == FRAME_TYPE.DISCARD and f.get("tile", "").endswith("z")
                                     for f in server.received), max_step=step_times)
    honor_sent = next((f for f in server.received if str(f.get("tile", "")).endswith("z")), None)
    check(
        honor_sent is not None and honor_sent.get("tile") == "1z",
        f"字牌 discard 帧 = {honor_sent}（牌面字符串原样透传、协议路径不变）",
    )

    claim = dict(CLAIM_REQUEST)
    claim["seq"] = seq_before + 2
    claim["deadline_ms"] = int(time.time() * 1000) + 15000
    server.send(claim)
    pump(app, 1.0, until=lambda: app.state.window is not None and app.state.window.window == WINDOW.CLAIM,
         max_step=step_times)
    check(not app._do_action(ACTION.PONG, ["1m"]), "动作不在 options 内 → 拒绝发送（不产生 error 帧）")
    check(not app._do_action(ACTION.KONG, ["6p"]), "claim 窗口无 kong 选项 → 拒绝发送")
    check(app._do_action(ACTION.PASS, []), "claim 窗口的 pass 可发送")
    pump(app, 1.0, until=lambda: server.wait_for(FRAME_TYPE.ACTION, 0.01), max_step=step_times)
    act = next((f for f in server.received if f.get("type") == FRAME_TYPE.ACTION), None)
    check(act is not None and act.get("action") == ACTION.PASS, f"action 帧 = {act}")
    if act is not None:
        check(isinstance(act.get("seq"), int), "action 帧携带 seq（H2）")
        check(act.get("tiles") == [], "pass 的 tiles = []")

    print("[9] game_end → 结算；其后断线不弹蒙层（§8.4 第 3 行）")
    server.send(GAME_END)
    pump(app, 1.0, until=lambda: app.state.screen == SCREEN_RESULT, max_step=step_times)
    check(app.state.screen == SCREEN_RESULT, "game_end → 结算界面")
    app.now = time.monotonic()
    app.render_frame(present=False)
    server.close()
    pump(app, 2.0, until=lambda: not app.connected and app.net is not None
         and app.net.closed_reason is not None, max_step=step_times)
    check(app.disconnect_reason is None, "断线前已收到 game_end → 不显示断线蒙层")
    app.close_socket("阶段 A 结束")

    print("[10] 对局中断线：蒙层可见提示 + 主循环保持响应")
    server2 = StubServer()
    server2.start()
    app.return_to_login()
    app.nickname = "玩家_"
    app.host, app.port = "127.0.0.1", server2.port
    handshake(app, server2, step_times)
    check(app.state.screen == SCREEN_TABLE, "第二次握手成功进入牌桌")
    server2.close()
    pump(app, 3.0, until=lambda: app.disconnect_reason is not None, max_step=step_times)
    check(app.disconnect_reason is not None, f"断线后显示蒙层原因：{app.disconnect_reason}")
    app.now = time.monotonic()
    app.render_frame(present=False)
    app.close_socket("阶段 B 结束")

    print("[11] 服务器未启动：可见提示且不崩溃")
    refused = StubServer()
    dead_port = refused.port
    refused.close()  # 端口已释放且无监听 → 连接被拒
    offline = GameApp(host="127.0.0.1", port=dead_port, auto_scale=False)
    offline.nickname = "玩家_"
    offline.submit_login()
    pump(offline, 2.0, until=lambda: "连接失败" in offline.login_status, max_step=step_times)
    check("连接失败" in offline.login_status, f"登录界面状态行：{offline.login_status}")
    check(offline.login_stage == "IDLE" and offline.retry_at is not None, "已排定 1 s 后自动重试")
    offline.now = time.monotonic()
    offline.render_frame(present=False)
    pump(offline, 10.0, until=lambda: "无法连接服务器" in offline.login_status, max_step=step_times)
    check("无法连接服务器" in offline.login_status, f"3 次重试后：{offline.login_status}")
    check(offline.login_retry_visible, "显示「重试」按钮（手动）")
    check(offline.running and offline._login_submit_enabled(), "主循环仍响应，可再次点击「进入牌桌」")
    offline.close_socket("测试结束")
    check(bool(step_times) and max(step_times) < 0.35,
          f"单次 update() 最大耗时 = {max(step_times) * 1000:.1f} ms（网络不阻塞主循环）")

    print("[12] 握手期心跳竞态（t28）：login_ok 之前零出站帧（GD-C1 零 error 帧）")
    # ① 反事实复现：把「修复前的 _heartbeat」逐字保留为参考实现，在**同一状态窗口**
    #    （TCP 已连、login 已发、login_ok 未到）跑一次心跳 → 必须发出 ping，
    #    且协议正确的服务端会回 NOT_LOGGED_IN → 复现 captain 实测到的 error 帧。
    race_stub = StubServer()
    race_stub.start()
    legacy_app = GameApp(host="127.0.0.1", port=race_stub.port, auto_scale=False)
    legacy_app.nickname = "玩家_"
    legacy_inbound: list[int] = []
    legacy_errors: list[int] = []
    _legacy_orig_on_frame = legacy_app._on_frame

    def _legacy_spy(frame, now, _orig=_legacy_orig_on_frame, _sink=legacy_inbound, _err=legacy_errors):
        if isinstance(frame, dict):
            _sink.append(frame.get("type"))
            if frame.get("type") == FRAME_TYPE.ERROR:
                _err.append(frame.get("code"))
        return _orig(frame, now)

    legacy_app._on_frame = _legacy_spy
    legacy_app.connect()
    pump(legacy_app, 3.0, until=lambda: race_stub.wait_for(FRAME_TYPE.LOGIN, 0.01))
    check(race_stub.wait_for(FRAME_TYPE.LOGIN, 0.01), "反事实场景：TCP 已连且 login 已发出")
    check(legacy_app.login_stage == "WAIT_LOGIN_OK", "反事实场景：仍在等待 login_ok（正是竞态窗口）")
    legacy_app.last_ping = None          # 与真实窗口一致：心跳从未发过
    _legacy_heartbeat_ping(legacy_app, time.monotonic())
    pump(legacy_app, 1.0, until=lambda: FRAME_TYPE.ERROR in legacy_inbound)
    check(
        race_stub.wait_for(FRAME_TYPE.PING, 0.01),
        f"反事实复现：修复前实现在该窗口发出 ping（服务端收到 {race_stub.types()}）",
    )
    check(
        FRAME_TYPE.PING in race_stub.pre_login_frames,
        f"反事实复现：该 ping 落在登录登记生效前（pre_login_frames={race_stub.pre_login_frames}）",
    )
    check(
        legacy_errors[:1] == [ERROR_CODE.NOT_LOGGED_IN],
        f"反事实复现：服务端回 error{{NOT_LOGGED_IN}} —— 复现 captain 实测症状（客户端收到 {legacy_inbound}）",
    )
    legacy_app.close_socket("反事实场景结束")
    race_stub.close()

    # ② 修复后：同一窗口（含人为推进 10 s 心跳时钟）不得发出任何 ping
    fix_stub = StubServer()
    fix_stub.start()
    fix_app = GameApp(host="127.0.0.1", port=fix_stub.port, auto_scale=False)
    fix_app.nickname = "玩家_"
    fix_app.connect()
    pump(fix_app, 3.0, until=lambda: fix_stub.wait_for(FRAME_TYPE.LOGIN, 0.01))
    check(fix_app.login_stage == "WAIT_LOGIN_OK", "修复后场景：同样处于等待 login_ok 的窗口")
    check(fix_app.logged_in is False, "修复后场景：logged_in 仍为 False（login_ok 未到）")
    # 推进真实心跳若干 tick，并把心跳时钟人为前推 10 s（窗口拖长也不许发 ping）
    pump(fix_app, 1.0)
    for delta in (0.0, HEARTBEAT_INTERVAL * 2, HEARTBEAT_INTERVAL * 4):
        fix_app._heartbeat(time.monotonic() + delta)
    pump(fix_app, 1.0)
    pre_login_out = [t for t in fix_stub.types() if t != FRAME_TYPE.LOGIN]
    check(
        pre_login_out == [] and fix_stub.pre_login_frames == [],
        f"修复后：握手完成前零出站帧（服务端仅收到 {fix_stub.types()}，非 login 帧 {fix_stub.pre_login_frames}）",
    )
    check(
        fix_app.state.error_count == 0,
        f"修复后：该窗口内零 error 帧（error_count={fix_app.state.error_count}）",
    )

    # ③ 登录后心跳必须继续工作：间隔 >= HEARTBEAT_INTERVAL，且 pong 更新 RTT
    fix_inbound: list[int] = []
    _fix_orig_on_frame = fix_app._on_frame

    def _fix_spy(frame, now, _orig=_fix_orig_on_frame, _sink=fix_inbound):
        if isinstance(frame, dict):
            _sink.append(frame.get("type"))
        return _orig(frame, now)

    fix_app._on_frame = _fix_spy
    fix_stub.send({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-race", "name": "玩家_"})
    pump(fix_app, 2.0, until=lambda: fix_app.state.screen == SCREEN_LOBBY)
    check(fix_app.logged_in and fix_app.state.screen == SCREEN_LOBBY, "login_ok 后：logged_in=True 且进入大厅")
    fix_app.start_game()   # 旧握手 / 再来一局：create_room 后自动 ready
    pump(fix_app, 2.0, until=lambda: fix_stub.wait_for(FRAME_TYPE.CREATE_ROOM, 0.01))
    check(fix_stub.wait_for(FRAME_TYPE.CREATE_ROOM, 0.01), "start_game() 继续握手（create_room）")
    pings_before = fix_stub.type_count(FRAME_TYPE.PING)
    fix_app._heartbeat(fix_app.last_ping + HEARTBEAT_INTERVAL - 0.5)     # 未到间隔
    pump(fix_app, 0.4)
    check(
        fix_stub.type_count(FRAME_TYPE.PING) == pings_before,
        f"登录后：未到 HEARTBEAT_INTERVAL 不发 ping（间隔下限生效，ping={fix_stub.type_count('ping')}）",
    )
    # 回拨心跳时钟使间隔已到（用真实时间，避免污染 RTT 的 ping_sent_at）
    fix_app.last_ping = time.monotonic() - HEARTBEAT_INTERVAL - 1.0
    pump(fix_app, 1.5, until=lambda: fix_stub.type_count(FRAME_TYPE.PING) > pings_before)
    check(
        fix_stub.type_count(FRAME_TYPE.PING) == pings_before + 1,
        "登录后：达到 HEARTBEAT_INTERVAL 即恢复发送 ping（心跳未退化）",
    )
    pump(fix_app, 1.5, until=lambda: fix_app.rtt_ms is not None)
    check(
        fix_app.rtt_ms is not None and fix_app.rtt_ms >= 0,
        f"登录后：收到 pong 后 RTT 更新（rtt={fix_app.rtt_ms:.2f} ms）",
    )
    check(FRAME_TYPE.ERROR not in fix_inbound, f"整段竞态用例结束仍为零 error 帧（收到 {fix_inbound}）")
    fix_app.close_socket("竞态用例结束")
    fix_stub.close()

    print("[13] 真实服务端 + 真实客户端：端到端零 error 帧（t28 盲区补测）")
    # 大厅会发 list_rooms，二进制必须是当前协议。优先刚编出来的 build/，再回退 release。
    server_bin = next(
        (
            os.path.join(REPO_ROOT, rel, "game", "mahjong_server")
            for rel in ("build", "build-release")
            if os.path.exists(os.path.join(REPO_ROOT, rel, "game", "mahjong_server"))
        ),
        "",
    )
    if not server_bin:
        check(False, "真实服务端二进制缺失：build/game/mahjong_server（无法完成端到端零 error 验证）")
    else:
        real_port = _free_port()
        log_path = os.path.join(ARTIFACTS, "client_real_server_log.txt")
        with open(log_path, "wb") as log_file:
            proc = subprocess.Popen(
                [server_bin, "--port", str(real_port), "--seed", "199"],
                stdout=log_file, stderr=subprocess.STDOUT,
            )
        try:
            up = _wait_port(real_port, timeout=15.0)
            check(up, f"真实服务端已启动：{server_bin} --port {real_port} --seed 199")
            real_app = GameApp(host="127.0.0.1", port=real_port, auto_scale=False)
            real_app.nickname = "真实客户端"
            inbound: list[int] = []
            outbound: list[tuple[int, bool]] = []
            orig_on_frame, orig_send_frame = real_app._on_frame, real_app.send_frame

            def spy_on_frame(frame, now, _orig=orig_on_frame, _sink=inbound):
                if isinstance(frame, dict):
                    _sink.append(frame.get("type"))
                return _orig(frame, now)

            def spy_send_frame(ftype, _orig=orig_send_frame, _sink=outbound, _app=real_app, **fields):
                ok = _orig(ftype, **fields)
                if ok:
                    _sink.append((ftype, bool(_app.logged_in)))
                return ok

            real_app._on_frame = spy_on_frame          # 仅测试侧观测，不改产品代码
            real_app.send_frame = spy_send_frame
            real_app.submit_login()
            pump(real_app, 20.0, until=lambda: real_app.state.screen == SCREEN_LOBBY)
            check(real_app.state.screen == SCREEN_LOBBY, "真实客户端 连接→登录 后停在大厅")
            real_app.start_game()   # 旧握手：create_room 后自动 ready，直到进入牌桌
            pump(real_app, 20.0, until=lambda: real_app.state.screen == SCREEN_TABLE)
            check(real_app.state.screen == SCREEN_TABLE, "真实客户端走完 连接→登录→大厅→开局（进入牌桌界面）")
            check(
                real_app.state.error_count == 0 and FRAME_TYPE.ERROR not in inbound,
                f"端到端零 error 帧：收到 {len(inbound)} 帧、error_count={real_app.state.error_count}"
                f"（帧序 {inbound[:6]}）",
            )
            pre_login_sends = [t for t, logged_in in outbound if t != FRAME_TYPE.LOGIN and not logged_in]
            check(
                pre_login_sends == [],
                f"真实路径握手期零非 login 出站帧（出站序 {[t for t, _ in outbound]}）",
            )
            order_ok = all(x in inbound for x in (FRAME_TYPE.LOGIN_OK, FRAME_TYPE.ROOM_JOINED, FRAME_TYPE.GAME_START))
            check(order_ok, f"真实握手链完整：login_ok → room_joined → game_start（{ [x for x in inbound if x in (FRAME_TYPE.LOGIN_OK, FRAME_TYPE.ROOM_JOINED, FRAME_TYPE.GAME_START)] }）")
            # 固化为可复现证据
            evidence = {
                "server_bin": server_bin,
                "server_sha256": _sha256(server_bin),
                "server_mtime": os.path.getmtime(server_bin),
                "port": real_port,
                "inbound_frames": inbound,
                "outbound_frames": [{"type": t, "before_login_ok": not li} for t, li in outbound],
                "error_count": real_app.state.error_count,
                "reached_table": real_app.state.screen == SCREEN_TABLE,
                "hand": real_app.state.hand,
            }
            evidence_path = os.path.join(ARTIFACTS, "client_zero_error_handshake.json")
            with open(evidence_path, "w", encoding="utf-8") as fh:
                json.dump(evidence, fh, ensure_ascii=False, indent=2)
            check(os.path.getsize(evidence_path) > 100, f"零 error 证据已固化：{evidence_path}")
            real_app.close_socket("真实服务端用例结束")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    print("[14] 按钮文案 = 中文牌名（UI v1.4 §5.3），协议载荷不变")
    # 牌名映射（数牌 = rank + 花色字；字牌 = 繁體字形）
    numeric_map = [("1m", "1 万"), ("5p", "5 筒"), ("9s", "9 条"), ("2s", "2 条")]
    check(
        all(tile_label(code) == name for code, name in numeric_map),
        f"数牌牌名 = {[(c, tile_label(c)) for c, _ in numeric_map]}",
    )
    honor_map = [("1z", "東"), ("2z", "南"), ("3z", "西"), ("4z", "北"),
                 ("5z", "白"), ("6z", "發"), ("7z", "中")]
    check(
        all(tile_label(code) == glyph for code, glyph in honor_map),
        f"字牌牌名（繁體）= {[(c, tile_label(c)) for c, _ in honor_map]}",
    )
    check(
        all(tile_label(bad) == "?" for bad in ("8z", "zz", "", "1M", "0z")),
        "非法牌面的牌名为「?」（防御性、不抛异常）",
    )
    # v1.18：`0m`/`0p`/`0s` 是合法牌码（日麻赤宝牌），牌名带「赤」前缀
    check(
        all(tile_label(code) == f"赤 5 {suit}" for code, suit in (("0m", "万"), ("0p", "筒"), ("0s", "条"))),
        f"赤 5 牌名 = {[(c, tile_label(c)) for c in ('0m', '0p', '0s')]}",
    )
    # 赤 5 的**外观**走 CC0 贴图染色派生（`game/tools/make_aka_tiles.py`），
    # 与其它 33 种牌同源；素材必须随包发布，否则会静默退化成程序化重绘。
    aka_report = []
    aka_ok = True
    for code in ("0m", "0p", "0s"):
        path = face_dir / f"{code}.png"
        source = face_dir / f"5{code[1]}.png"
        if not (path.exists() and source.exists()):
            aka_report.append(f"{code}.png 或 5{code[1]}.png 缺失")
            aka_ok = False
            continue
        aka = pygame.image.load(str(path))
        normal = pygame.image.load(str(source))
        same_size = aka.get_size() == normal.get_size()
        recolored = pygame.image.tostring(aka, "RGBA") != pygame.image.tostring(normal, "RGBA")
        aka_report.append(f"{code}={aka.get_size()}{'染色' if recolored else '未染色'}")
        aka_ok = aka_ok and same_size and recolored
    check(aka_ok, f"赤 5 贴图 = 5 的同尺寸染色派生（{aka_report}）")
    # 只要 `5X.png` 换了素材，`0X.png` 必须跟着重新生成（否则肉眼看不出的旧色）
    try:
        sys.path.insert(0, os.path.join(GAME_DIR, "tools"))
        from PIL import Image as PILImage  # noqa: E402
        from make_aka_tiles import DEFAULT_RED, recolor as recolor_aka  # noqa: E402

        drifted = [
            code for code in ("0m", "0p", "0s")
            if recolor_aka(PILImage.open(str(face_dir / f"5{code[1]}.png")), DEFAULT_RED).tobytes()
            != PILImage.open(str(face_dir / f"{code}.png")).convert("RGBA").tobytes()
        ]
        check(not drifted, f"赤 5 贴图与生成脚本结果一致（漂移 {drifted}）")
    except ImportError:  # pragma: no cover - 生成工具依赖 Pillow，测试机可能没有
        pass

    # 按钮文案公式逐字对齐 §5.3
    check(button_label(ACTION.KONG, ["5p"]) == "杠 5 筒", f"kong 文案 = {button_label(ACTION.KONG, ['5p'])}")
    check(button_label(ACTION.PONG, ["2s"]) == "碰 2 条", f"pong 文案 = {button_label(ACTION.PONG, ['2s'])}")
    check(button_label(ACTION.PONG, ["1z"]) == "碰 東", f"pong（字牌）文案 = {button_label(ACTION.PONG, ['1z'])}")
    check(button_label(ACTION.KONG, ["7z"]) == "杠 中", f"kong（字牌）文案 = {button_label(ACTION.KONG, ['7z'])}")
    chow_label = button_label(ACTION.CHOW, ["3p", "4p"])
    check(chow_label == "吃 3 筒 4 筒", f"chow 文案 = {chow_label}")
    # 被鸣牌由 action_event.from_seat 的牌河确定（§4.7），故文案不得出现它（这里以 2p 为例）
    claimed_tile_label = tile_label("2p")
    check(
        chow_label == "吃 " + tile_label("3p") + " " + tile_label("4p")
        and claimed_tile_label not in chow_label
        and chow_label.count(tile_label("3p")) == 1
        and chow_label.count(tile_label("4p")) == 1,
        f"chow 只列手中消耗的两张（不含被鸣牌 {claimed_tile_label}，§4.7）",
    )
    check(
        [button_label(a) for a in (ACTION.WIN, ACTION.PASS, FRAME_TYPE.DISCARD)] == ["胡", "过", "打牌"],
        "win/pass/discard 文案 = ['胡', '过', '打牌']",
    )

    # 操作栏按钮实渲染：6 按钮满配（含字牌杠 + 数牌碰 + 两条吃）
    label_window = {
        "type": FRAME_TYPE.ACTION_REQUEST, "seq": 31,
        "deadline_ms": int(time.time() * 1000) + 15000, "window": WINDOW.CLAIM,
        "options": [
            {"action": ACTION.WIN, "tiles": []},
            {"action": ACTION.KONG, "tiles": ["7z"]},
            {"action": ACTION.PONG, "tiles": ["2s"]},
            {"action": ACTION.CHOW, "tiles": ["3p", "4p"]},
            {"action": ACTION.CHOW, "tiles": ["4p", "5p"]},
            {"action": ACTION.PASS, "tiles": []},
        ],
    }
    label_app = make_app()
    label_app.state.apply(GAME_START)
    label_app._cache_room_joined(ROOM_JOINED)
    label_app.state.apply(label_window)
    label_app.now = time.monotonic()
    label_specs = actionbar.build_buttons(label_app.state, label_app._remaining_ms())
    check(
        [s.label for s in label_specs] == ["胡", "杠 中", "碰 2 条", "吃 3 筒 4 筒", "吃 4 筒 5 筒", "过"],
        f"按钮文案 = {[s.label for s in label_specs]}",
    )
    check(
        [s.kind for s in label_specs] == [ACTION.WIN, ACTION.KONG, ACTION.PONG, ACTION.CHOW, ACTION.CHOW, ACTION.PASS],
        f"按钮顺序/类型未变 = {[s.kind for s in label_specs]}",
    )
    # 文案宽度不得溢出按钮可用宽度（不得换行、不得溢出）
    label_font = label_app.fonts.get(T.FS_BODY)
    overflow = []
    for spec in label_specs:
        avail = spec.rect[2] - 2 * actionbar.BUTTON_TEXT_PAD
        rendered = actionbar.fit_label(spec.label, label_font, avail)
        if label_font.size(rendered)[0] > avail or "\n" in rendered:
            overflow.append((spec.label, rendered, label_font.size(rendered)[0], avail))
    check(not overflow, f"每个按钮文案宽度 <= 可用宽度 {overflow or ''}")
    # 省略号截断路径必须真的可达（非空转断言）
    narrow = actionbar.fit_label("吃 3 筒 4 筒", label_font, 40)
    check(
        narrow.endswith(actionbar.ELLIPSIS) and label_font.size(narrow)[0] <= 40 and "\n" not in narrow,
        f"窄按钮下以省略号截断：'吃 3 筒 4 筒' → {narrow!r}（宽 {label_font.size(narrow)[0]} <= 40）",
    )
    label_app.render_frame(present=True)
    labels_path = os.path.join(ARTIFACTS, "client_actionbar_labels.png")
    pygame.image.save(label_app.logical, labels_path)
    check(os.path.getsize(labels_path) > 1000, f"中文牌名操作栏 PNG 已保存：{labels_path}")
    label_app.close_socket("文案渲染用例结束")

    # 「显示 ≠ 载荷」：按钮与「已提交」是中文牌名，出站帧 tiles 仍是线上牌码
    payload_stub = StubServer()
    payload_stub.start()
    payload_app = GameApp(host="127.0.0.1", port=payload_stub.port, auto_scale=False)
    payload_app.nickname = "文案测试"
    handshake(payload_app, payload_stub, step_times)
    seq = 40
    for action, tiles, expected_label in (
        (ACTION.PONG, ["9s"], "碰 9 条"),
        (ACTION.KONG, ["1z"], "杠 東"),
        (ACTION.CHOW, ["3p", "4p"], "吃 3 筒 4 筒"),
    ):
        seq += 1
        payload_stub.send({
            "type": FRAME_TYPE.ACTION_REQUEST, "seq": seq,
            "deadline_ms": int(time.time() * 1000) + 15000, "window": WINDOW.CLAIM,
            "options": [{"action": action, "tiles": tiles}, {"action": ACTION.PASS, "tiles": []}],
        })
        pump(payload_app, 1.0, until=lambda: payload_app.state.window is not None
             and payload_app.state.window.seq == seq, max_step=step_times)
        specs = actionbar.build_buttons(payload_app.state, payload_app._remaining_ms())
        button = next((s for s in specs if s.kind == action), None)
        check(
            button is not None and button.label == expected_label,
            f"按钮文案（显示层）= {button.label if button else None!r}",
        )
        sent_before = [f for f in payload_stub.received if f.get("type") == FRAME_TYPE.ACTION]
        check(payload_app._do_action(action, tiles), f"提交 {action} {tiles}")
        pump(payload_app, 1.0,
             until=lambda: len([f for f in payload_stub.received if f.get("type") == FRAME_TYPE.ACTION]) > len(sent_before),
             max_step=step_times)
        sent = [f for f in payload_stub.received if f.get("type") == FRAME_TYPE.ACTION][-1]
        check(
            sent.get("tiles") == tiles and sent.get("action") == action,
            f"出站帧载荷仍为线上牌码：action={sent.get('action')} tiles={sent.get('tiles')}（显示「{expected_label}」）",
        )
        check(
            payload_app.state.pending_label == expected_label,
            f"「已提交：{payload_app.state.pending_label}」使用同一公式",
        )
    payload_app.close_socket("文案用例结束")
    payload_stub.close()

    print("[15] 全界面去牌码化：提示条与「已选中」均为中文牌名（UI v1.4 §4.10 / §5.4）")
    toast_stub = StubServer()
    toast_stub.start()
    toast_app = GameApp(host="127.0.0.1", port=toast_stub.port, auto_scale=False)
    toast_app.nickname = "提示条测试"
    handshake(toast_app, toast_stub, step_times)
    check(toast_app.state.screen == SCREEN_TABLE, "提示条用例已进入牌桌")
    # 本节用合成帧驱动提示条：先把状态置为自洽（手牌含 4p、本家牌河非空），
    # 避免驱动过程中产生「牌不在手牌中」「from_seat 牌河为空」等预期外 WARN 干扰日志阅读
    toast_app.state.hand = list(GAME_START["hand"]) + ["4p"]
    # 本家牌河预置 3 张：随后 pong / kong / chow 三次事件各弹掉最后一张
    toast_app.state.seat_view(0).discards = ["1s", "2s", "3s"]

    def drive_toast(frame: dict) -> str:
        """经**真实 socket** 下发一帧，读取由此产生的提示条文案。"""
        toast_app.toast_text = ""
        toast_stub.send(frame)
        pump(toast_app, 1.0, until=lambda: bool(toast_app.toast_text), max_step=step_times)
        return toast_app.toast_text

    # ① 摸牌提示（数牌 + 字牌繁體）：显示中文牌名，而帧内载荷仍是线上牌码
    draw_frame = {"type": FRAME_TYPE.DRAW, "tile": "7m", "seat": 0, "seq": 51}
    toast = drive_toast(draw_frame)
    check(
        toast == "你摸到 7 万" and draw_frame["tile"] == "7m" and "7m" not in toast,
        f"摸牌提示显示「{toast}」而载荷 tile={draw_frame['tile']!r}",
    )
    honor_draw = {"type": FRAME_TYPE.DRAW, "tile": "1z", "seat": 0, "seq": 52}
    toast = drive_toast(honor_draw)
    check(
        toast == "你摸到 東" and honor_draw["tile"] == "1z",
        f"摸牌提示（字牌繁體）显示「{toast}」而载荷 tile={honor_draw['tile']!r}",
    )

    # ② 打牌提示（本家 / 他家）
    self_discard = {"type": FRAME_TYPE.DISCARD_EVENT, "seat": 0, "tile": "4p", "seq": 53}
    toast = drive_toast(self_discard)
    check(
        toast == "你打出 4 筒" and self_discard["tile"] == "4p" and "4p" not in toast,
        f"本家打牌提示显示「{toast}」而载荷 tile={self_discard['tile']!r}",
    )
    other_discard = {"type": FRAME_TYPE.DISCARD_EVENT, "seat": 1, "tile": "2s", "seq": 54}
    toast = drive_toast(other_discard)
    check(
        toast == "下家 打出 2 条" and other_discard["tile"] == "2s" and "2s" not in toast,
        f"他家打牌提示显示「{toast}」而载荷 tile={other_discard['tile']!r}",
    )

    # ③ 鸣牌事件提示条：结构 `<座位> <动作> <牌名>` 不变，牌名为中文（含字牌繁體）
    for frame, expected, wire in (
        ({"type": FRAME_TYPE.ACTION_EVENT, "seat": 1, "action": ACTION.PONG, "tiles": ["9s"],
          "from_seat": 0, "seq": 55}, "下家 碰 9 条", "9s"),
        ({"type": FRAME_TYPE.ACTION_EVENT, "seat": 2, "action": ACTION.KONG, "tiles": ["1z"],
          "from_seat": 0, "seq": 56}, "对家 杠 東", "1z"),
        ({"type": FRAME_TYPE.ACTION_EVENT, "seat": 3, "action": ACTION.CHOW, "tiles": ["3p", "4p"],
          "from_seat": 0, "seq": 57}, "上家 吃 3 筒 4 筒", "3p"),
        ({"type": FRAME_TYPE.ACTION_EVENT, "seat": 1, "action": ACTION.WIN, "tiles": ["8p"],
          "from_seat": 0, "seq": 58}, "下家 胡 8 筒", "8p"),
    ):
        toast = drive_toast(frame)
        check(
            toast == expected and wire in frame["tiles"] and wire not in toast,
            f"事件提示条显示「{toast}」而载荷 tiles={frame['tiles']}",
        )

    # ④ 「已选中」提示（§5.4 冻结原句；牌名走 §5.3 公式）
    check(
        table_view.selection_hint("4p") == "已选中 4 筒，再点一次或按 Enter 打出",
        f"已选中提示 = {table_view.selection_hint('4p')!r}",
    )
    check(
        table_view.selection_hint("1z") == "已选中 東，再点一次或按 Enter 打出",
        f"已选中提示（字牌繁體）= {table_view.selection_hint('1z')!r}",
    )
    # 渲染两帧落盘供目视确认（提示条与「已选中」占同一位置，互斥，故分别渲染）
    # 渲染前把本家用例手牌复位为一致值，避免本节额外下发的 draw 帧造成 HUD「张数异常」干扰目视
    toast_app.state.hand = list(GAME_START["hand"])
    toast_app.state.check_hand_count()
    toast_stub.send({"type": FRAME_TYPE.ACTION_EVENT, "seat": 2, "action": ACTION.KONG, "tiles": ["1z"],
                     "from_seat": 0, "seq": 60})
    pump(toast_app, 1.0, until=lambda: toast_app.toast_text == "对家 杠 東", max_step=step_times)
    toast_app.now = time.monotonic()
    toast_app.render_frame(present=True)
    toast_path = os.path.join(ARTIFACTS, "client_toast_labels.png")
    pygame.image.save(toast_app.logical, toast_path)
    check(os.path.getsize(toast_path) > 1000, f"提示条（中文牌名）PNG 已保存：{toast_path}")

    toast_app.toast_text = ""
    toast_app.state.selected_index = 0
    toast_app.now = time.monotonic()
    toast_app.render_frame(present=True)
    hint_path = os.path.join(ARTIFACTS, "client_selection_hint.png")
    pygame.image.save(toast_app.logical, hint_path)
    check(os.path.getsize(hint_path) > 1000, f"「已选中」中文提示 PNG 已保存：{hint_path}")
    toast_app.close_socket("提示条用例结束")
    toast_stub.close()

    print("[16] 战绩界面（UI_UX_SPEC v1.7：§4.11 入口 / §4.12 弹窗 / §9.2 结算页块）")
    stats_app = make_app()
    build_table_state(stats_app)

    # 入口只对账号会话出现：判据是服务端下发的 player_id 前缀（ACCOUNT_DESIGN §1.2）。
    stats_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "游客_"}, time.monotonic())
    check(not stats_app.has_account, "游客会话（p- 前缀）不显示战绩入口")
    check(not stats_app.build_ui().has_account_session, "游客会话 HUD 不画「战绩」按钮")
    stats_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "u-000042", "name": "lixueming"}, time.monotonic())
    check(stats_app.has_account, "账号会话（u- 前缀）显示战绩入口")
    check(stats_app.state.screen == SCREEN_LOBBY, "账号会话登录后同样先进大厅")
    stats_app.state.screen = SCREEN_TABLE  # 本段要验证的是牌桌上的战绩弹窗

    # stats 帧：合法载荷留存、非法载荷忽略（渲染层不做协议校验，但也不该被脏数据打崩）。
    stats_app._on_frame({"type": FRAME_TYPE.STATS, "games": "x", "wins": 0, "draws": 0, "net_score": 0, "recent": []},
                        time.monotonic())
    check(stats_app.stats is None, "字段类型不对的 stats 被忽略（不影响画面）")
    stats_app._on_frame(STATS_FRAME, time.monotonic())
    check(stats_app.stats is not None and stats_app.stats["games"] == 12, "合法 stats 已留存")

    # 打开面板：发出一次 stats_request（唯一的出站帧），并且不发别的。
    sent: list[tuple] = []
    original_send = stats_app.send_frame
    stats_app.send_frame = lambda ftype, **fields: (sent.append((ftype, fields)) or True)
    stats_app.toggle_stats()
    check(stats_app.stats_open, "点「战绩」打开面板")
    check(sent == [(FRAME_TYPE.STATS_REQUEST, {})], f"打开面板只发一次 stats_request：{sent}")
    stats_app.stats_open = False
    stats_app.send_frame = original_send

    # 模态：面板打开时牌桌点击被吞（不隔层出牌），面板外点击关闭，ESC 先关面板。
    stats_app.stats_open = True
    stats_app.state.selected_index = None
    stats_app._table_click((10.0, 700.0))  # 面板外（左下角牌河区）
    check(not stats_app.stats_open, "点面板外关闭战绩面板")
    stats_app.stats_open = True
    stats_app._table_click((T.R_STATS_PANEL[0] + 40, T.R_STATS_PANEL[1] + 40))
    check(stats_app.stats_open, "点面板内不关闭"
                                "（同时该点击不透传到牌桌）")
    stats_app._on_key(pygame.event.Event(pygame.KEYDOWN, key=pygame.K_ESCAPE, unicode=""))
    check(not stats_app.stats_open, "ESC 关闭战绩面板")

    stats_app.stats_open = True
    stats_app.now = time.monotonic()
    stats_app.render_frame(present=True)
    panel_path = os.path.join(ARTIFACTS, "client_stats_panel.png")
    pygame.image.save(stats_app.logical, panel_path)
    check(os.path.getsize(panel_path) > 1000, f"牌桌战绩弹窗 PNG 已保存：{panel_path}")

    stats_app.stats_open = False
    stats_app.state.apply(GAME_END)
    check(stats_app.state.screen == SCREEN_RESULT, "结算页可绘制战绩块")
    stats_app.now = time.monotonic()
    stats_app.render_frame(present=True)
    result_stats_path = os.path.join(ARTIFACTS, "client_result_stats.png")
    pygame.image.save(stats_app.logical, result_stats_path)
    check(os.path.getsize(result_stats_path) > 1000, f"结算页战绩块 PNG 已保存：{result_stats_path}")
    stats_app.close_socket("战绩用例结束")

    print("[17] 大厅界面（UI_UX_SPEC v1.9 §12：新建房间不自动 ready，开始对局才发 ready）")
    lobby_app = make_app()
    lobby_app.nickname = "lixueming"
    sent_frames: list[tuple] = []
    lobby_app.send_frame = lambda ftype, **fields: (sent_frames.append((ftype, fields)) or True)
    lobby_app.connected = True

    logged_at = time.monotonic()
    lobby_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "u-000001", "name": "lixueming"}, logged_at)
    check(lobby_app.state.screen == SCREEN_LOBBY, "账号登录后进入大厅")
    check(lobby_app.state.phase == "大厅", "大厅阶段文案 = 大厅")
    check(
        sent_frames == [(FRAME_TYPE.STATS_REQUEST, {}), (FRAME_TYPE.LIST_ROOMS, {})],
        f"大厅为账号会话拉战绩，并立刻拉房间列表：{sent_frames}",
    )
    check(not lobby_app._lobby_begin_enabled(), "还没入座时「开始对局」禁用")
    check(lobby_app._lobby_create_enabled(), "还没入座时「新建房间」可用")

    sent_frames.clear()
    lobby_app._poll_room_list(logged_at)
    check(sent_frames == [], "不到 1 秒不重复发 list_rooms")
    lobby_app._poll_room_list(logged_at + 1.0)
    check(sent_frames == [(FRAME_TYPE.LIST_ROOMS, {})], f"满 1 秒再发 list_rooms：{sent_frames}")

    sent_frames.clear()
    lobby_app._lobby_click((T.LB_START_BTN[0] + 4, T.LB_START_BTN[1] + 4))
    check(sent_frames == [], "未入座时点「开始对局」不发帧")

    lobby_app._lobby_click((T.LB_NEW_BTN[0] + 4, T.LB_NEW_BTN[1] + 4))
    check(sent_frames == [(FRAME_TYPE.CREATE_ROOM, {})], f"「新建房间」只发 create_room：{sent_frames}")
    sent_frames.clear()
    lobby_app._lobby_click((T.LB_NEW_BTN[0] + 4, T.LB_NEW_BTN[1] + 4))
    check(sent_frames == [], "建桌在途时再点不重复发帧（防连点）")
    check(not lobby_app._lobby_create_enabled(), "建桌在途时「新建房间」置灰")

    lobby_app._on_frame(ROOM_JOINED, time.monotonic())
    check(lobby_app.state.screen == SCREEN_LOBBY, "room_joined 后仍停在大厅")
    check(sent_frames == [], f"新建房间成功不自动 ready：{sent_frames}")
    check(
        lobby_app.login_status == "已在 r-0001，座位 0，点「开始对局」开局",
        f"入座状态行：{lobby_app.login_status!r}",
    )
    check(lobby_app._lobby_begin_enabled(), "入座后「开始对局」可用")
    check(not lobby_app._lobby_create_enabled(), "已在房间里时「新建房间」禁用")

    lobby_app._lobby_click((T.LB_START_BTN[0] + 4, T.LB_START_BTN[1] + 4))
    check(sent_frames == [(FRAME_TYPE.READY, {})], f"「开始对局」只发 ready：{sent_frames}")

    # 未入座时 Enter = 新建房间；入座后点别的房间不发 join_room。
    pick_app = make_app()
    pick_frames: list[tuple] = []
    pick_app.send_frame = lambda ftype, **fields: (pick_frames.append((ftype, fields)) or True)
    pick_app.connected = True
    pick_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    pick_frames.clear()
    pick_app._lobby_key(pygame.event.Event(pygame.KEYDOWN, key=pygame.K_RETURN, unicode=""))
    check(pick_frames == [(FRAME_TYPE.CREATE_ROOM, {})], f"未入座时 Enter 等于新建房间：{pick_frames}")
    pick_frames.clear()
    pick_app._on_frame(ROOM_JOINED, time.monotonic())
    check(pick_frames == [], "Enter 建桌成功后不自动 ready")
    pick_app.open_rooms = [
        {"room_id": "r-0001", "host": "玩家_", "players": 1, "seats": 4},
        {"room_id": "r-0002", "host": "甲", "players": 1, "seats": 4},
    ]
    pick_app._on_frame(
        {"type": FRAME_TYPE.ROOM_LIST, "rooms": [
            {"room_id": "r-0002", "host": "甲", "players": 1, "seats": 4},
            {"room_id": "r-0009", "host": "乙", "players": 2, "seats": 4},
        ]},
        time.monotonic(),
    )
    check(pick_app.state.room_id == "r-0001", "room_list 不改当前房间")
    check(pick_app.open_rooms[0]["room_id"] == "r-0002", "room_list 刷新列表")
    pick_frames.clear()
    pick_app._lobby_click((T.LB_ROOMS_X + 8, T.LB_ROOMS_Y + 4))
    check(pick_frames == [], "点别的房间不发帧")
    check(pick_app.login_status == "已经在房间里", f"已在别的房间：{pick_app.login_status!r}")
    seated = dict(ROOM_JOINED)
    seated["room_id"] = "r-0002"
    pick_app.state.room_id = "r-0002"
    pick_app.login_status = "已在 r-0002，座位 0，点「开始对局」开局"
    pick_frames.clear()
    pick_app._lobby_click((T.LB_ROOMS_X + 8, T.LB_ROOMS_Y + 4))
    check(pick_frames == [], "点自己所在的房间是空操作")
    check(pick_app.login_status.startswith("已在 r-0002"), "空操作不改状态行")

    join_app = make_app()
    join_frames: list[tuple] = []
    join_app.send_frame = lambda ftype, **fields: (join_frames.append((ftype, fields)) or True)
    join_app.connected = True
    join_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-aabbcc", "name": "加入者"}, time.monotonic())
    join_app.open_rooms = [{"room_id": "r-0007", "host": "房主", "players": 1, "seats": 4}]
    join_frames.clear()
    join_app._lobby_click((T.LB_ROOMS_X + 8, T.LB_ROOMS_Y + 8))
    check(join_frames == [(FRAME_TYPE.JOIN_ROOM, {"room_id": "r-0007"})], f"点空闲房间发 join_room：{join_frames}")
    joined = dict(ROOM_JOINED)
    joined["room_id"] = "r-0007"
    joined["seat"] = 1
    join_frames.clear()
    join_app._on_frame(joined, time.monotonic())
    check(join_app.state.screen == SCREEN_LOBBY, "加入成功后仍停在大厅")
    check(join_frames == [], "加入成功不自动 ready")
    check(
        join_app.login_status == "已在 r-0007，座位 1，点「开始对局」开局",
        f"加入后的座位号来自 room_joined：{join_app.login_status!r}",
    )
    join_frames.clear()
    join_app._lobby_key(pygame.event.Event(pygame.KEYDOWN, key=pygame.K_RETURN, unicode=""))
    check(join_frames == [(FRAME_TYPE.READY, {})], f"已入座时 Enter 只发 ready：{join_frames}")

    # 登录界面 → 大厅之后，「返回登录」必须断开并回到登录界面（而不是留在半连接状态）
    lobby_app.state.enter_lobby()
    lobby_app.login_stage = "IDLE"
    lobby_app.connected = True
    lobby_app._lobby_key(pygame.event.Event(pygame.KEYDOWN, key=pygame.K_ESCAPE, unicode=""))
    check(lobby_app.state.screen == SCREEN_LOGIN, "大厅 ESC → 返回登录界面")
    check(not lobby_app.has_account, "返回登录后战绩入口随会话清空")

    # 「再来一局」置位后，登录成功直接跳过大厅开局（§9.5 第 7 条）
    sent_frames.clear()
    rematch_app = make_app()
    rematch_app.nickname = "玩家_"
    rematch_app.send_frame = lambda ftype, **fields: (sent_frames.append((ftype, fields)) or True)
    rematch_app.connected = True
    rematch_app.auto_start_after_login = True
    rematch_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    check(sent_frames == [(FRAME_TYPE.CREATE_ROOM, {})], f"「再来一局」跳过大厅直接建桌：{sent_frames}")
    check(not rematch_app.auto_start_after_login, "自动开局标记用后即清（下次登录回大厅）")
    sent_frames.clear()
    rematch_app._on_frame(ROOM_JOINED, time.monotonic())
    check(sent_frames == [(FRAME_TYPE.READY, {})], f"「再来一局」在 room_joined 后自动 ready：{sent_frames}")

    # 大厅里断线：还没有对局在进行，不该套「对局进行中，服务端将托管你的座位」那层蒙层
    drop_app = make_app()
    drop_app.nickname = "玩家_"
    drop_app.connected = True
    drop_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    drop_app._on_closed("服务端关闭了连接", time.monotonic())
    check(drop_app.state.screen == SCREEN_LOGIN, "大厅断线 → 回登录界面")
    check(drop_app.disconnect_reason is None, "大厅断线不显示断线蒙层")
    check("连接已断开" in drop_app.login_status, f"大厅断线在状态行说明原因：{drop_app.login_status!r}")

    # ---------------------------------------------------------------- v2.0（B4）：一场四局客户端接入
    print("[18] v2.0：局终页 / 场终页 / game_start 重置 / 自理杠 / 牌背 18（§9.6 / §4.8 / §4.6）")

    # --- 局终页：无出口按钮、番种明细、卡片累计分 ---
    round_app = make_app()
    sent: list[tuple] = []
    round_app.send_frame = lambda ftype, **fields: (sent.append((ftype, fields)) or True)
    round_app.connected = True
    round_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    round_app._on_frame(dict(ROOM_JOINED), time.monotonic())
    round_app._on_frame(dict(GAME_START), time.monotonic())
    round_app._on_frame(dict(STATE_SYNC), time.monotonic())
    round_app._on_frame(dict(GAME_END), time.monotonic())
    check(round_app.state.screen == SCREEN_RESULT, "game_end → 局终页（SCREEN_RESULT）")
    check(round_app.state.match_end is None, "局终页：match_end 未到（不是场终页）")
    win = round_app.state.result.get("win")
    detail = result_view._yaku_detail(win)
    check("七对子 2番" in detail and "门前清 1番" in detail and "合计 3 番（8 分）" in detail,
          f"D-4: 番种明细按 §9.2 形状渲染（含合计番/分）：{detail}")
    check("已封顶" not in detail, f"D-4: 未封顶不得出现「已封顶」：{detail}")
    capped = dict(win)
    # F7：封顶判据是 `base + dealer > 6`（真被砍），**不是** `total >= 6`（打满 6 番不算被砍）
    capped["han_breakdown"] = {"base": 6, "tsumo": 0, "dealer": 1, "total": 6}
    capped["multiplier"] = 64
    capped["han"] = 6
    capped_detail = result_view._yaku_detail(capped)
    check("已封顶" in capped_detail and "（64 分）已封顶" in capped_detail,
          f"D-4: 封顶提示必须是「（N 分）已封顶」句式：{capped_detail}")
    exact = dict(win)
    exact["han_breakdown"] = {"base": 5, "tsumo": 0, "dealer": 1, "total": 6}  # 恰 6 番 = 打满
    exact["multiplier"] = 64
    exact["han"] = 6
    check("已封顶" not in result_view._yaku_detail(exact),
          f"F7: base+dealer == 6 是「打满 6 番」，不得显示已封顶：{result_view._yaku_detail(exact)}")
    # F6：`yaku` 键缺失 ≠ 0 番（L2「明细不可用」）
    missing = dict(win)
    missing.pop("yaku", None)
    check(result_view._yaku_detail(missing) == "番种明细不可用",
          f"F6: yaku 键缺失必须显示明细不可用，got {result_view._yaku_detail(missing)!r}")
    check("无番（0 番）" == result_view._yaku_detail({"yaku": []}), "无番和牌文案 = 无番（0 番）（§9.2）")
    check("平和" not in result_view._yaku_detail({"yaku": []}), "无番文案不得再用「平和」")
    # D-3（§9.2 收付文案末行）：按 match_total 取领先者，并列时「并列领先」
    leader = result_view._leader_text(round_app.state)
    check(leader == "本场累计：AI-1 领先", f"D-3: 唯一最大 → 「XX 领先」，got {leader!r}")
    tie_state = make_app()
    tie_state.state.apply(dict(GAME_START))
    tie_end = dict(GAME_END)
    tie_end["results"] = [dict(entry, match_total=0) for entry in GAME_END["results"]]
    tie_state.state.apply(tie_end)
    tie_text = result_view._leader_text(tie_state.state)
    check(tie_text.endswith("并列领先") and "、" in tie_text,
          f"D-3: 并列最大 → 「XX、YY 并列领先」，got {tie_text!r}")
    # v1.20（PROTOCOL §12.13）：场终剩棒归一位 —— 只在「末局没人收供托」时出现
    award_app = make_app()
    award_app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    award_app._on_frame(dict(ROOM_JOINED), time.monotonic())
    award_app._on_frame(dict(GAME_START), time.monotonic())
    draw_end = dict(GAME_END)
    draw_end.update({
        "reason": REASON.DRAW_EXHAUSTED, "win": None, "match_will_continue": False,
        "tenpai": [False, True, True, False],
        "points_after": [24000, 26000, 25000, 25000],
        "kyoutaku_award": {"seat": 1, "sticks": 2, "points": 2000},
    })
    draw_end["results"] = [dict(entry, match_total=0) for entry in GAME_END["results"]]
    award_app._on_frame(draw_end, time.monotonic())
    award_text = result_view._kyoutaku_award_text(award_app.state)
    check(award_text == "场供 2 根 → AI-1 +2000", f"v1.20: 场供归属文案，got {award_text!r}")
    check(result_view._kyoutaku_award_text(round_app.state) == "",
          "v1.20: 非场终局（无 kyoutaku_award）不得多画一行")
    # 播报结束后（阶段 B）才看得到结算页本体：跳过播报再推进时间轴
    future = time.monotonic() + 300.0
    award_app.bc.skip(future)
    for _ in range(5):
        award_app.bc.update(future + 1.0)
    award_app.now = future
    award_surface = award_app.render_frame(present=True)
    award_path = os.path.join(ARTIFACTS, "client_result_kyoutaku_award.png")
    pygame.image.save(award_surface, award_path)
    check(os.path.getsize(award_path) > 1000, f"场供归一位的结算 PNG 已保存：{award_path}")
    check(round_app.state.renchan_count == 0, "局终帧的 renchan_count 已入状态")
    round_app.now = time.monotonic()
    round_app.render_frame(present=False)
    # 局终页点击出口 → 无效果（§9.5：出口只在场终页；局间发帧会撞 BAD_REQUEST）
    btn = T.R_RESULT_REMATCH_BTN
    round_app._on_left_click((btn[0] + btn[2] / 2, btn[1] + btn[3] / 2))
    check(round_app.state.screen == SCREEN_RESULT and round_app.state.match_end is None,
          "局终页点「再来一局」必须无效果")
    check(not any(f[0] in (FRAME_TYPE.CREATE_ROOM, FRAME_TYPE.READY) for f in sent), "局终页出口不发出站帧")
    # D-1：键盘出口同规则——局终页 Enter/ESC 必须无效果
    import pygame as _pg
    round_app._on_key(_pg.event.Event(_pg.KEYDOWN, key=_pg.K_RETURN))
    round_app._on_key(_pg.event.Event(_pg.KEYDOWN, key=_pg.K_ESCAPE))
    check(round_app.state.screen == SCREEN_RESULT and round_app.state.match_end is None,
          "D-1: 局终页 Enter/ESC 不得触发出口")

    # --- 场终页：match_end → 排名表 + 两出口恢复 ---
    round_app._on_frame(dict(MATCH_END), time.monotonic())
    check(round_app.state.match_end is not None, "match_end → 场终页数据就位")
    rankings = round_app.state.match_end.get("rankings")
    check([r["rank"] for r in rankings] == [1, 2, 2, 2], "rankings 并列跳号（1,2,2,2）照协议直渲")
    check(sum(r["final_score"] for r in rankings) == 0, "rankings.final_score 零和")
    round_app.now = time.monotonic()
    round_app.render_frame(present=False)  # 场终页渲染不崩溃

    # --- game_start = 本地牌桌整体重置（新一局）；round_index 推进 ---
    next_start = dict(GAME_START)
    next_start["round_index"] = 1
    next_start["dealer"] = 1
    round_app._on_frame(next_start, time.monotonic())
    check(round_app.state.screen == SCREEN_TABLE, "下一局 game_start → 回到牌桌界面")
    check(round_app.state.round_index == 1 and round_app.state.round_count == 4,
          f"场次进度来自 game_start：{round_app.state.round_index}/{round_app.state.round_count}")
    check(round_app.state.melds == [] and round_app.state.result is None and round_app.state.match_end is None,
          "game_start 整体重置：副露/局终/场终载荷清空")
    check(len(round_app.state.hand) == 14 and round_app.state.dealer == 1, "新一局手牌与庄家重建")

    # --- 自理杠（v2.0 §5.3/§4.6）：按钮顺序 + 提交 tiles 恰 1 牌面 ---
    kan_sync = dict(STATE_SYNC)
    kan_sync["melds"] = [
        {"type": MELD_TYPE.ANKAN, "tiles": ["1z", "1z", "1z", "1z"]},   # 本家暗杠：4 张牌面形态
    ]
    kan_sync["seats"] = [
        dict(STATE_SYNC["seats"][0], melds=[{"type": MELD_TYPE.ANKAN, "tiles": ["1z", "1z", "1z", "1z"]}]),
        dict(STATE_SYNC["seats"][1], melds=[{"type": MELD_TYPE.ANKAN, "tiles": []}]),  # 他家暗杠：空数组形态
        dict(STATE_SYNC["seats"][2], melds=[{"type": MELD_TYPE.KAKAN, "tiles": ["3p", "3p", "3p", "3p"]}]),
        dict(STATE_SYNC["seats"][3]),
    ]
    round_app._on_frame(kan_sync, time.monotonic())
    check(round_app.state.melds[0].type == MELD_TYPE.ANKAN and len(round_app.state.melds[0].tiles) == 4,
          "本家暗杠解析：4 张牌面形态")
    check(round_app.state.seat_view(1).melds[0].type == MELD_TYPE.ANKAN
          and round_app.state.seat_view(1).melds[0].tiles == [],
          "他家暗杠解析：tiles == [] 空数组形态（两种都必须可解析）")
    check(round_app.state.seat_view(2).melds[0].type == MELD_TYPE.KAKAN, "加杠组解析（type=kakan）")
    kan_request = {
        "type": FRAME_TYPE.ACTION_REQUEST,
        "seq": 8,
        "deadline_ms": int(time.time() * 1000) + 15000,
        "window": WINDOW.DISCARD,
        "options": [
            {"action": ACTION.ANKAN, "tiles": ["1z"]},
            {"action": ACTION.KAKAN, "tiles": ["6p"]},
        ],
    }
    round_app._on_frame(kan_request, time.monotonic())
    buttons = actionbar.build_buttons(round_app.state, round_app._remaining_ms())
    kinds = [b.kind for b in buttons]
    check(kinds == [FRAME_TYPE.DISCARD, ACTION.ANKAN, ACTION.KAKAN], f"打牌窗口按钮顺序（§5.3 v2.0）= {kinds}")
    labels = [b.label for b in buttons]
    check(labels[1] == "暗杠", f"暗杠按钮文案不显示牌名（防录屏泄露）：{labels[1]!r}")
    check(labels[2].startswith("加杠 "), f"加杠按钮显示牌名：{labels[2]!r}")
    sent.clear()
    check(round_app._do_action(ACTION.ANKAN, ["1z"]), "暗杠可提交")
    round_app.state.clear_pending()  # PENDING 守卫：同窗口第二个动作前先清（真实流程由服务端帧推进）
    check(round_app._do_action(ACTION.KAKAN, ["6p"]), "加杠可提交")
    kan_frames = [f for f in ([{"type": t, **fields} for t, fields in sent]) if f.get("type") == FRAME_TYPE.ACTION]
    check(all(len(f.get("tiles") or []) == 1 for f in kan_frames),
          f"ankan/kakan 出站 tiles 恰好 1 个牌面元素：{[f.get('tiles') for f in kan_frames]}")
    check(not round_app._do_action(ACTION.ANKAN, ["1z", "1z"]), "暗杠 tiles 多于 1 个 → 拒绝发送")
    check(not round_app._do_action(ACTION.ANKAN, []), "暗杠 tiles 为空 → 拒绝发送")

    # --- 他家牌背容量 14 → 18（§4.6 v2.0：15..18 换行第二行）---
    check(T.OPP_BACK_MAX == 18 and T.OPP_BACK_ROW_CAPACITY == 14, "牌背容量常量 = 18（首行 14）")
    backs = round_app.state.seats
    backs[1].hand_count = 18
    round_app.now = time.monotonic()
    round_app.render_frame(present=False)  # 18 张（含换行第二行）渲染不崩溃、不裁剪
    backs[1].hand_count = 13

    round_app.close_socket("B4 用例结束")

    # ---------------------------------------------------------------- [19] B 批手感
    print("[19] B 批手感：听牌条 / 可吃碰高亮 / 音效 / 动画（FEEL_SPEC v1.7 §2 / §3 / §5 / §6）")

    feel_app = make_app()
    build_table_state(feel_app)

    # --- 音效引擎：16 条预加载、静音与缺资产降级 ---
    from client.audio import SFX as AUDIO_SFX
    from client.audio import AudioEngine

    check(len(AUDIO_SFX) == 16, f"音效 id 恰好 16 条：{len(AUDIO_SFX)}")
    check(feel_app.audio.available, "混音器可用（dummy 驱动下降级也不崩）")
    loaded = sum(1 for v in feel_app.audio.sounds.values() if v is not None)
    check(loaded == 16, f"16 条样本全部加载成功：{loaded}/16")
    check(AudioEngine(assets_dir="/nonexistent-dir").available is False or True,
          "缺资产目录时不抛异常（静音占位）")
    empty_engine = AudioEngine(assets_dir="/nonexistent-dir")
    check(empty_engine.play("win_ron") is False, "缺资产 ⇒ play() 返回 False 且不抛异常")
    check(feel_app.audio.play("tile_discard") in (True, False), "正常引擎 play() 不抛异常")
    feel_app.audio.set_muted(True)
    check(feel_app.audio.play("tile_discard") is False, "静音 ⇒ 不出声（F-AU-25）")
    feel_app.audio.set_muted(False)
    # F-AU-24：(id, seq) 200 ms 去重
    first = feel_app.audio.play("tile_draw", seq=900)
    second = feel_app.audio.play("tile_draw", seq=900)
    check(first is not second or first is False, "同一 (id, seq) 200ms 内重复 ⇒ 去重（F-AU-24）")

    # --- 动画：加速态 / 飞行体上限 / snap ---
    from client.feel import Feel

    feel = Feel()
    feel.note_operation(time.monotonic())
    check(feel.speed() == 2.0, "刚操作完 ⇒ 速度倍率 2.0（F-AN-04）")
    feel._last_op_at = -1e9
    check(feel.speed() == 1.0, "超过 400 ms ⇒ 回落 1.0（F-AN-04）")
    rect_a = (100, 500, 56, 76)
    rect_b = (600, 400, 30, 38)
    started = []
    for i in range(4):
        started.append(feel.start_discard(i, "1m", rect_a, rect_b))
    check(sum(1 for ok in started if ok) == 3, "同屏飞行体上限 3，第 4 个直接落位（F-AN-05）")
    check(len(feel.flights) == 3, f"飞行体数量 = 3：{len(feel.flights)}")
    feel.snap_all()
    check(not feel.flights, "snap_all() 立即清空全部飞行体（F-AN-03）")
    # 单位量纲：duration 必须是「秒」，否则 140ms 会被当成 140s
    feel2 = Feel()
    feel2._last_op_at = -1e9
    feel2.start_discard(0, "1m", rect_a, rect_b)
    check(abs(feel2.flights[0].duration - 0.14) < 1e-6,
          f"出牌动画时长 = 0.14 s（不是 140 s）：{feel2.flights[0].duration}")
    feel2.flights[0].started = time.monotonic()
    check(feel2.flights[0].progress(time.monotonic() + 0.07) > 0.4, "进度按秒推进（量纲正确）")
    # 摸牌进场：起点 dy=16 / 终点 dy=0
    feel3 = Feel()
    feel3._last_op_at = -1e9
    feel3.start_draw(3)
    now0 = time.monotonic()
    feel3.draw_entry.started = now0
    dy0, a0 = feel3.draw_offset(3, now0)
    dy1, a1 = feel3.draw_offset(3, now0 + 0.12)
    check(dy0 > 12 and abs(a0) < 40, f"进场起点 dy≈16 / alpha≈0：({dy0:.1f}, {a0:.1f})")
    check(abs(dy1) < 0.5 and a1 > 250, f"进场终点 dy=0 / alpha=255：({dy1:.1f}, {a1:.1f})")
    check(feel3.draw_offset(4, now0) == (0.0, 255.0), "非摸牌下标不受动画影响")

    # --- --no-feel ---
    nofeel_app = make_app()
    nofeel2 = GameApp(host="127.0.0.1", port=9527, auto_scale=False, no_feel=True)
    check(nofeel2.feel.animations_on is False, "--no-feel ⇒ 动画关闭（F-AN-06）")
    check(nofeel2.feel.start_discard(0, "1m", rect_a, rect_b) is False,
          "--no-feel ⇒ 不起飞，直接按终态绘制")
    check(nofeel_app.feel.animations_on is True, "默认开启动画")
    nofeel2.close_socket("no-feel 用例结束")
    nofeel_app.close_socket("默认动画用例结束")

    # --- 听牌帧 → 状态（§5.16）---
    tp_app = make_app()
    build_table_state(tp_app)
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 1, "is_tenpai": True,
                        "waits": [{"tile": "3p", "remaining": 2}, {"tile": "1z", "remaining": 1}]})
    check(tp_app.state.is_tenpai, "tenpai{is_tenpai:true} ⇒ 状态置位")
    check([w["tile"] for w in tp_app.state.tenpai_waits] == ["3p", "1z"],
          "waits 顺序与服务端一致（客户端不排序，F-TP-03/FE-A1）")
    # 乱序 / 重放：tenpai_seq 不递增 ⇒ 丢弃（§7 R6）
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 1, "is_tenpai": False, "waits": []})
    check(tp_app.state.is_tenpai is True, "过期 tenpai 帧（seq 不递增）被丢弃（§7 R6）")
    # 清空信号
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 2, "is_tenpai": False, "waits": []})
    check(not tp_app.state.is_tenpai and tp_app.state.tenpai_waits == [],
          "is_tenpai=false ⇒ 清空（F-TP-11 ⑧）")
    # remaining 越界（>4）照画不崩
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 3, "is_tenpai": True,
                        "waits": [{"tile": "5m", "remaining": 77}]})
    check(tp_app.state.tenpai_waits[0]["remaining"] == 77,
          "remaining 越界（>4）照画不崩（两个 remaining 混用的探测器）")
    tp_app.now = time.monotonic()
    tp_app.render_frame(present=False)   # 越界值渲染不崩溃
    check(True, "越界 remaining 渲染不崩溃")

    # --- v1.15 §12.10：抢杠窗口（window="chankan"，额外带 tile）---
    ck_app = make_app()
    build_table_state(ck_app)
    ck_app.state.apply({
        "type": FRAME_TYPE.ACTION_REQUEST, "seq": 41, "deadline_ms": int(time.time() * 1000) + 15000,
        "window": WINDOW.CHANKAN, "tile": "5m",
        "options": [{"action": ACTION.WIN, "tiles": []}, {"action": ACTION.PASS, "tiles": []}],
    })
    check(ck_app.state.window is not None and ck_app.state.window.window == WINDOW.CHANKAN,
          "chankan 窗口被接受（不是「window 非法」）")
    check(getattr(ck_app.state.window, "tile", None) == "5m",
          f"抢杠牌面来自帧：{getattr(ck_app.state.window, 'tile', None)}")
    ck_app.state.my_seat = 1
    ck_app.now = time.monotonic()
    buttons = actionbar.build_buttons(ck_app.state, 10000)
    kinds = [b.kind for b in buttons]
    check(ACTION.WIN in kinds and ACTION.PASS in kinds,
          f"抢杠窗口同时给「胡」和「过」：{kinds}")
    # `_status_text` 只读 ui.illegal_count，用一个最小替身即可（渲染层其余部分不参与）
    status = actionbar._status_text(ck_app.state, types.SimpleNamespace(illegal_count=0))[0]
    # tile_label 给的是中文牌名（"5 万"），断言只要求「抢杠 + 这张牌」
    check(status.startswith("抢杠") and "5" in status, f"状态行写明抢杠牌面：{status!r}")
    ck_app.render_frame(present=False)
    check(True, "抢杠窗口渲染不崩溃")
    ck_app.close_socket("抢杠用例结束")

    # --- v1.13 §12.8：日麻「形听但无役」的提示字段 ---
    # classic 帧不带 yaku/no_yaku ⇒ 缺省 yaku=True、no_yaku=False（老渲染路径完全不变）
    check(tp_app.state.tenpai_waits[0].get("yaku", None) is None
          or tp_app.state.tenpai_waits[0]["yaku"] is True and tp_app.state.tenpai_no_yaku is False,
          "classic tenpai 帧不含 yaku 键时按「都有役」渲染")
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 4, "is_tenpai": True, "no_yaku": True,
                        "waits": [{"tile": "6s", "remaining": 3, "yaku": False},
                                  {"tile": "9s", "remaining": 2, "yaku": False}]})
    check(tp_app.state.tenpai_no_yaku is True, "no_yaku=true ⇒ 无役提示置位")
    check([w["yaku"] for w in tp_app.state.tenpai_waits] == [False, False],
          "waits[].yaku 逐张解析（副露无役：两张都荣和不了）")
    check(tp_app.state.is_tenpai is True, "无役不改变 is_tenpai：形听照收罚符、仍可立直")
    tp_app.now = time.monotonic()
    tp_app.render_frame(present=False)   # 灰牌 + 红字 + 尾巴渲染不崩溃
    check(True, "无役听牌条渲染不崩溃")
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 5, "is_tenpai": True, "no_yaku": False,
                        "waits": [{"tile": "1p", "remaining": 3, "yaku": False},
                                  {"tile": "4p", "remaining": 2, "yaku": True}]})
    check([w["yaku"] for w in tp_app.state.tenpai_waits] == [False, True],
          "逐张有役（1p 无役 / 4p 断幺）不被抹平")
    check(tp_app.state.tenpai_no_yaku is False, "no_yaku 只表示「全部无役」")
    tp_app.now = time.monotonic()
    tp_app.render_frame(present=False)
    check(True, "混合有役/无役的听牌条渲染不崩溃")

    # --- v1.24 §12.14：振听（本家私有）也要进提示 ---
    # 缺省：classic / 老服务端的帧没有 furiten 键 ⇒ 恒 False，渲染路径完全不变。
    check(tp_app.state.tenpai_furiten is False, "帧里没有 furiten 键时按「未振听」处理")
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 6, "is_tenpai": True, "no_yaku": False,
                        "furiten": True,
                        "waits": [{"tile": "6s", "remaining": 3, "yaku": True, "furiten": True},
                                  {"tile": "9s", "remaining": 2, "yaku": True, "furiten": False}]})
    check(tp_app.state.tenpai_furiten is True, "furiten=true ⇒ 振听提示置位")
    check([w["furiten"] for w in tp_app.state.tenpai_waits] == [True, False],
          "waits[].furiten 逐张解析（放过 6s 不挡 9s）")
    check(tp_app.state.is_tenpai is True, "振听不改变 is_tenpai：形听照收罚符")
    tp_app.now = time.monotonic()
    tp_app.render_frame(present=False)   # 红边牌 + 尾巴渲染不崩溃
    check(True, "振听听牌条渲染不崩溃")

    # 双向一致：形听消失时 furiten 也要清掉，避免听牌条隐藏后残留提示状态。
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 7, "is_tenpai": False, "furiten": True,
                        "waits": []})
    check(tp_app.state.tenpai_furiten is False, "is_tenpai=false ⇒ furiten 一并清零")

    # F-TP-11 硬列表：game_end / game_start / match_end / 本家副露 / win 均清零
    for name, frame in (
        ("game_end", {"type": FRAME_TYPE.GAME_END, "reason": REASON.WIN_RON, "results": []}),
        ("game_start（下一局）", GAME_START),
        ("match_end", {"type": FRAME_TYPE.MATCH_END, "hands_played": 4, "renchan_count": 0, "rankings": []}),
    ):
        tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 90, "is_tenpai": True,
                            "waits": [{"tile": "2m", "remaining": 3}]})
        tp_app.state.apply(frame)
        check(tp_app.state.tenpai_waits == [], f"F-TP-11：{name} ⇒ 听牌清空")
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 91, "is_tenpai": True,
                        "waits": [{"tile": "2m", "remaining": 3}]})
    tp_app.state.apply({"type": FRAME_TYPE.ACTION_EVENT, "seat": 0, "action": ACTION.PONG,
                        "tiles": ["2m", "2m"], "from_seat": 3})
    check(tp_app.state.tenpai_waits == [], "F-TP-11：本家副露 action_event ⇒ 听牌清空")
    tp_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 92, "is_tenpai": True,
                        "waits": [{"tile": "2m", "remaining": 3}]})
    tp_app.state.apply({"type": FRAME_TYPE.ACTION_EVENT, "seat": 2, "action": ACTION.WIN,
                        "tiles": ["2m"], "from_seat": None})
    check(tp_app.state.tenpai_waits == [], "F-TP-11：和牌 action_event ⇒ 听牌清空")
    tp_app.close_socket("听牌用例结束")

    # --- 可吃碰高亮（F-CL-02）：只算「哪些牌要描边」，不改 options ---
    cl_app = make_app()
    build_table_state(cl_app)
    from client.render.table import claim_hand_faces, claim_highlight_active

    check(claim_highlight_active(cl_app.state), "claim 窗口 + 非 pass 选项 ⇒ 高亮生效")
    got = claim_hand_faces(cl_app.state, cl_app.state.window)
    check(isinstance(got, dict), f"可吃碰牌面集合已算出：{got}")
    before_options = [dict(o) for o in cl_app.state.window.options]
    claim_hand_faces(cl_app.state, cl_app.state.window)
    check([dict(o) for o in cl_app.state.window.options] == before_options,
          "提示层不改 options（§3.1 红线 1）")
    chow_state = cl_app.state
    chow_state.window.options = [
        {"action": ACTION.CHOW, "tiles": ["1m", "2m"]},
        {"action": ACTION.CHOW, "tiles": ["2m", "3m"]},
        {"action": ACTION.PASS, "tiles": []},
    ]
    union = claim_hand_faces(chow_state, chow_state.window)
    check(set(union) == {"1m", "2m", "3m"}, f"吃多解取并集：{sorted(union)}")
    cl_app.close_socket("高亮用例结束")

    # --- 听牌条渲染：开 / 关 / 未听牌隐藏（F-TP-05 / F-TP-10）---
    bar_app = make_app()
    build_table_state(bar_app)
    bar_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 1, "is_tenpai": True,
                         "waits": [{"tile": "3p", "remaining": 3}, {"tile": "7s", "remaining": 1}]})
    bar_app.now = time.monotonic()
    # ⚠️ `render_frame()` 返回的是**复用的同一张** `logical` 画布，比较前必须 `.copy()`，
    # 否则两次拿到的是同一个对象，永远「零差异」（假通过 / 假失败的双重陷阱）。
    surface_on = bar_app.render_frame(present=False).copy()
    bar_app.feel.toggle_tenpai()      # F-TP-10：关闭（仍消费帧）
    bar_app.now = time.monotonic()
    surface_off = bar_app.render_frame(present=False).copy()
    import pygame as _pg

    # ⚠️ 本家**有副露** ⇒ 听牌条上移到 R_TENPAI_SHIFTED（theme 注释的版式冲突处置），
    # 采样区域必须跟随，否则比对的是一块空区域（0 个采样点差异 = 假失败）。
    region_rect = T.R_TENPAI_SHIFTED if bar_app.state.melds else T.R_TENPAI
    region = _pg.Rect(region_rect)
    diff = sum(
        1
        for x in range(region.x, region.right, 3)
        for y in range(region.y, region.bottom, 3)
        if surface_on.get_at((x, y)) != surface_off.get_at((x, y))
    )
    check(diff > 50, f"听牌条开/关在 F-TP-01 区域有可见差异（{diff} 个采样点）")
    check(bar_app.state.is_tenpai is True, "F-TP-10：关闭听牌条后**仍消费** tenpai 帧（缓存不丢）")
    bar_app.feel.toggle_tenpai()
    bar_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 2, "is_tenpai": False, "waits": []})
    bar_app.now = time.monotonic()
    bar_app.feel.tenpai_anim = None      # 跳过淡出，直接看「隐藏」终态
    surface_hidden = bar_app.render_frame(present=False).copy()
    diff2 = sum(
        1
        for x in range(region.x, region.right, 3)
        for y in range(region.y, region.bottom, 3)
        if surface_on.get_at((x, y)) != surface_hidden.get_at((x, y))
    )
    check(diff2 > 50, f"未听牌时整条隐藏（F-TP-05，{diff2} 个采样点差异）")

    # 落盘：听牌条可见 / 关闭 两版牌桌（供人工目视核对 F-TP-01/03 的版位与文案）
    bar_app.state.apply({"type": FRAME_TYPE.TENPAI, "tenpai_seq": 3, "is_tenpai": True,
                         "waits": [{"tile": "3p", "remaining": 3}, {"tile": "7s", "remaining": 1},
                                   {"tile": "1z", "remaining": 2}]})
    bar_app.feel.tenpai_bar_on = True
    bar_app.now = time.monotonic()
    shot_on = os.path.join(ARTIFACTS, "client_table_tenpai.png")
    pygame.image.save(bar_app.render_frame(present=False), shot_on)
    bar_app.feel.tenpai_bar_on = False
    bar_app.now = time.monotonic()
    bar_app.render_frame(present=False)
    bar_app.feel.tenpai_bar_on = True
    check(os.path.getsize(shot_on) > 1000, "牌桌（含听牌条）PNG 已保存：client_table_tenpai.png")
    bar_app.close_socket("听牌条用例结束")

    # --- 四个快捷键（§8 C6）---
    key_app = make_app()
    build_table_state(key_app)

    class _Ev:
        def __init__(self, key: int) -> None:
            self.key = key

    key_app._handle_feel_key(_Ev(_pg.K_t))
    check(key_app.feel.tenpai_bar_on is False, "T 键：切换听牌条")
    key_app._handle_feel_key(_Ev(_pg.K_h))
    check(key_app.feel.claim_hints_on is False, "H 键：切换可吃碰提示")
    key_app._handle_feel_key(_Ev(_pg.K_m))
    check(key_app.feel.muted is True and key_app.audio.muted is True, "M 键：静音（含音频引擎同步）")
    key_app._handle_feel_key(_Ev(_pg.K_n))
    check(key_app.feel.countdown_on is True and key_app.audio.countdown_enabled is True,
          "N 键：倒计时滴答（默认关 ⇒ 打开）")
    check(key_app._handle_feel_key(_Ev(_pg.K_a)) is False, "无关按键不被 T/H/M/N 消费")
    key_app.close_socket("快捷键用例结束")

    # --- 和牌音判定：荣和 / 自摸只认 from_seat（v1.7 F-AU-09/10）---
    win_app = make_app()
    build_table_state(win_app)
    played: list[str] = []

    class _RecAudio:
        countdown_enabled = False
        muted = False

        def play(self, sound_id, seq=None):
            played.append(sound_id)
            return True

        def stop_all(self, fade_ms=40):
            return None

        def set_muted(self, muted):
            self.muted = muted

    win_app.audio = _RecAudio()
    win_app._action_event_feel({"type": FRAME_TYPE.ACTION_EVENT, "seat": 2, "action": ACTION.WIN,
                                "tiles": ["2m"], "from_seat": 1}, time.monotonic())
    check(played == ["win_ron"], f"有 from_seat ⇒ 荣和音（F-AU-09）：{played}")
    played.clear()
    win_app._action_event_feel({"type": FRAME_TYPE.ACTION_EVENT, "seat": 2, "action": ACTION.WIN,
                                "tiles": ["2m"], "from_seat": None}, time.monotonic())
    check(played == ["win_tsumo"], f"无 from_seat ⇒ 自摸音（F-AU-10）：{played}")
    played.clear()
    win_app._action_event_feel({"type": FRAME_TYPE.ACTION_EVENT, "seat": 1, "action": ACTION.PONG,
                                "tiles": ["2m", "2m"], "from_seat": 2}, time.monotonic())
    check(played == ["call_pong"], f"碰 ⇒ 碰音（F-AU-04）：{played}")
    win_app.close_socket("和牌音用例结束")

    # --- 切屏静场 / 出牌动画触发不崩 ---
    misc_app = make_app()
    build_table_state(misc_app)
    now_m = time.monotonic()
    misc_app._start_discard_flight(0)
    check(len(misc_app.feel.flights) <= 3, "discard_event ⇒ 起飞（≤3 个飞行体）")
    misc_app._on_frame({"type": FRAME_TYPE.STATE_SYNC, "seq": 99999, "hand": list(misc_app.state.hand),
                        "melds": [], "seats": [], "remaining": 40, "current_seat": 0}, now_m)
    check(not misc_app.feel.flights, "state_sync ⇒ 动画 snap（§7 R2 / F-AN-03）")
    played_screen: list[str] = []
    misc_app.close_socket("杂项用例结束")

    # --- 结算播报（SETTLEMENT_BROADCAST_RULINGS v1.2 §2.3 / §2.5）---
    # 阶段 A 整幅舞台、六段同帧铺满、高亮逐条推进、跳过不发帧、阶段 A ≤56 次 draw、
    # 倒计时读 interlude_ms 而不是硬编码 5 s。
    from client import broadcast as bc_mod
    from client.render import broadcast as bc_render

    def broadcast_app(game_end_frame: dict) -> tuple[GameApp, list[tuple]]:
        app = make_app()
        sent_frames: list[tuple] = []
        app.send_frame = lambda ftype, **fields: (sent_frames.append((ftype, fields)) or True)
        app.connected = True
        app._on_frame({"type": FRAME_TYPE.LOGIN_OK, "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
        app._on_frame(dict(ROOM_JOINED), time.monotonic())
        app._on_frame(dict(GAME_START), time.monotonic())
        app._on_frame(dict(STATE_SYNC), time.monotonic())
        sent_frames.clear()          # 只统计局间的出站帧
        app._on_frame(game_end_frame, time.monotonic())
        app.now = time.monotonic()
        return app, sent_frames

    bc_app, bc_sent = broadcast_app(dict(GAME_END, interlude_ms=12000))
    plan = bc_app.bc.plan
    check(bc_app.bc.running, "interlude 12000 ⇒ 播报起播（budget 10500 ≥ 6200 档）")
    check(plan is not None and plan.budget_ms == 10500 and plan.level == "L0",
          f"budget = clamp(12000-1500,4200,10500) = 10500，解析 L0：{plan and (plan.level, plan.budget_ms)}")
    tl = bc_app.bc.timeline
    check(tl is not None and tl.total_ms <= tl.budget_ms,
          f"TOTAL ≤ budget：{tl and (tl.total_ms, tl.budget_ms)}")
    # 语音驱动（RULINGS v1.3 §2.1）：逐条停留 = max(1200, 该句实测 + 150)，句首与步切换同时刻
    check(tl.voice, "budget ≥ 6200 档 ⇒ 时间轴走语音驱动")
    check(all(step.dur_ms >= 1200 for step in tl.steps if step.focus and step.focus[0] == "yaku"),
          f"番种步停留 ≥1200 ms：{[(s.focus, s.dur_ms) for s in tl.steps]}")
    check(len(tl.cues) >= 4 and all(c.at_ms <= tl.end_ms for c in tl.cues),
          f"语音队列落在 [0, end] 内：{[(c.at_ms, c.cue) for c in tl.cues]}")
    first_cue_per_step: dict[int, int] = {}
    for cue in tl.cues:
        if cue.step_index >= 0:
            first_cue_per_step.setdefault(cue.step_index, cue.at_ms)
    check(
        all(abs(at - tl.steps[index].at_ms) <= 120 for index, at in first_cue_per_step.items()),
        f"每步首句与步切换时刻差 ≤120 ms："
        f"{[(i, at, tl.steps[i].at_ms) for i, at in sorted(first_cue_per_step.items())]}",
    )
    check(not tl.deleted, f"该局装得下、不删句：{tl.deleted}")
    # §2.8 短局地板（v1.6 定稿）：**只保底、不拉伸** ——
    # `total = max(自然, min(8000, budget-40))`，自然 > 8000 的局一律不动；荒庄豁免。
    natural_ms = tl.total_ms - tl.padded_ms
    floor_ms = max(natural_ms, min(bc_mod.BC_PAD_FLOOR_MS, tl.budget_ms - bc_mod.BC_END_SLACK_MS))
    check(tl.total_ms <= tl.budget_ms, f"总长硬卡 ≤ budget：{tl.total_ms} / {tl.budget_ms}")
    check(tl.total_ms == floor_ms,
          f"短局地板：自然 {natural_ms} → 实际 {tl.total_ms}（应 {floor_ms}，补 {tl.padded_ms}）")

    # 语音 id 自检（team-lead 要求，比听感可靠）：`voice_cues()` 产出的每个 id
    # 必须在 `client/assets/vo/` 里真有一个 `.wav`，且必须在时长表里（否则时长为 0）。
    from client import vo_durations as vo_mod

    def _ge(yaku: list, base: int, dealer: int, total: int, mult: int, per: int, tsumo: bool) -> dict:
        return {
            "type": FRAME_TYPE.GAME_END, "reason": REASON.WIN_TSUMO if tsumo else REASON.WIN_RON,
            "round_index": 0, "dealer": 0, "renchan_count": 0, "match_will_continue": True,
            "interlude_ms": 12000,
            "results": [{"seat": i, "delta_score": 0, "match_total": 0} for i in range(4)],
            "win": {"seat": 1, "tile": "1m", "from_seat": None if tsumo else 0, "tsumo": tsumo,
                    "tags": [], "multiplier": mult, "per_payer": per,
                    "total": per * 3 if tsumo else per,
                    "yaku": [{"id": value_of("yaku", yid), "han": h} for yid, h in yaku], "han": total,
                    "han_breakdown": {"base": base, "tsumo": 0, "dealer": dealer, "total": total},
                    "dealer_bonus": dealer == 1},
        }

    probes: list[dict] = []
    for yid in bc_mod.YAKU_LABEL:
        for h in ((1, 2, 3) if yid == "yakuhai" else (1,)):
            probes.append(_ge([(yid, h)], h, 0, min(6, h), 1 << min(6, h), 1, False))
    for han in range(7):
        probes.append(_ge([("menzen", 1)], han, 0, han, 1 << han, 1, False))
    for mult in (1, 2, 4, 8, 16, 32, 64):
        probes.append(_ge([("menzen", 1)], 1, 0, 1, mult, mult, False))
    for per in (1, 2, 4, 8, 16, 32, 64, 128):
        probes.append(_ge([("menzen", 1)], 1, 0, 1, per, per, per != 1))
    probes.append(_ge([("menzen", 1)], 8, 1, 6, 64, 128, True))          # 封顶(8+1>6) + 庄家
    probes.append({"type": FRAME_TYPE.GAME_END, "reason": REASON.DRAW_EXHAUSTED, "round_index": 0, "dealer": 0,
                   "renchan_count": 0, "match_will_continue": True, "interlude_ms": 12000,
                   "results": [{"seat": i, "delta_score": 0, "match_total": 0} for i in range(4)],
                   "win": None})
    emitted: set[str] = set()
    for frame in probes:
        emitted.update(bc_mod.voice_cues(bc_mod.plan_for(frame)))
    absent = sorted(cue for cue in emitted
                    if not os.path.isfile(os.path.join(GAME_DIR, "client", "assets", "vo", f"{cue}.wav")))
    check(not absent, f"voice_cues() 产出的 {len(emitted)} 个 id 全部有 wav：缺 {absent}")
    check(all(cue in vo_mod.VOICE_MS for cue in emitted),
          f"全部 id 在时长表里（否则时长为 0）：{sorted(emitted - set(vo_mod.VOICE_MS))}")
    check(not any(cue.startswith("vo_yaku_sum_") for cue in emitted),
          "S-2 裁决 B：逐条路径不出现摘要句 vo_yaku_sum_*")
    ui_bc = bc_app.build_ui()
    check(bc_render.is_broadcasting(ui_bc), "阶段 A 判据 = 播报正在跑（不是 plan 存在）")
    check(ui_bc.bc is not None and ui_bc.bc.caption, f"字幕条首句非空：{ui_bc.bc and ui_bc.bc.caption!r}")

    # 六段同帧铺满：plan 的六个区块数据都在（S1 番种 / S2 牌型番 / S3 加成+合计 / S4 封顶 /
    # S5 倍率 / S6 四户 delta）
    check(len(plan.yaku_rows) >= 1 and plan.base is not None and plan.total_han is not None
          and plan.cap_state in (bc_mod.CAP_NONE, bc_mod.CAP_EXACT, bc_mod.CAP_CAPPED)
          and plan.mult is not None and len(plan.delta) == 4,
          "六段数据齐备（S1 番种 / S2 牌型番 / S3 合计 / S4 封顶 / S5 倍率 / S6 四户）")

    # 高亮逐条推进：把时钟往前推，字幕必须随段变化、focus 必须前移
    # （v1.3：逐条停留 ≥1200 ms，所以每轮推进 0.9 s、共 6 轮 ⇒ 走到第 4 步）
    captions: list[str] = []
    focuses: list[object] = []
    for step in range(6):
        bc_app.now = time.monotonic()
        bc_app.bc.update(bc_app.now)
        view = bc_app.bc.view()
        captions.append(view.caption)
        focuses.append(view.focus)
        bc_app.bc._t0 -= 0.9           # 纯逻辑推进（不 sleep）
    check(len({c for c in captions if c}) >= 2, f"字幕随段变化：{captions}")
    check(len({f for f in focuses if f}) >= 2, f"高亮逐条推进：{focuses}")

    # 阶段 A 最坏帧 ≤56 次 draw（RULINGS §2.3 硬约束 2）：8 槽全填 + 当前行高亮
    worst_end = dict(GAME_END, interlude_ms=12000)
    worst_end["win"] = dict(GAME_END["win"])
    worst_end["win"]["yaku"] = [
        {"id": value_of("yaku", yid), "han": 1} for yid in
        ("menzen", "tanyao", "pinfu", "iipeikou", "yakuhai", "rinshan", "sanshoku_doujun", "ittsuu")
    ]
    worst_end["win"]["han_breakdown"] = {"base": 8, "tsumo": 0, "dealer": 1, "total": 6}
    worst_end["win"]["han"] = 6
    worst_end["win"]["multiplier"] = 64
    worst_end["win"]["per_payer"] = 8
    worst_end["win"]["total"] = 24
    worst_app, _worst_sent = broadcast_app(worst_end)
    check(len(worst_app.bc.plan.yaku_rows) == 8, f"8 槽全填：{len(worst_app.bc.plan.yaku_rows)}")
    draw_calls = {"n": 0}
    _orig_draw, _orig_blit = bc_render._draw, bc_render._blit_panel
    _orig_rect, _orig_line = pygame.draw.rect, pygame.draw.line

    def _count_draw(*args, **kwargs):
        draw_calls["n"] += 1
        return _orig_draw(*args, **kwargs)

    def _count_blit(*args, **kwargs):
        draw_calls["n"] += 1
        return _orig_blit(*args, **kwargs)

    def _count_rect(*args, **kwargs):
        draw_calls["n"] += 1
        return _orig_rect(*args, **kwargs)

    def _count_line(*args, **kwargs):
        draw_calls["n"] += 1
        return _orig_line(*args, **kwargs)

    bc_render._draw, bc_render._blit_panel = _count_draw, _count_blit
    pygame.draw.rect, pygame.draw.line = _count_rect, _count_line
    try:
        worst_app.now = time.monotonic()
        worst_app.bc._t0 -= 0.30       # 让第 0 槽成为「当前」（含高亮条 + 当前行底）
        worst_app.bc.update(worst_app.now)
        # 只计**阶段 A 自己**的 draw：牌桌快照 / 蒙层 / 主面板是既有渲染，不计入本预算
        stage_surface = pygame.Surface((T.LOGICAL_W, T.LOGICAL_H))
        bc_render.render_round_end(stage_surface, worst_app.build_ui())
    finally:
        bc_render._draw, bc_render._blit_panel = _orig_draw, _orig_blit
        pygame.draw.rect, pygame.draw.line = _orig_rect, _orig_line
    check(draw_calls["n"] <= 56, f"阶段 A 最坏帧 draw 次数 ≤56：{draw_calls['n']}")

    # v4.5：播报面板与赢家手牌条都不得压住四家摊牌区域
    stage = pygame.Rect(bc_render.R_STAGE)
    win_strip = pygame.Rect(bc_render.R_WIN_HAND)
    for label, rect in (
        ("对家手牌", T.R_OPP_BACK),
        ("上家手牌", T.R_SIDE_L_BACK),
        ("下家手牌", T.R_SIDE_R_BACK),
        ("本家手牌", T.R_SELF_HAND),
    ):
        check(not stage.colliderect(pygame.Rect(rect)), f"播报面板不遮挡{label} {rect}")
        check(not win_strip.colliderect(pygame.Rect(rect)), f"赢家手牌条不遮挡{label} {rect}")

    # 跳过：起播 100ms 内不响应；之后一次走完；**全程不发任何帧**
    early = make_app()
    early.bc.start({"type": FRAME_TYPE.GAME_END, "reason": REASON.WIN_RON, "interlude_ms": 12000}, time.monotonic())
    check(not early.bc.skip(time.monotonic()), "起播 100ms 内不响应跳过（防连点穿透）")
    early_sent: list[tuple] = []
    early.send_frame = lambda ftype, **fields: (early_sent.append((ftype, fields)) or True)
    early.connected = True
    time.sleep(0.12)                    # 用例自身等 100ms 门槛；产品代码里没有 sleep
    skipped = early.bc.skip(time.monotonic())
    check(skipped and early.bc.view().fast_forward, "起播 100ms 后空格/回车/ESC/左键可跳过")
    early.now = time.monotonic()
    early.render_frame(present=False)
    check(early.bc.state == bc_mod.STATE_RUNNING and not early_sent,
          f"跳过期间与跳过之后出站帧必须为 0：{early_sent}")
    early.bc.update(time.monotonic() + 0.5)
    check(early.bc.finished, "180ms 快进 + 220ms 收束后进入 DONE/SKIPPED")
    check(not bc_render.is_broadcasting(early.build_ui()) and not early_sent,
          "播完回到阶段 B（v2.0 静态页），且仍未发帧")

    # 倒计时：播完按 interlude_ms 剩余续算（12 s 档 ⇒ 约 12 → 0），不是硬编码 5
    early.state.result_at = time.monotonic()
    left = result_view._seconds_to_next_round(early.state, early.state.result_at)
    check(left == 12, f"倒计时读 interlude_ms（12000 ⇒ 12 秒），got {left}")
    left_later = result_view._seconds_to_next_round(early.state, early.state.result_at + 9.4)
    check(left_later == 3, f"播完（9.4 s）后剩余约 3 秒，got {left_later}")

    # 三档门槛：interlude 0（既有 gtest/e2e 的 0/300）⇒ 完全不播报，退 v2.0 静态页
    quiet_app, quiet_sent = broadcast_app(dict(GAME_END, interlude_ms=0))
    check(not quiet_app.bc.running and quiet_app.bc.plan is not None
          and not quiet_app.bc.plan.broadcast and not quiet_sent,
          "interlude 0 ⇒ 不播报（budget 4200 < 5100 档），且不发帧")
    # --- 日麻（mode=riichi）结算播报：字幕 / 格子 / 语音 / 阶段 B 明细行四处同口径 ---
    # 回归背景：日麻沿用 classic 六段文案时会念「2 番，2000 倍」「庄家加一番」「按 6 番封顶」，
    # 与 S4「满贯」/ S5「2000 点」/ 阶段 B「合计 2 番 30 符（2000 点）」互相矛盾。
    def _riichi_end(*, tsumo, yaku, han, fu, points, limit, per):
        win = {
            "seat": 0, "tile": "5m", "from_seat": None if tsumo else 2, "tsumo": tsumo,
            "multiplier": points, "per_payer": per, "total": points,
            "yaku": [{"id": value_of("yaku", yid), "han": h} for yid, h in yaku], "han": han,
            "han_breakdown": {"base": sum(h for _, h in yaku), "tsumo": 0, "dealer": 1,
                              "total": han},
            "dealer_bonus": True, "fu": fu, "points": points, "limit": limit,
        }
        return {
            "type": FRAME_TYPE.GAME_END, "reason": REASON.WIN_TSUMO if tsumo else REASON.WIN_RON,
            "round_index": 0, "dealer": 0, "renchan_count": 0, "match_will_continue": True,
            "interlude_ms": 12000, "win": win,
            "results": [{"seat": i, "delta_score": 0, "match_total": 0} for i in range(4)],
        }

    riichi_cases = [
        ("荣和 2番30符", _riichi_end(tsumo=False, yaku=[("riichi", 1), ("tanyao", 1)],
                                    han=2, fu=30, points=2000, limit=LIMIT.NONE, per=2000)),
        ("自摸 满贯", _riichi_end(tsumo=True, yaku=[("riichi", 1), ("honitsu", 3)],
                                 han=4, fu=30, points=8000, limit=LIMIT.MANGAN, per=2000)),
        ("役满 fu=0", _riichi_end(tsumo=False, yaku=[("kokushi", 13)],
                                  han=13, fu=0, points=32000, limit=LIMIT.YAKUMAN, per=32000)),
    ]
    for label, frame in riichi_cases:
        rplan = bc_mod.plan_for(frame)
        captions = rplan.captions()
        joined = " ｜ ".join(captions)
        check(rplan.riichi_mode and rplan.points == frame["win"]["points"],
              f"日麻 {label}：按 fu 键判玩法、点数取 win.points")
        check(all(word not in joined for word in ("倍", "庄家加一番", "自摸不加番", "封顶", "分")),
              f"日麻 {label}：字幕不得出现 classic 的倍/加成/封顶/分：{captions}")
        check(f"{frame['win']['points']} 点" in joined and "符" in joined,
              f"日麻 {label}：字幕必须写符与点数：{captions}")
        check(bc_render._s3_text(rplan) == bc_mod._script_line(rplan.lines, "dealer").caption
              and bc_render._s4_text(rplan) == bc_mod._script_line(rplan.lines, "cap").caption
              and bc_render._s5_text(rplan) in bc_mod._script_line(rplan.lines, "total").caption,
              f"日麻 {label}：S3/S4/S5 格子与段 3/4/5 字幕同源")
        check(not any(cue.startswith(("vo_mult_", "vo_cap", "vo_dealer_bonus",
                                     "vo_tsumo_no_add", "vo_pay_")) for cue in rplan.voice),
              f"日麻 {label}：不念 classic 的倍率/封顶/加成/支付整句：{rplan.voice}")
        check(all(cue in vo_mod.VOICE_MS for cue in rplan.voice),
              f"日麻 {label}：只复用 VO 库里存在的句子：{rplan.voice}")
        detail = result_view._yaku_detail(frame["win"])
        check("庄家 +1" not in detail and f"（{frame['win']['points']} 点）" in detail,
              f"日麻 {label}：阶段 B 明细行不写庄家加成、写点数：{detail}")

    # 役满 fu=0 是判据的关键：用 `fu > 0` 判会把整局误判回 classic（「13 番，32000 倍」）
    yplan = bc_mod.plan_for(riichi_cases[-1][1])
    check(yplan.riichi_mode and "不计符" in yplan.captions()
          and bc_render._s4_text(yplan) == "役满" and bc_render._s5_text(yplan) == "32000 点",
          "日麻役满（fu=0）：仍按日麻播报，不写「0 符」，S4 显示役满")
    # 番值不一致的 classic 整句（混一色两番 vs 日麻混一色 3 番）不得复用
    check("vo_yaku_honitsu" not in bc_mod.plan_for(riichi_cases[1][1]).voice,
          "日麻：役名相同但番值不同的 classic 语音整句不复用（字幕仍显示）")

    # 日麻流局有听牌罚符，「本局不计分 / 四家均不计分」是假话
    riichi_draw = {
        "type": FRAME_TYPE.GAME_END, "reason": REASON.DRAW_EXHAUSTED, "round_index": 0, "dealer": 0,
        "renchan_count": 0, "match_will_continue": True, "interlude_ms": 12000,
        "win": None, "tenpai": [True, False, True, False],
        "points_after": [26000, 24000, 26000, 24000],
        "results": [{"seat": i, "delta_score": d, "match_total": 0}
                    for i, d in enumerate((1500, -1500, 1500, -1500))],
    }
    dplan = bc_mod.plan_for(riichi_draw)
    check(dplan.riichi_mode and "本局不计分" not in dplan.captions()
          and "庄家连庄" in dplan.static_caption and dplan.voice == ("vo_mode_draw",),
          f"日麻流局：字幕报听牌罚符与连庄，不念 classic 的不计分/连庄句：{dplan.captions()}")
    check(all(text == "—" for text in (bc_render._s2_text(dplan), bc_render._s3_text(dplan),
                                      bc_render._s4_text(dplan), bc_render._s5_text(dplan))),
          "日麻流局：S2–S5 四格留空，不出现番/封顶/倍率")

    # classic 逐字不变：仍写「倍」与「分」，日麻分支不得污染老玩法
    classic_captions = bc_mod.plan_for(dict(GAME_END, interlude_ms=12000)).captions()
    check(any("倍" in text for text in classic_captions)
          and any("分" in text for text in classic_captions),
          f"classic 播报文案不变（倍率/分）：{classic_captions}")

    # 阶段 B 收付文案：日麻单位「点」+ 番符，classic 单位「分」（只抓文本，不真渲染）
    from unittest.mock import patch

    def _payment_text(game_end_frame: dict) -> str:
        captured: list[str] = []
        fake_state = types.SimpleNamespace(
            result=game_end_frame, name_of=lambda seat: f"座位{seat}", remaining=70,
        )
        fake_ui = types.SimpleNamespace(
            state=fake_state,
            fonts=types.SimpleNamespace(get=lambda size, bold=False: None),
        )
        win = dict(game_end_frame["win"])
        win.pop("tile", None)       # 跳过和牌张绘制，只留文本
        win["seat"] = None          # 跳过赢家手牌绘制（收付文案在此之前已全部生成）
        with patch.object(result_view.W, "draw_text",
                          side_effect=lambda *args, **kwargs: captured.append(args[1])):
            result_view._render_win_block(pygame.Surface((1, 1)), fake_ui, win)
        return " ｜ ".join(captured)

    r_pay = _payment_text(riichi_cases[0][1])
    c_pay = _payment_text(dict(GAME_END, interlude_ms=12000))
    check("点" in r_pay and "分" not in r_pay and "30 符" in r_pay,
          f"日麻阶段 B：收付写「点」+ 符，不写「分」：{r_pay}")
    check("分" in c_pay and "点" not in c_pay,
          f"classic 阶段 B：收付文案逐字不变（仍写「分」）：{c_pay}")

    check(bc_app.state.screen == SCREEN_RESULT, "播报期停在局终页")
    bc_app.close_socket("播报用例结束")
    worst_app.close_socket("播报用例结束")
    early.close_socket("播报用例结束")
    quiet_app.close_socket("播报用例结束")

    # 两个大厅版面落盘（账号会话带战绩 / 游客会话无战绩）
    for label, account in (("client_lobby.png", True), ("client_lobby_guest.png", False)):
        shot = make_app()
        shot.nickname = "lixueming" if account else "游客_"
        shot.connected = True
        shot._on_frame(
            {"type": FRAME_TYPE.LOGIN_OK, "player_id": "u-000001" if account else "p-3f2a1c",
             "name": "lixueming" if account else "游客_"},
            time.monotonic(),
        )
        if account:
            shot._on_frame(STATS_FRAME, time.monotonic())
        shot.now = time.monotonic()
        pygame.image.save(shot.render_frame(present=False), os.path.join(ARTIFACTS, label))
        check(os.path.getsize(os.path.join(ARTIFACTS, label)) > 1000, f"大厅 PNG 已保存：{label}")
        shot.close_socket("大厅用例结束")

    import pygame as _pygame

    _pygame.quit()

    print()
    if FAILURES:
        print(f"失败 {len(FAILURES)} 项：")
        for item in FAILURES:
            print(f"  - {item}")
        return 1
    print(f"全部通过；产物目录：{ARTIFACTS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
