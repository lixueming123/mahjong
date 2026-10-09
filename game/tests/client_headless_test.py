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

os.environ["SDL_VIDEODRIVER"] = "dummy"
os.environ["SDL_AUDIODRIVER"] = "dummy"
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")

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
    "type": "room_joined",
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
    "type": "game_start",
    "seat": 0,
    "dealer": 0,
    # 庄家 14 张（含字牌 1z/1z/5z/7z；按 §2.2 排序：数牌在前、字牌 1z..7z）
    "hand": ["1m", "1m", "2m", "3m", "4m", "5m", "6p", "6p", "7p", "9s",
             "1z", "1z", "5z", "7z"],
}

# 136 口径（UI v1.3 §0.1）：牌山初始 83；自带 2 组副露（数牌碰 + 字牌杠）= 7 张门前手牌
STATE_SYNC = {
    "type": "state_sync",
    "seq": 5,
    "remaining": 83,
    "current_seat": 1,
    "hand": ["1m", "1m", "2m", "9s", "1z", "5z", "5z"],
    "melds": [
        {"type": "pong", "tiles": ["6p", "6p", "6p"]},
        {"type": "kong", "tiles": ["7z", "7z", "7z", "7z"]},
    ],
    "seats": [
        {"seat": 0, "name": "玩家_", "is_bot": False, "hand_count": 7,
         "discards": ["9s", "4p", "1z"], "melds": [{"type": "pong", "tiles": ["6p", "6p", "6p"]}],
         "disconnected": False},
        {"seat": 1, "name": "AI-1", "is_bot": True, "hand_count": 13,
         "discards": ["3m", "5p", "7z"],
         "melds": [{"type": "pong", "tiles": ["3z", "3z", "3z"]}], "disconnected": False},
        {"seat": 2, "name": "AI-2", "is_bot": True, "hand_count": 11,
         "discards": ["7m", "2s", "2s", "5z"],
         "melds": [{"type": "chow", "tiles": ["3p", "4p", "5p"]},
                   {"type": "kong", "tiles": ["1z", "1z", "1z", "1z"]}],
         "disconnected": False},
        {"seat": 3, "name": "AI-3", "is_bot": True, "hand_count": 13,
         "discards": ["1s", "6z"],
         "melds": [{"type": "kong", "tiles": ["5z", "5z", "5z", "5z"]}], "disconnected": True},
    ],
}

CLAIM_REQUEST = {
    "type": "action_request",
    "seq": 6,
    "deadline_ms": int(time.time() * 1000) + 15000,
    "window": "claim",
    "options": [
        {"action": "win", "tiles": []},
        {"action": "pong", "tiles": ["2s"]},
        {"action": "pass", "tiles": []},
    ],
}

DISCARD_REQUEST = {
    "type": "action_request",
    "seq": 7,
    "deadline_ms": int(time.time() * 1000) + 15000,
    "window": "discard",
    "options": [{"action": "win", "tiles": []}],
}

GAME_END = {
    "type": "game_end",
    "reason": "win_ron",
    "results": [
        {"seat": 0, "delta_score": -4, "hand": [], "win_type": []},
        {"seat": 1, "delta_score": 4,
         "hand": ["1m", "1m", "3m", "3m", "5m", "5m", "7m", "7m", "2p", "2p", "6p", "6p", "8p", "8p"],
         "win_type": ["seven_pairs", "ron"]},
        {"seat": 2, "delta_score": 0, "hand": [], "win_type": []},
        {"seat": 3, "delta_score": 0, "hand": [], "win_type": []},
    ],
    "win": {"seat": 1, "tile": "8p", "from_seat": 0, "tsumo": False,
            "tags": ["seven_pairs"], "multiplier": 4, "per_payer": 4, "total": 4},
}

# 战绩用例（ACCOUNT_DESIGN §3.4 / UI_UX_SPEC v1.7）：四个汇总数字 + 最近 ≤10 局，倒序。
STATS_FRAME = {
    "type": "stats",
    "games": 12,
    "wins": 5,
    "draws": 3,
    "net_score": -14,
    "recent": [
        {"room_id": f"r-{1000 + i:04d}", "reason": reason, "delta_score": delta, "win": delta > 0,
         "ended_at": 1759900000 + i * 600}
        for i, (reason, delta) in enumerate([
            ("win_ron", 12), ("draw_exhausted", 0), ("win_tsumo", 16), ("win_ron", -1), ("win_ron", -1),
            ("draw_exhausted", 0), ("win_ron", -1), ("win_tsumo", 32), ("win_ron", -1), ("draw_exhausted", 0),
        ])
    ],
}

