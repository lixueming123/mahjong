#!/usr/bin/env python3
"""登录凭据记忆验收（`UI_UX_SPEC.md` §3.5 / `ACCOUNT_DESIGN.md` §5.7）。

覆盖：

1. `LoginStore` 磁盘口径：写入/读取/删除、按 `host:port` 分条、文件权限 `0600`、
   坏 JSON 降级为「没记住」（不抛异常）、`MAHJONG_LOGIN_STORE=off` 全程 no-op；
2. `GameApp` 预填：有存档 → 账号模式 + 两个框已填 + 「登录」可用，且**不建连接**；
3. `GameApp` 自动登录（`auto_login=True`）：直接进 `CONNECTING`，握手帧是
   `login{username,password}` 且口令取自存档；
4. 「记住密码」勾选态决定落盘；取消勾选**立刻**删掉本机存档；
5. 登录界面账号模式渲染：「记住密码」勾选框勾/未勾在像素上可见（勾上时框内是
   `ACTIVE` 金），账号模式下渲染不崩。

运行：
    ./.venv/bin/python game/tests/login_memory_test.py
"""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile

os.environ["SDL_VIDEODRIVER"] = "dummy"
os.environ["SDL_AUDIODRIVER"] = "dummy"
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
# 本测试必须真的读写临时文件：别被调用者环境里的开关（或它指向的真家目录）搅乱。
os.environ.pop("MAHJONG_LOGIN_STORE", None)

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = os.path.dirname(HERE)
if GAME_DIR not in sys.path:
    sys.path.insert(0, GAME_DIR)

from client import theme as T  # noqa: E402
from client.app import GameApp  # noqa: E402
from client.login_store import DISABLE_ENV, LoginStore  # noqa: E402

FAILURES: list[str] = []

ACCOUNT = "lixueming"
PASSWORD = "hunter2!"
#: 连一个必然拒绝的端口：预填/自动登录的断言都不依赖真实服务端。
DEAD_PORT = 1


def check(condition: bool, label: str) -> None:
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {label}")
    if not condition:
        FAILURES.append(label)


def make_app(store: LoginStore | None, auto_login: bool = False) -> GameApp:
    return GameApp(
        host="127.0.0.1",
        port=DEAD_PORT,
        auto_scale=False,
        login_store=store,
        auto_login=auto_login,
    )


# ---------------------------------------------------------------- 1. 磁盘口径


def test_store(tmpdir: str) -> None:
    print("[1] LoginStore 磁盘口径")
    path = os.path.join(tmpdir, "login.json")
    store = LoginStore(path)

    check(store.load("127.0.0.1", DEAD_PORT) is None, "空存档 → load 返回 None（不抛异常）")
    store.save("127.0.0.1", DEAD_PORT, ACCOUNT, PASSWORD)
    check(
        store.load("127.0.0.1", DEAD_PORT) == (ACCOUNT, PASSWORD),
        f"写完立刻读回：{store.load('127.0.0.1', DEAD_PORT)!r}",
    )
    mode = stat.S_IMODE(os.stat(path).st_mode)
    check(mode == 0o600, f"存档权限 0600（实际 {oct(mode)}）")

    store.save("10.0.0.2", DEAD_PORT, "bob", "secret12")
    check(store.load("10.0.0.1", DEAD_PORT) is None, "别的服务器（host 不同）不会串号")
    other = store.load("10.0.0.2", DEAD_PORT)
    check(other == ("bob", "secret12"), f"按 host:port 分条存：{other!r}")

    store.clear("127.0.0.1", DEAD_PORT)
    check(store.load("127.0.0.1", DEAD_PORT) is None, "clear 删掉命中的那条")
    check(
        store.load("10.0.0.2", DEAD_PORT) == ("bob", "secret12"),
        "clear 不影响其它服务器的条目",
    )

    # 坏文件：手改成乱码 / 结构不对，都必须降级成「没记住」，且之后还能正常写。
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("{not json")
    check(store.load("10.0.0.2", DEAD_PORT) is None, "坏 JSON → 按「没记住」处理")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump({"version": 1, "accounts": ["不是字典"]}, handle)
    check(store.load("10.0.0.2", DEAD_PORT) is None, "结构不对 → 按「没记住」处理")
    store.save("127.0.0.1", DEAD_PORT, ACCOUNT, PASSWORD)
    check(
        store.load("127.0.0.1", DEAD_PORT) == (ACCOUNT, PASSWORD),
        "坏文件之后仍能写新存档（自愈）",
    )

    # 环境变量总开关：自动化脚本与排障时用，全程不读不写。
    os.environ[DISABLE_ENV] = "off"
    try:
        disabled = LoginStore(path)
        before = os.stat(path).st_mtime_ns
        disabled.save("127.0.0.1", DEAD_PORT, "carol", "secret34")
        check(
            disabled.enabled is False
            and disabled.load("127.0.0.1", DEAD_PORT) is None
            and os.stat(path).st_mtime_ns == before,
            f"{DISABLE_ENV}=off → 读不到也写不进",
        )
    finally:
        os.environ.pop(DISABLE_ENV, None)


# ---------------------------------------------------------------- 2. 预填


def test_prefill(tmpdir: str) -> None:
    print("[2] 有存档时启动预填（不自动连接）")
    store = LoginStore(os.path.join(tmpdir, "prefill.json"))
    store.save("127.0.0.1", DEAD_PORT, ACCOUNT, PASSWORD)
    app = make_app(store)
    try:
        check(app.login_mode == "account", f"自动切到账号模式（实际 {app.login_mode}）")
        check(
            app.account == ACCOUNT and app.password == PASSWORD,
            f"两个框已填：{app.account!r} / {'●' * len(app.password)}",
        )
        check(app.remember_password, "「记住密码」默认勾着")
        check(app._login_submit_enabled(), "预填完整 ⇒ 「登录」按钮可用")
        check(app.login_stage == "IDLE" and app.net is None, "只预填：不建连接、不发帧")
        check(ACCOUNT in app.login_status, f"状态行说明记住了谁：{app.login_status!r}")
    finally:
        app.shutdown()


