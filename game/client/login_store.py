"""本地登录凭据记忆（`UI_UX_SPEC.md` §3.5 / `ACCOUNT_DESIGN.md` §5.7）。

用户诉求：每次启动都得重敲账号密码。这里把**上次成功登录**的账号名与口令存到本机一个
文件里，下次启动直接预填（`main.py` 默认还会自动登录一次）；勾掉「记住密码」即删档。

口径（**演示级**，与 `ACCOUNT_DESIGN.md` §2.2 的口令口径同一取舍）：

- **明文存储、不做加密**：不新增依赖，也不假装安全。目录 `0700`、文件 `0600`，只有本机
  同一个用户能读。要上生产就把 `save()` 换成系统钥匙串（macOS Keychain / Windows DPAPI）。
- **按 `host:port` 分条存**：连本机与连局域网别的机器互不覆盖。
- 写入走「临时文件 + `os.replace`」：进程被 Ctrl-C 打断也不会留下半截 JSON。
- `MAHJONG_LOGIN_STORE=off` 可整体关掉（自动化脚本与排障用，见 `LoginStore.enabled`）。
"""

from __future__ import annotations

import json
import logging
import os
import tempfile

log = logging.getLogger("mahjong.client.login_store")

STORE_VERSION = 1
#: 与 `app.py` 的 NICKNAME_MAX / PASSWORD_MAX 同口径。此处独立一份，避免 app ↔ store 循环依赖。
USERNAME_MAX = 16
PASSWORD_MAX = 64
DEFAULT_DIRNAME = ".ex_actor_mahjong"
DEFAULT_FILENAME = "login.json"
#: 关掉凭据记忆的环境变量；取值 `off` / `0` / `false` / `no` 时 `LoginStore` 全程 no-op。
DISABLE_ENV = "MAHJONG_LOGIN_STORE"


class LoginStore:
    """一个 JSON 文件 = 若干条「host:port → {username, password}」。

    所有失败都**吞掉并降级成「没记住」**：凭据记忆是便利功能，不能因为它写不了盘就让
    客户端起不来（磁盘满、家目录只读、文件被手改成乱码都在此列）。
    """

    def __init__(self, path: str | os.PathLike[str] | None = None) -> None:
        flag = os.environ.get(DISABLE_ENV, "").strip().lower()
        self.enabled = flag not in ("off", "0", "false", "no")
        if path is None:
            self.path = os.path.join(os.path.expanduser("~"), DEFAULT_DIRNAME, DEFAULT_FILENAME)
        else:
            self.path = os.fspath(path)

    @staticmethod
    def key(host: str, port: int) -> str:
        """键就是用户看得懂的那一行「目标服务器」：同机不同端口算两台服务器。"""
        return f"{host}:{int(port)}"

    def load(self, host: str, port: int) -> tuple[str, str] | None:
        """返回 `(username, password)`；没有存档、存档不合法或被关掉时返回 `None`。"""
        if not self.enabled:
            return None
        entry = self._read().get(self.key(host, port))
        if entry is None:
            return None
        username = entry.get("username")
        password = entry.get("password")
        if not isinstance(username, str) or not isinstance(password, str):
            return None
        if not username or len(username) > USERNAME_MAX or len(password) > PASSWORD_MAX:
            return None
        return username, password

    def save(self, host: str, port: int, username: str, password: str) -> None:
        """覆盖这条 `host:port` 的存档；其它服务器的条目原样保留。"""
        if not self.enabled or not username or not password:
            return
        accounts = self._read()
        accounts[self.key(host, port)] = {"username": username, "password": password}
        self._write(accounts)

    def clear(self, host: str, port: int) -> None:
        """删掉这条 `host:port` 的存档（取消勾选「记住密码」时调用）。"""
        if not self.enabled:
            return
        accounts = self._read()
        if accounts.pop(self.key(host, port), None) is None:
            return
        self._write(accounts)

    # ------------------------------------------------------------ 内部

    def _read(self) -> dict[str, dict]:
        try:
            with open(self.path, encoding="utf-8") as handle:
                raw = json.load(handle)
        except FileNotFoundError:
            return {}
        except (OSError, ValueError) as exc:
            log.warning("登录凭据文件不可读（%s），按「没有记住」处理：%s", self.path, exc)
            return {}
        if not isinstance(raw, dict):
            return {}
        accounts = raw.get("accounts")
        if not isinstance(accounts, dict):
            return {}
        return {str(key): value for key, value in accounts.items() if isinstance(value, dict)}

    def _write(self, accounts: dict[str, dict]) -> None:
        directory = os.path.dirname(os.path.abspath(self.path))
        payload = {"version": STORE_VERSION, "accounts": accounts}
        try:
            os.makedirs(directory, mode=0o700, exist_ok=True)
            handle, tmp_path = tempfile.mkstemp(dir=directory, prefix=".login-", suffix=".json")
            try:
                with os.fdopen(handle, "w", encoding="utf-8") as tmp:
                    json.dump(payload, tmp, ensure_ascii=False, indent=2)
                # mkstemp 已是 0600；显式再设一次，避免 umask 或文件系统默认值把它放宽。
                os.chmod(tmp_path, 0o600)
                os.replace(tmp_path, self.path)
            except BaseException:
                # 半截文件不留：否则它既不是合法 JSON，也会被下次 mkstemp 落在同一目录里。
                try:
                    os.unlink(tmp_path)
                except OSError:
                    pass
                raise
        except OSError as exc:
            log.warning("登录凭据写盘失败（%s），本次不记忆：%s", self.path, exc)