# ------------------------------------------------------------------ 字牌 fixtures
# 结算用例：和牌者 = seat 1，含**字牌杠**（1z×4）、**字牌刻子**（2z×3）+ **字牌做将**（5z×2）
# 物理持有 = 14 + 杠数 1 = 15 张（PROTOCOL §5.10 / UI §9.3）
HONOR_WIN_HAND = ["7m", "7m", "7m", "3p", "4p", "5p",
                  "1z", "1z", "1z", "1z", "2z", "2z", "2z", "5z", "5z"]

HONOR_MELDS_SEAT1 = [
    {"type": "kong", "tiles": ["1z", "1z", "1z", "1z"]},
    {"type": "pong", "tiles": ["2z", "2z", "2z"]},
    {"type": "chow", "tiles": ["3p", "4p", "5p"]},
    {"type": "pong", "tiles": ["7m", "7m", "7m"]},
]

HONOR_STATE_SYNC = {
    "type": "state_sync",
    "seq": 9,
    "remaining": 40,
    "current_seat": 1,
    "hand": ["1m", "1m", "2m", "9s", "1z", "5z", "5z"],
    "melds": [
        {"type": "pong", "tiles": ["6p", "6p", "6p"]},
        {"type": "kong", "tiles": ["7z", "7z", "7z", "7z"]},
    ],
    "seats": [
        {"seat": 0, "name": "玩家_", "is_bot": False, "hand_count": 7,
         "discards": ["9s", "4p", "1z"],
         "melds": [{"type": "pong", "tiles": ["6p", "6p", "6p"]},
                   {"type": "kong", "tiles": ["7z", "7z", "7z", "7z"]}],
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
    "type": "game_end",
    "reason": "win_ron",
    "results": [
        {"seat": 0, "delta_score": -16, "hand": [], "win_type": []},
        {"seat": 1, "delta_score": 16, "hand": HONOR_WIN_HAND, "win_type": ["ron"]},
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
        self.pre_login_frames: list[str] = []   # 登录登记完成前收到的非 login 帧（应为空）
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
        if ftype == "login":
            self.login_received = True     # 仅表示「收到」，登录登记以发出 login_ok 为准
            return
        if not self.logged_in:
            self.pre_login_frames.append(str(ftype))
            if self.enforce_login:
                self.send({"type": "error", "code": "NOT_LOGGED_IN", "message": "send login first"})
            return
        if ftype == "ping" and self.auto_pong:
            self.send({"type": "pong"})

    def send(self, frame: dict) -> None:
        if frame.get("type") == "login_ok":
            self.logged_in = True          # 登录登记完成（与服务端回 login_ok 等价）
        if self.conn is None:
            return
        try:
            self.conn.sendall((json.dumps(frame, ensure_ascii=False) + "\n").encode("utf-8"))
        except OSError:
            pass

    def types(self) -> list[str]:
        return [f.get("type") for f in list(self.received)]

    def type_count(self, ftype: str) -> int:
        return sum(1 for f in list(self.received) if f.get("type") == ftype)

    def wait_for(self, ftype: str, timeout: float = 3.0) -> bool:
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
    `send_frame("login")` 之间的十几毫秒窗口内，服务端按 PROTOCOL §4.1 回
    `NOT_LOGGED_IN`，破坏 GD-C1「全程零 error 帧」。
    """
    if not app.connected or app.net is None:
        return
    if app.last_ping is None or now - app.last_ping >= HEARTBEAT_INTERVAL:
        app.last_ping = now
        app.ping_sent_at = now
        app.send_frame("ping")


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
    pump(app, 3.0, until=lambda: server.wait_for("login", 0.01), max_step=step_times)
    server.send({"type": "login_ok", "player_id": "p-3f2a1c", "name": "玩家_"})
    pump(app, 2.0, until=lambda: app.state.screen == SCREEN_LOBBY, max_step=step_times)
    check(app.state.screen == SCREEN_LOBBY, "login_ok → 停在大厅（v1.8 §12）")
    check(not server.wait_for("create_room", 0.2), "大厅里没有玩家指令前不发 create_room")
    app.start_game()
    pump(app, 2.0, until=lambda: server.wait_for("create_room", 0.01), max_step=step_times)
    server.send(ROOM_JOINED)
    pump(app, 2.0, until=lambda: server.wait_for("ready", 0.01), max_step=step_times)
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
        [m.type for m in app.state.melds] == ["pong", "kong"],
        f"本家副露分组可还原（含字牌杠 4 张）：{[m.type for m in app.state.melds]}",
    )
    check(app.state.remaining == 83, f"牌山剩余 = {app.state.remaining}（136 口径）")
    check(app.state.current_seat == 1, f"当前行动座位 = {app.state.current_seat}")
    check(
        len(app.state.seat_view(2).melds) == 2 and app.state.seat_view(2).melds[0].type == "chow",
        "他家副露分组可还原（chow）",
    )
    check(
        not any(hasattr(v, "hand_tiles") for v in app.state.seats),
        "SeatView 无任何他家牌面字段（私有信息隔离）",
    )
    buttons = actionbar.build_buttons(app.state, app._remaining_ms())
    kinds = [b.kind for b in buttons]
    check(kinds == ["win", "pong", "pass"], f"claim 窗口按钮 = {kinds}（打牌/杠/吃 不可见）")
    check(all(b.enabled for b in buttons), "claim 窗口按钮均可点击")

    table_path = os.path.join(ARTIFACTS, "client_table.png")
    app.now = time.monotonic()
    table_surface = app.render_frame(present=True)
    pygame.image.save(table_surface, table_path)
    check(os.path.getsize(table_path) > 1000, f"牌桌界面 PNG 已保存：{table_path}")

    print("[3] 打牌窗口：手牌可点选、无「过」按钮（H1）")
    app.state.apply(DISCARD_REQUEST)
    app.now = time.monotonic()
    kinds = [b.kind for b in actionbar.build_buttons(app.state, app._remaining_ms())]
    check(kinds == ["discard", "win"], f"discard 窗口按钮 = {kinds}")
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

    # 6 档冻结字号：必须是 SUIT_Z 墨色的真字形，不得退化为「?」占位，且字形不越界
    ink_ok, fit_ok, slots_note = True, True, []
    for label, size, (tw, th) in HONOR_SIZE_SLOTS:
        tile_surf = pygame.Surface((tw, th))
        tile_surf.fill((0, 0, 0))
        widgets_view.draw_tile(tile_surf, app.fonts, (0, 0, tw, th), "1z", honor_size=size)
        pixels = [tile_surf.get_at((x, y))[:3] for x in range(tw) for y in range(th)]
        ink = sum(1 for p in pixels if all(abs(p[i] - T.SUIT_Z[i]) < 12 for i in range(3)))
        placeholder = sum(1 for p in pixels if p[0] > 200 and 60 < p[1] < 140 and 60 < p[2] < 140)
        glyph_img = app.fonts.get(size, bold=True).render(honor_glyph("1z"), True, (255, 255, 255))
        if ink <= 20 or placeholder:
            ink_ok = False
            slots_note.append(f"{label}@{size}px SUIT_Z={ink} ?={placeholder}")
        if glyph_img.get_width() > tw or glyph_img.get_height() > th:
            fit_ok = False
            slots_note.append(f"{label}@{size}px 字形 {glyph_img.get_width()}x{glyph_img.get_height()} > 牌面 {tw}x{th}")
    check(ink_ok, f"6 档字号均以 SUIT_Z{T.SUIT_Z} 渲染真字形、无「?」占位 {slots_note or ''}")
    check(fit_ok, f"字牌字形在 6 档牌位尺寸内不越界 {slots_note or ''}")

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

    # 副露布局不溢出：横排（本家 / 对家）与竖排（上家 / 下家）逐块核验
    overflows: list[str] = []
    for area, melds, vertical, tag in (
        (T.R_SELF_MELD, app.state.melds, False, "本家横排"),
        (T.R_OPP_MELD, seat2_melds, False, "对家横排"),
        (T.R_SIDE_L_MELD, app.state.seat_view(3).melds, True, "上家竖排"),
        (T.R_SIDE_R_MELD, app.state.seat_view(1).melds, True, "下家竖排"),
    ):
        for group_rect, tiles in table_view.meld_layout(area, melds, vertical=vertical):
            if group_rect.x < area[0] or group_rect.right > area[0] + area[2]:
                overflows.append(f"{tag}组框 {group_rect}")
            for (tx, ty, tw, th), face in tiles:
                if tx < area[0] or tx + tw > area[0] + area[2] or ty < area[1] or ty + th > area[1] + area[3]:
                    overflows.append(f"{tag}{face}@({tx},{ty},{tw},{th})")
    check(not overflows, f"字牌副露 3 张 / 4 张在横排与竖排布局内均不溢出 {overflows or ''}")

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
        [m.type for m in honor_groups] == ["kong", "pong", "chow", "pong"],
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
    check(server.wait_for("login", 0.01), f"客户端发出 login；已收到 {server.types()}")
    login_frame = next((f for f in server.received if f.get("type") == "login"), {})
    check(login_frame.get("name") == "玩家_", f"login.name = {login_frame.get('name')!r}")
    if server.raw_lines:
        raw = server.raw_lines[0]
        check(raw.endswith(b"\n"), "出站帧以单个 \\n 结尾（NDJSON）")
        check(raw.decode("utf-8").strip().startswith("{"), "出站帧为 UTF-8 JSON 对象")
    check(server.type_count("create_room") == 1, "整条握手链只发过一次 create_room（大厅点一次）")
    check(server.wait_for("ready", 0.01), "room_joined → 自动 ready")
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
    pump(app, 1.0, until=lambda: app.state.window is not None and app.state.window.window == "discard",
         max_step=step_times)
    check(app.state.window is not None, "收到打牌窗口 action_request")
    app.state.selected_index = 1
    tile_to_discard = app.state.hand[1]
    seq_before = app.state.window.seq
    check(app._confirm_discard(), f"确认打牌 → 发送 discard（tile={tile_to_discard}）")
    pump(app, 1.0, until=lambda: server.wait_for("discard", 0.01), max_step=step_times)
    sent = next((f for f in server.received if f.get("type") == "discard"), None)
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
    pump(app, 1.0, until=lambda: any(f.get("type") == "discard" and f.get("tile", "").endswith("z")
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
    pump(app, 1.0, until=lambda: app.state.window is not None and app.state.window.window == "claim",
         max_step=step_times)
    check(not app._do_action("pong", ["1m"]), "动作不在 options 内 → 拒绝发送（不产生 error 帧）")
    check(not app._do_action("kong", ["6p"]), "claim 窗口无 kong 选项 → 拒绝发送")
    check(app._do_action("pass", []), "claim 窗口的 pass 可发送")
    pump(app, 1.0, until=lambda: server.wait_for("action", 0.01), max_step=step_times)
    act = next((f for f in server.received if f.get("type") == "action"), None)
    check(act is not None and act.get("action") == "pass", f"action 帧 = {act}")
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
    legacy_inbound: list[str] = []
    legacy_errors: list[str] = []
    _legacy_orig_on_frame = legacy_app._on_frame

    def _legacy_spy(frame, now, _orig=_legacy_orig_on_frame, _sink=legacy_inbound, _err=legacy_errors):
        if isinstance(frame, dict):
            _sink.append(str(frame.get("type")))
            if frame.get("type") == "error":
                _err.append(str(frame.get("code")))
        return _orig(frame, now)

    legacy_app._on_frame = _legacy_spy
    legacy_app.connect()
    pump(legacy_app, 3.0, until=lambda: race_stub.wait_for("login", 0.01))
    check(race_stub.wait_for("login", 0.01), "反事实场景：TCP 已连且 login 已发出")
    check(legacy_app.login_stage == "WAIT_LOGIN_OK", "反事实场景：仍在等待 login_ok（正是竞态窗口）")
    legacy_app.last_ping = None          # 与真实窗口一致：心跳从未发过
    _legacy_heartbeat_ping(legacy_app, time.monotonic())
    pump(legacy_app, 1.0, until=lambda: "error" in legacy_inbound)
    check(
        race_stub.wait_for("ping", 0.01),
        f"反事实复现：修复前实现在该窗口发出 ping（服务端收到 {race_stub.types()}）",
    )
    check(
        "ping" in race_stub.pre_login_frames,
        f"反事实复现：该 ping 落在登录登记生效前（pre_login_frames={race_stub.pre_login_frames}）",
    )
    check(
        legacy_errors[:1] == ["NOT_LOGGED_IN"],
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
    pump(fix_app, 3.0, until=lambda: fix_stub.wait_for("login", 0.01))
    check(fix_app.login_stage == "WAIT_LOGIN_OK", "修复后场景：同样处于等待 login_ok 的窗口")
    check(fix_app.logged_in is False, "修复后场景：logged_in 仍为 False（login_ok 未到）")
    # 推进真实心跳若干 tick，并把心跳时钟人为前推 10 s（窗口拖长也不许发 ping）
    pump(fix_app, 1.0)
    for delta in (0.0, HEARTBEAT_INTERVAL * 2, HEARTBEAT_INTERVAL * 4):
        fix_app._heartbeat(time.monotonic() + delta)
    pump(fix_app, 1.0)
    pre_login_out = [t for t in fix_stub.types() if t != "login"]
    check(
        pre_login_out == [] and fix_stub.pre_login_frames == [],
        f"修复后：握手完成前零出站帧（服务端仅收到 {fix_stub.types()}，非 login 帧 {fix_stub.pre_login_frames}）",
    )
    check(
        fix_app.state.error_count == 0,
        f"修复后：该窗口内零 error 帧（error_count={fix_app.state.error_count}）",
    )

    # ③ 登录后心跳必须继续工作：间隔 >= HEARTBEAT_INTERVAL，且 pong 更新 RTT
    fix_inbound: list[str] = []
    _fix_orig_on_frame = fix_app._on_frame

    def _fix_spy(frame, now, _orig=_fix_orig_on_frame, _sink=fix_inbound):
        if isinstance(frame, dict):
            _sink.append(str(frame.get("type")))
        return _orig(frame, now)

    fix_app._on_frame = _fix_spy
    fix_stub.send({"type": "login_ok", "player_id": "p-race", "name": "玩家_"})
    pump(fix_app, 2.0, until=lambda: fix_app.state.screen == SCREEN_LOBBY)
    check(fix_app.logged_in and fix_app.state.screen == SCREEN_LOBBY, "login_ok 后：logged_in=True 且进入大厅")
    fix_app.start_game()   # 旧握手 / 再来一局：create_room 后自动 ready
    pump(fix_app, 2.0, until=lambda: fix_stub.wait_for("create_room", 0.01))
    check(fix_stub.wait_for("create_room", 0.01), "start_game() 继续握手（create_room）")
    pings_before = fix_stub.type_count("ping")
    fix_app._heartbeat(fix_app.last_ping + HEARTBEAT_INTERVAL - 0.5)     # 未到间隔
    pump(fix_app, 0.4)
    check(
        fix_stub.type_count("ping") == pings_before,
        f"登录后：未到 HEARTBEAT_INTERVAL 不发 ping（间隔下限生效，ping={fix_stub.type_count('ping')}）",
    )
    # 回拨心跳时钟使间隔已到（用真实时间，避免污染 RTT 的 ping_sent_at）
    fix_app.last_ping = time.monotonic() - HEARTBEAT_INTERVAL - 1.0
    pump(fix_app, 1.5, until=lambda: fix_stub.type_count("ping") > pings_before)
    check(
        fix_stub.type_count("ping") == pings_before + 1,
        "登录后：达到 HEARTBEAT_INTERVAL 即恢复发送 ping（心跳未退化）",
    )
    pump(fix_app, 1.5, until=lambda: fix_app.rtt_ms is not None)
    check(
        fix_app.rtt_ms is not None and fix_app.rtt_ms >= 0,
        f"登录后：收到 pong 后 RTT 更新（rtt={fix_app.rtt_ms:.2f} ms）",
    )
    check("error" not in fix_inbound, f"整段竞态用例结束仍为零 error 帧（收到 {fix_inbound}）")
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
            inbound: list[str] = []
            outbound: list[tuple[str, bool]] = []
            orig_on_frame, orig_send_frame = real_app._on_frame, real_app.send_frame

            def spy_on_frame(frame, now, _orig=orig_on_frame, _sink=inbound):
                if isinstance(frame, dict):
                    _sink.append(str(frame.get("type")))
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
                real_app.state.error_count == 0 and "error" not in inbound,
                f"端到端零 error 帧：收到 {len(inbound)} 帧、error_count={real_app.state.error_count}"
                f"（帧序 {inbound[:6]}）",
            )
            pre_login_sends = [t for t, logged_in in outbound if t != "login" and not logged_in]
            check(
                pre_login_sends == [],
                f"真实路径握手期零非 login 出站帧（出站序 {[t for t, _ in outbound]}）",
            )
            order_ok = all(x in inbound for x in ("login_ok", "room_joined", "game_start"))
            check(order_ok, f"真实握手链完整：login_ok → room_joined → game_start（{ [x for x in inbound if x in ('login_ok','room_joined','game_start')] }）")
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
        all(tile_label(bad) == "?" for bad in ("0m", "8z", "zz", "", "1M")),
        "非法牌面的牌名为「?」（防御性、不抛异常）",
    )

    # 按钮文案公式逐字对齐 §5.3
    check(button_label("kong", ["5p"]) == "杠 5 筒", f"kong 文案 = {button_label('kong', ['5p'])}")
    check(button_label("pong", ["2s"]) == "碰 2 条", f"pong 文案 = {button_label('pong', ['2s'])}")
    check(button_label("pong", ["1z"]) == "碰 東", f"pong（字牌）文案 = {button_label('pong', ['1z'])}")
    check(button_label("kong", ["7z"]) == "杠 中", f"kong（字牌）文案 = {button_label('kong', ['7z'])}")
    chow_label = button_label("chow", ["3p", "4p"])
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
        [button_label(a) for a in ("win", "pass", "discard")] == ["胡", "过", "打牌"],
        "win/pass/discard 文案 = ['胡', '过', '打牌']",
    )

    # 操作栏按钮实渲染：6 按钮满配（含字牌杠 + 数牌碰 + 两条吃）
    label_window = {
        "type": "action_request", "seq": 31,
        "deadline_ms": int(time.time() * 1000) + 15000, "window": "claim",
        "options": [
            {"action": "win", "tiles": []},
            {"action": "kong", "tiles": ["7z"]},
            {"action": "pong", "tiles": ["2s"]},
            {"action": "chow", "tiles": ["3p", "4p"]},
            {"action": "chow", "tiles": ["4p", "5p"]},
            {"action": "pass", "tiles": []},
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
        [s.kind for s in label_specs] == ["win", "kong", "pong", "chow", "chow", "pass"],
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
        ("pong", ["9s"], "碰 9 条"),
        ("kong", ["1z"], "杠 東"),
        ("chow", ["3p", "4p"], "吃 3 筒 4 筒"),
    ):
        seq += 1
        payload_stub.send({
            "type": "action_request", "seq": seq,
            "deadline_ms": int(time.time() * 1000) + 15000, "window": "claim",
            "options": [{"action": action, "tiles": tiles}, {"action": "pass", "tiles": []}],
        })
        pump(payload_app, 1.0, until=lambda: payload_app.state.window is not None
             and payload_app.state.window.seq == seq, max_step=step_times)
        specs = actionbar.build_buttons(payload_app.state, payload_app._remaining_ms())
        button = next((s for s in specs if s.kind == action), None)
        check(
            button is not None and button.label == expected_label,
            f"按钮文案（显示层）= {button.label if button else None!r}",
        )
        sent_before = [f for f in payload_stub.received if f.get("type") == "action"]
        check(payload_app._do_action(action, tiles), f"提交 {action} {tiles}")
        pump(payload_app, 1.0,
             until=lambda: len([f for f in payload_stub.received if f.get("type") == "action"]) > len(sent_before),
             max_step=step_times)
        sent = [f for f in payload_stub.received if f.get("type") == "action"][-1]
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
    draw_frame = {"type": "draw", "tile": "7m", "seat": 0, "seq": 51}
    toast = drive_toast(draw_frame)
    check(
        toast == "你摸到 7 万" and draw_frame["tile"] == "7m" and "7m" not in toast,
        f"摸牌提示显示「{toast}」而载荷 tile={draw_frame['tile']!r}",
    )
    honor_draw = {"type": "draw", "tile": "1z", "seat": 0, "seq": 52}
    toast = drive_toast(honor_draw)
    check(
        toast == "你摸到 東" and honor_draw["tile"] == "1z",
        f"摸牌提示（字牌繁體）显示「{toast}」而载荷 tile={honor_draw['tile']!r}",
    )

    # ② 打牌提示（本家 / 他家）
    self_discard = {"type": "discard_event", "seat": 0, "tile": "4p", "seq": 53}
    toast = drive_toast(self_discard)
    check(
        toast == "你打出 4 筒" and self_discard["tile"] == "4p" and "4p" not in toast,
        f"本家打牌提示显示「{toast}」而载荷 tile={self_discard['tile']!r}",
    )
    other_discard = {"type": "discard_event", "seat": 1, "tile": "2s", "seq": 54}
    toast = drive_toast(other_discard)
    check(
        toast == "下家 打出 2 条" and other_discard["tile"] == "2s" and "2s" not in toast,
        f"他家打牌提示显示「{toast}」而载荷 tile={other_discard['tile']!r}",
    )

    # ③ 鸣牌事件提示条：结构 `<座位> <动作> <牌名>` 不变，牌名为中文（含字牌繁體）
    for frame, expected, wire in (
        ({"type": "action_event", "seat": 1, "action": "pong", "tiles": ["9s"],
          "from_seat": 0, "seq": 55}, "下家 碰 9 条", "9s"),
        ({"type": "action_event", "seat": 2, "action": "kong", "tiles": ["1z"],
          "from_seat": 0, "seq": 56}, "对家 杠 東", "1z"),
        ({"type": "action_event", "seat": 3, "action": "chow", "tiles": ["3p", "4p"],
          "from_seat": 0, "seq": 57}, "上家 吃 3 筒 4 筒", "3p"),
        ({"type": "action_event", "seat": 1, "action": "win", "tiles": ["8p"],
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
    toast_stub.send({"type": "action_event", "seat": 2, "action": "kong", "tiles": ["1z"],
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
    stats_app._on_frame({"type": "login_ok", "player_id": "p-3f2a1c", "name": "游客_"}, time.monotonic())
    check(not stats_app.has_account, "游客会话（p- 前缀）不显示战绩入口")
    check(not stats_app.build_ui().has_account_session, "游客会话 HUD 不画「战绩」按钮")
    stats_app._on_frame({"type": "login_ok", "player_id": "u-000042", "name": "lixueming"}, time.monotonic())
    check(stats_app.has_account, "账号会话（u- 前缀）显示战绩入口")
    check(stats_app.state.screen == SCREEN_LOBBY, "账号会话登录后同样先进大厅")
    stats_app.state.screen = SCREEN_TABLE  # 本段要验证的是牌桌上的战绩弹窗

    # stats 帧：合法载荷留存、非法载荷忽略（渲染层不做协议校验，但也不该被脏数据打崩）。
    stats_app._on_frame({"type": "stats", "games": "x", "wins": 0, "draws": 0, "net_score": 0, "recent": []},
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
    check(sent == [("stats_request", {})], f"打开面板只发一次 stats_request：{sent}")
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
    lobby_app._on_frame({"type": "login_ok", "player_id": "u-000001", "name": "lixueming"}, logged_at)
    check(lobby_app.state.screen == SCREEN_LOBBY, "账号登录后进入大厅")
    check(lobby_app.state.phase == "大厅", "大厅阶段文案 = 大厅")
    check(
        sent_frames == [("stats_request", {}), ("list_rooms", {})],
        f"大厅为账号会话拉战绩，并立刻拉房间列表：{sent_frames}",
    )
    check(not lobby_app._lobby_begin_enabled(), "还没入座时「开始对局」禁用")
    check(lobby_app._lobby_create_enabled(), "还没入座时「新建房间」可用")

    sent_frames.clear()
    lobby_app._poll_room_list(logged_at)
    check(sent_frames == [], "不到 1 秒不重复发 list_rooms")
    lobby_app._poll_room_list(logged_at + 1.0)
    check(sent_frames == [("list_rooms", {})], f"满 1 秒再发 list_rooms：{sent_frames}")

    sent_frames.clear()
    lobby_app._lobby_click((T.LB_START_BTN[0] + 4, T.LB_START_BTN[1] + 4))
    check(sent_frames == [], "未入座时点「开始对局」不发帧")

    lobby_app._lobby_click((T.LB_NEW_BTN[0] + 4, T.LB_NEW_BTN[1] + 4))
    check(sent_frames == [("create_room", {})], f"「新建房间」只发 create_room：{sent_frames}")
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
    check(sent_frames == [("ready", {})], f"「开始对局」只发 ready：{sent_frames}")

    # 未入座时 Enter = 新建房间；入座后点别的房间不发 join_room。
    pick_app = make_app()
    pick_frames: list[tuple] = []
    pick_app.send_frame = lambda ftype, **fields: (pick_frames.append((ftype, fields)) or True)
    pick_app.connected = True
    pick_app._on_frame({"type": "login_ok", "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    pick_frames.clear()
    pick_app._lobby_key(pygame.event.Event(pygame.KEYDOWN, key=pygame.K_RETURN, unicode=""))
    check(pick_frames == [("create_room", {})], f"未入座时 Enter 等于新建房间：{pick_frames}")
    pick_frames.clear()
    pick_app._on_frame(ROOM_JOINED, time.monotonic())
    check(pick_frames == [], "Enter 建桌成功后不自动 ready")
    pick_app.open_rooms = [
        {"room_id": "r-0001", "host": "玩家_", "players": 1, "seats": 4},
        {"room_id": "r-0002", "host": "甲", "players": 1, "seats": 4},
    ]
    pick_app._on_frame(
        {"type": "room_list", "rooms": [
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
    join_app._on_frame({"type": "login_ok", "player_id": "p-aabbcc", "name": "加入者"}, time.monotonic())
    join_app.open_rooms = [{"room_id": "r-0007", "host": "房主", "players": 1, "seats": 4}]
    join_frames.clear()
    join_app._lobby_click((T.LB_ROOMS_X + 8, T.LB_ROOMS_Y + 8))
    check(join_frames == [("join_room", {"room_id": "r-0007"})], f"点空闲房间发 join_room：{join_frames}")
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
    check(join_frames == [("ready", {})], f"已入座时 Enter 只发 ready：{join_frames}")

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
    rematch_app._on_frame({"type": "login_ok", "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    check(sent_frames == [("create_room", {})], f"「再来一局」跳过大厅直接建桌：{sent_frames}")
    check(not rematch_app.auto_start_after_login, "自动开局标记用后即清（下次登录回大厅）")
    sent_frames.clear()
    rematch_app._on_frame(ROOM_JOINED, time.monotonic())
    check(sent_frames == [("ready", {})], f"「再来一局」在 room_joined 后自动 ready：{sent_frames}")

    # 大厅里断线：还没有对局在进行，不该套「对局进行中，服务端将托管你的座位」那层蒙层
    drop_app = make_app()
    drop_app.nickname = "玩家_"
    drop_app.connected = True
    drop_app._on_frame({"type": "login_ok", "player_id": "p-3f2a1c", "name": "玩家_"}, time.monotonic())
    drop_app._on_closed("服务端关闭了连接", time.monotonic())
    check(drop_app.state.screen == SCREEN_LOGIN, "大厅断线 → 回登录界面")
    check(drop_app.disconnect_reason is None, "大厅断线不显示断线蒙层")
    check("连接已断开" in drop_app.login_status, f"大厅断线在状态行说明原因：{drop_app.login_status!r}")

    # 两个大厅版面落盘（账号会话带战绩 / 游客会话无战绩）
    for label, account in (("client_lobby.png", True), ("client_lobby_guest.png", False)):
        shot = make_app()
        shot.nickname = "lixueming" if account else "游客_"
        shot.connected = True
        shot._on_frame(
            {"type": "login_ok", "player_id": "u-000001" if account else "p-3f2a1c",
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