def test_no_store() -> None:
    print("[2′] 没注入 LoginStore（测试/嵌入式）⇒ 行为与改动前一致")
    app = make_app(None)
    try:
        check(app.login_mode == "guest" and not app.account, "默认仍是游客模式、账号框为空")
        check(app.nickname == "" and not app._login_submit_enabled(), "游客「进入牌桌」仍置灰")
    finally:
        app.shutdown()


# ---------------------------------------------------------------- 3. 自动登录


def test_auto_login(tmpdir: str) -> None:
    print("[3] auto_login=True：启动即用存档登录")
    store = LoginStore(os.path.join(tmpdir, "auto.json"))
    store.save("127.0.0.1", DEAD_PORT, ACCOUNT, PASSWORD)
    app = make_app(store, auto_login=True)
    try:
        check(app.login_stage == "CONNECTING", f"直接进连接阶段（实际 {app.login_stage}）")
        frame_type, fields = app.pending_auth or ("", {})
        check(
            frame_type == "login" and fields.get("username") == ACCOUNT,
            f"握手第一帧是账号登录：{(frame_type, fields.get('username'))!r}",
        )
        check(fields.get("password") == PASSWORD, "口令取自存档（不是空串）")
    finally:
        app.close_socket("测试结束")
        app.shutdown()


def test_bad_saved_credentials(tmpdir: str) -> None:
    print("[3′] 存档不合规（例如换了版本口径）⇒ 只预填，不自动发帧")
    store = LoginStore(os.path.join(tmpdir, "bad.json"))
    store.save("127.0.0.1", DEAD_PORT, "a", "1")  # 账号 1 位、口令 1 位：过不了 §3.3
    app = make_app(store, auto_login=True)
    try:
        check(app.login_stage == "IDLE" and app.pending_auth is None, "不提交：停在可编辑的登录界面")
        check(app.account == "a" and app.password == "1", "字段仍然填回，用户自己改就行")
    finally:
        app.shutdown()


# ---------------------------------------------------------------- 4. 落盘与删档


def test_remember_toggle(tmpdir: str) -> None:
    print("[4] 勾选态决定落盘 / 取消勾选立即删档")
    store = LoginStore(os.path.join(tmpdir, "toggle.json"))
    app = make_app(None)  # 先拿一个不落盘的实例，避免预填干扰
    app.login_store = store
    try:
        app.account, app.password = ACCOUNT, PASSWORD
        app._submitted_credentials = (ACCOUNT, PASSWORD)
        app.remember_password = True
        app._remember_credentials()
        check(
            store.load("127.0.0.1", DEAD_PORT) == (ACCOUNT, PASSWORD),
            "勾着 ⇒ 账号登录成功后落盘",
        )

        app.remember_password = False
        store.clear("127.0.0.1", DEAD_PORT)
        app._remember_credentials()
        check(store.load("127.0.0.1", DEAD_PORT) is None, "未勾 ⇒ 不落盘")

        app.remember_password = True
        store.save("127.0.0.1", DEAD_PORT, ACCOUNT, PASSWORD)
        app._toggle_remember()  # True → False
        check(not app.remember_password, "点一下勾选框：勾选态翻转")
        check(store.load("127.0.0.1", DEAD_PORT) is None, "取消勾选立刻删掉本机存档")
        check("取消记住密码" in app.login_status, f"状态行给出反馈：{app.login_status!r}")

        app._toggle_remember()  # False → True
        check(app.remember_password and "已勾选" in app.login_status, "再点一下恢复勾选")
    finally:
        app.shutdown()


# ---------------------------------------------------------------- 5. 渲染


def test_render_checkbox(tmpdir: str) -> None:
    print("[5] 账号模式渲染 + 勾选框像素")
    store = LoginStore(os.path.join(tmpdir, "render.json"))
    store.save("127.0.0.1", DEAD_PORT, ACCOUNT, PASSWORD)
    app = make_app(store)
    try:
        # 勾选框内一个不受「勾」笔画影响的采样点（框 = 16×16，左上角 (700,394)）
        sample = (702, 396)
        app.remember_password = True
        checked = app.render_frame(present=False).get_at(sample)
        app.remember_password = False
        unchecked = app.render_frame(present=False).get_at(sample)
        check(
            tuple(checked)[:3] == T.ACTIVE,
            f"勾上时框内是 ACTIVE 金：{tuple(checked)[:3]}",
        )
        check(
            tuple(unchecked)[:3] != T.ACTIVE,
            f"未勾时框内不是金色：{tuple(unchecked)[:3]}",
        )
        app.remember_password = True
        surface = app.render_frame(present=False)
        check(
            surface.get_size() == (T.LOGICAL_W, T.LOGICAL_H),
            f"账号模式整屏渲染不崩：{surface.get_size()}",
        )
    finally:
        app.shutdown()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mahjong-login-") as tmpdir:
        test_store(tmpdir)
        test_prefill(tmpdir)
        test_no_store()
        test_auto_login(tmpdir)
        test_bad_saved_credentials(tmpdir)
        test_remember_toggle(tmpdir)
        test_render_checkbox(tmpdir)
    if FAILURES:
        print(f"\n登录凭据记忆 FAILED：{len(FAILURES)} 项")
        for item in FAILURES:
            print(f"  - {item}")
        return 1
    print("\n登录凭据记忆 OK：存档 0600、预填/自动登录、勾选态落盘与删除、渲染全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
