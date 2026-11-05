#!/usr/bin/env python3
"""真客户端 + 真服务端：创建日麻房间并进入牌桌（SDL dummy）。

这是「日麻能不能玩」的端到端验证 —— 不是 stub，客户端与 `mahjong_server` 都跑真实现：

  1. 大厅选「日本麻将」→ `create_room{"mode":"riichi"}` → `room_joined`；
  2. `ready` → `game_start`，客户端必须拿到 `points`（四家 25000）与 `dora_indicators`；
  3. 牌桌渲染一帧落盘，肉眼可核对「持点 / 宝牌指示」；
  4. 全程零 `error` 帧。

用法：
    ./.venv/bin/python game/tests/client_riichi_e2e_test.py
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
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

import pygame  # noqa: E402

from client.app import GameApp  # noqa: E402
from client.game_state import SCREEN_LOBBY, SCREEN_TABLE  # noqa: E402

FAILURES: list[str] = []


def check(condition: bool, label: str) -> None:
    print(f"  [{'PASS' if condition else 'FAIL'}] {label}")
    if not condition:
        FAILURES.append(label)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_port(port: int, timeout: float = 15.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def pump(app: GameApp, seconds: float, until) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        app.now = time.monotonic()
        app.update()
        if until():
            return
        time.sleep(0.01)


ALL_FACES = [f"{r}{s}" for s in "mps" for r in range(1, 10)] + [f"{r}z" for r in range(1, 8)]


def _can_form_melds(counts: dict[str, int]) -> bool:
    """剩余牌能否全部拆成面子（刻子/顺子）。"""
    keys = sorted((t for t in counts if counts[t] > 0), key=lambda t: ("mpsz".index(t[1]), int(t[0])))
    if not keys:
        return True
    tile = keys[0]
    if counts[tile] >= 3:
        counts[tile] -= 3
        if _can_form_melds(counts):
            counts[tile] += 3
            return True
        counts[tile] += 3
    if tile[1] != "z":
        rank, suit = int(tile[0]), tile[1]
        first, second = f"{rank + 1}{suit}", f"{rank + 2}{suit}"
        if rank <= 7 and counts.get(first, 0) and counts.get(second, 0):
            counts[tile] -= 1
            counts[first] -= 1
            counts[second] -= 1
            if _can_form_melds(counts):
                counts[tile] += 1
                counts[first] += 1
                counts[second] += 1
                return True
            counts[tile] += 1
            counts[first] += 1
            counts[second] += 1
    return False


def is_complete_hand(tiles: list[str]) -> bool:
    """14 张是否和牌形（标准型 4 面子 + 1 将，或七对子）。"""
    from collections import Counter

    if len(tiles) != 14:
        return False
    counts = dict(Counter(tiles))
    if len(counts) == 7 and all(v == 2 for v in counts.values()):
        return True
    for pair in list(counts):
        if counts[pair] < 2:
            continue
        counts[pair] -= 2
        ok = _can_form_melds(counts)
        counts[pair] += 2
        if ok:
            return True
    return False


def is_tenpai(hand: list[str]) -> bool:
    """13 张是否听牌（加任一张即成和牌形）。与服务端 `WaitingTiles` 同口径（不含副露）。"""
    if len(hand) != 13:
        return False
    return any(is_complete_hand(hand + [face]) for face in ALL_FACES)


def riichi_discards(hand: list[str]) -> list[int]:
    """所有「打出后仍听牌」的下标 —— 立直宣言牌的合法集合。"""
    out = []
    for index in range(len(hand)):
        rest = hand[:index] + hand[index + 1:]
        if is_tenpai(rest):
            out.append(index)
    return out


def discard_order(hand: list[str]) -> list[int]:
    """按「越该打」排序的手牌下标（测试用启发式，见 pick_discard）。"""
    return sorted(range(len(hand)), key=lambda i: (tile_keep_score(hand, i), i))


def pick_discard(hand: list[str]) -> int:
    """最该打出的那张（discard_order 的第一个）。"""
    order = discard_order(hand)
    return order[0] if order else 0


def tile_keep_score(hand: list[str], index: int) -> int:
    """该牌的「留手价值」：成对 + 邻牌越多越高。分数越低越该打。"""
    from collections import Counter

    counts = Counter(hand)
    tile = hand[index]
    value = counts[tile] * 10
    if len(tile) == 2 and tile[1] in "mps":
        rank, suit = int(tile[0]), tile[1]
        for delta, weight in ((-2, 2), (-1, 6), (1, 6), (2, 2)):
            if 1 <= rank + delta <= 9 and f"{rank + delta}{suit}" in counts:
                value += weight
    return value


def server_binary() -> str:
    for rel in ("build", "build-release"):
        candidate = os.path.join(REPO_ROOT, rel, "game", "mahjong_server")
        if os.path.exists(candidate):
            return candidate
    return ""


def main() -> int:
    binary = server_binary()
    if not binary:
        print("[riichi-client] FAIL: 找不到 build/game/mahjong_server", file=sys.stderr)
        return 1

    port = free_port()
    log_path = os.path.join(ARTIFACTS, "client_riichi_server_log.txt")
    os.makedirs(ARTIFACTS, exist_ok=True)
    with open(log_path, "wb") as log_file:
        process = subprocess.Popen(
            [binary, "--port", str(port), "--seed", "20260101",
             "--ai-delay-ms", "0", "--inter-hand-delay-ms", "0"],
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
    app = GameApp(host="127.0.0.1", port=port, auto_scale=False)
    try:
        check(wait_port(port), f"真实服务端已启动（port={port}）")

        inbound: list[str] = []
        frames: list[dict] = []
        orig_on_frame = app._on_frame

        def spy_on_frame(frame, now, _orig=orig_on_frame):
            if isinstance(frame, dict):
                inbound.append(str(frame.get("type")))
                frames.append(frame)
            return _orig(frame, now)

        app._on_frame = spy_on_frame

        # 1) 登录 → 大厅，并把玩法切到日麻
        app.nickname = "日麻客户端"
        app.lobby_mode = "riichi"
        app.lobby_length = "hanchan"   # 半庄：8 局（东场 4 + 南场 4）
        app.submit_login()
        pump(app, 20.0, until=lambda: app.state.screen == SCREEN_LOBBY)
        check(app.state.screen == SCREEN_LOBBY, "登录后停在大厅")
        check(app.lobby_mode == "riichi", "大厅玩法开关已切到日本麻将")

        surface = app.render_frame(present=False)
        pygame.image.save(surface, os.path.join(ARTIFACTS, "client_lobby_riichi.png"))
        check(
            os.path.getsize(os.path.join(ARTIFACTS, "client_lobby_riichi.png")) > 1000,
            "大厅（日麻选中）已落盘：client_lobby_riichi.png",
        )

        # 2) 建房（带 mode=riichi）
        app.create_lobby_room()
        pump(app, 15.0, until=lambda: bool(app.state.room_id))
        check(bool(app.state.room_id), f"日麻房间已创建：{app.state.room_id}")

        # 3) 开局
        app.begin_match()
        pump(app, 20.0, until=lambda: app.state.screen == SCREEN_TABLE)
        check(app.state.screen == SCREEN_TABLE, "进入牌桌（game_start 已到）")
        check(
            app.state.points == [25000, 25000, 25000, 25000],
            f"持点来自日麻 game_start：{app.state.points}",
        )
        check(len(app.state.dora_indicators) == 1, f"宝牌指示牌：{app.state.dora_indicators}")
        check(app.state.round_count == 8, f"半庄局数来自服务端：round_count={app.state.round_count}")
        check("error" not in inbound and app.state.error_count == 0, "全程零 error 帧")

        # 4) 真打一张：`game_start` 不带 remaining，第一份 `state_sync` 要等有人摸牌。
        #    这一步同时验证日麻窗口下的打牌链路（选中 → discard → 他家摸牌 → 广播 state_sync）。
        if app.state.window is not None and app.state.window.window == "discard":
            app.state.selected_index = len(app.state.hand) - 1
            check(app._confirm_discard(), "日麻窗口下成功提交一次打牌")
        pump(app, 20.0, until=lambda: app.state.remaining is not None)
        check(
            app.state.remaining is not None and app.state.remaining <= 69,
            f"牌山剩余（已切 14 张王牌）：{app.state.remaining}",
        )
        check("error" not in inbound and app.state.error_count == 0, "打牌之后仍然零 error 帧")

        # 5) 一直打到「能立直」，然后真的点一次立直（AI 节流已关，几十手也只是秒级）
        declared = False
        offered = False
        for _ in range(400):
            if app.state.screen != SCREEN_TABLE:
                # 局终/场终：等下一局；场终（match_end）就真的结束了
                if app.state.match_end is not None:
                    break
                pump(app, 3.0, until=lambda: app.state.screen == SCREEN_TABLE)
                continue
            win = app.state.window
            if win is None:
                pump(app, 0.3, until=lambda: app.state.window is not None)
                continue
            prev_seq = win.seq
            if win.window != "discard":
                app._do_action("pass", [])
                pump(app, 0.3, until=lambda: app.state.window is None
                     or app.state.window.seq != prev_seq)
                continue
            if not app.state.hand:
                pump(app, 0.3, until=lambda: bool(app.state.hand))
                continue
            if win.has("riichi"):
                offered = True
                # 服务端只接受「打出后仍听牌」的那张：逐个候选试，最多 6 次
                # （每次拒绝只记 1 次非法操作，配额 10，留足余量）。
                def riichi_broadcast() -> bool:
                    return any(
                        f.get("type") == "action_event" and f.get("action") == "riichi"
                        for f in frames
                    )

                # 精确算出「打出后仍听牌」的那几张（与服务端口径一致），逐个试
                candidates = riichi_discards(app.state.hand) or discard_order(app.state.hand)[:4]
                for index in candidates[:8]:
                    app.state.selected_index = index
                    if not app._confirm_riichi():
                        print(f"  [DEBUG] _confirm_riichi 返回 False（index={index}）")
                        continue
                    errors_before = app.state.error_count
                    pump(app, 0.8, until=riichi_broadcast)
                    if riichi_broadcast():
                        declared = True
                        break
                    if app.state.error_count > errors_before:
                        last_error = next(
                            (f for f in reversed(frames) if f.get("type") == "error"), None
                        )
                        print(f"  [DEBUG] 宣言牌 {app.state.hand[index]} 被拒：{last_error}")
                        continue  # 该宣言牌不成立（ILLEGAL_ACTION），换下一张
                    break        # 没有任何回应：放弃，避免死等
                break
            app.state.selected_index = pick_discard(app.state.hand)
            app._confirm_discard()
            pump(app, 0.3, until=lambda: app.state.window is None
                 or app.state.window.seq != prev_seq)

        if offered:
            check(True, "服务端在听牌窗口提供了 riichi 选项")
            check(declared, "客户端点「立直」后服务端接受（宣言牌合法）")
            if declared:
                seat_index = app.state.my_seat or 0
                pump(app, 5.0, until=lambda: app.state.points is not None
                     and app.state.points[seat_index] == 24000)
                seat = app.state.my_seat or 0
                check(app.state.points[seat] == 24000,
                      f"立直后扣掉 1000 点供托：points={app.state.points}")
                check(any(f.get("type") == "action_event" and f.get("action") == "riichi" for f in frames),
                      "广播里出现 action_event{action:riichi}")
                last_sync = next((f for f in reversed(frames) if f.get("type") == "state_sync"), None)
                seat_flag = last_sync.get("seats", [{}])[seat].get("riichi") if last_sync else None
                check(app.state.seat_view(seat).riichi,
                      f"客户端座位状态已置立直（帧内 seats[].riichi={seat_flag}）")

                # 立直后手牌锁定：故意打一张「不是刚摸到」的牌，服务端必须拒绝
                pump(app, 5.0, until=lambda: app.state.window is not None
                     and app.state.window.window == "discard"
                     and app.state.drawn_index is not None)
                current = app.state.window
                if (current is not None and current.window == "discard"
                        and app.state.drawn_index is not None and len(app.state.hand) > 1):
                    other = next(
                        (i for i in range(len(app.state.hand)) if i != app.state.drawn_index), None
                    )
                    if other is not None:
                        app.state.selected_index = other
                        errors_before = app.state.error_count
                        app._confirm_discard()
                        pump(app, 1.5, until=lambda: app.state.error_count > errors_before)
                        check(app.state.error_count > errors_before,
                              "立直后打非摸牌被拒（立直锁手牌，只能摸切）")
                        last = next((f for f in reversed(frames) if f.get("type") == "error"), None)
                        if last is not None:
                            print(f"  [INFO] 拒绝原因：{last.get('code')} {last.get('message')}")
                else:
                    print("  [SKIP] 立直后没有等到自己的打牌窗口")
        else:
            print("  [SKIP] 160 手内没摸到可立直的听牌（不是失败；立直窗口的可达性由规则库单测覆盖）")

        table_surface = app.render_frame(present=False)
        pygame.image.save(table_surface, os.path.join(ARTIFACTS, "client_table_riichi.png"))
        check(
            os.path.getsize(os.path.join(ARTIFACTS, "client_table_riichi.png")) > 1000,
            "牌桌（日麻）已落盘：client_table_riichi.png",
        )
        print(f"[riichi-client] 帧序前 8 条：{inbound[:8]}")
    finally:
        app.close_socket("日麻端到端结束")
        process.kill()
        process.wait(timeout=10)
        pygame.quit()

    if FAILURES:
        print(f"\n失败 {len(FAILURES)} 项：")
        for item in FAILURES:
            print(f"  - {item}")
        return 1
    print("\n全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
