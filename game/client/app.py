"""主循环与交互（UI_UX_SPEC §1.2 / §2 / §3 / §5.4 / §8 / §11「app」）。

职责：
- 事件 → 意图（选牌 / 确认打出 / 执行动作）→ 经 §5.5 的守卫校验后交给 `net_client` 发送；
- 处理入站帧队列（≤256 帧/帧）；
- 登录后停在大厅：`新建房间` / 点列表加入只换到 `room_joined`，`开始对局` 才发 `ready`；
  「再来一局」仍跳过大厅，`create_room` 之后自动 `ready`；
- 连接失败自动重试 1/2/4 s（§8.2）、断线蒙层（§8.4）、错误条与横幅（§8.1/§8.5）；
- 主循环任何路径都不得阻塞在网络上（连接也在后台线程完成）。
"""

from __future__ import annotations

import logging
import os
import queue
import time

import pygame

from . import theme as T
from .audio import AudioEngine
from .broadcast import VOICE_ENABLED, Broadcast
from .feel import Feel
from .game_state import (SCREEN_LOGIN, SCREEN_LOBBY, SCREEN_RESULT, SCREEN_TABLE, WINDOW_CHANKAN,
                         WINDOW_CLAIM, WINDOW_DISCARD,
                         GameState)
from .login_store import LoginStore
from .net_client import NetClient
from .render import (actionbar, lobby as lobby_view, login as login_view, overlays, result as result_view,
                     stats as stats_view, table as table_view, tenpai as tenpai_view)
from .tiles import button_label, tile_face, tile_label
from .view import UiView

log = logging.getLogger("mahjong.client.app")

FRAMES_PER_TICK = 256
HEARTBEAT_INTERVAL = 5.0
LOGIN_OK_TIMEOUT = 3.0
# 账号登录要过一次数据库查询，给它比游客登录更宽的等待窗口（ACCOUNT_DESIGN §5）。
ACCOUNT_LOGIN_TIMEOUT = 8.0
GAME_START_TIMEOUT = 10.0
# UI_UX_SPEC §12.2：停在大厅时约每 1 秒拉一次未开局房间。
LIST_ROOMS_INTERVAL = 1.0
INBOUND_SILENCE_LIMIT = 20.0
RETRY_DELAYS = (1.0, 2.0, 4.0)
NICKNAME_MAX = 16
# ACCOUNT_DESIGN §3.1：账号名 3..16 且仅 [A-Za-z0-9_]；口令 6..64 可见 ASCII。
ACCOUNT_NAME_MIN = 3
ACCOUNT_NAME_MAX = 16
PASSWORD_MIN = 6
PASSWORD_MAX = 64
TOAST_DURATION = 2.0
ERROR_STRIP_DURATION = 4.0
# 「再来一局」撞上服务端还没释放账号时的短重试（UI_UX_SPEC §9.5 第 7 条）。
REMATCH_RETRY_LIMIT = 5
REMATCH_RETRY_DELAY = 0.3
# 结算播报的时间轴诊断开关（`MAHJONG_BC_TRACE=1`）：逐帧打印「第几步在什么时刻被触发」。
# 默认关，不影响任何逻辑；出问题时开它就能看到 S0→S7 的真实时间戳。
BROADCAST_TRACE = os.environ.get("MAHJONG_BC_TRACE") == "1"

ERROR_TEXT = {
    "INVALID_NAME": "昵称非法：需 1..16 个字符，且不含换行或控制字符",
    "NOT_LOGGED_IN": "会话未建立（客户端缺陷）",
    "ROOM_NOT_FOUND": "房间不存在或已关闭",
    "NOT_YOUR_TURN": "还没轮到你",
    "NO_PENDING_ACTION": "当前没有需要你处理的动作",
    "ILLEGAL_ACTION": "该动作当前不可用",
    "UNKNOWN_TILE": "牌不在你的手牌中（操作可能已过期）",
    "BAD_REQUEST": "请求格式错误（客户端缺陷）",
    "FRAME_TOO_LARGE": "发送帧过大",
    "RATE_LIMITED": "操作过于频繁，请稍候",
    "INTERNAL": "服务端内部错误",
    "AUTH_FAILED": "账号或密码错误",
    "USERNAME_TAKEN": "账号名已被占用",
    "ALREADY_ONLINE": "该账号已在别处登录",
}
# MySQL 不可用时服务端对账号帧统一回 INTERNAL（ACCOUNT_DESIGN §2.3），文案按该 message 区分。
ACCOUNT_UNAVAILABLE_TEXT = "账号服务暂不可用"


class GameApp:
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 9527,
        fullscreen: bool = False,
        *,
        auto_scale: bool = True,
        enable_display: bool = True,
        no_feel: bool = False,
        login_store: LoginStore | None = None,
        auto_login: bool = False,
    ) -> None:
        self.host = host
        self.port = port
        self.fullscreen = fullscreen
        # B 批（FEEL_SPEC §6 F-AN-06）：`--no-feel` ⇒ 不播任何动画，直接按终态绘制。
        self.feel = Feel(animations_enabled=not no_feel)
        # 结算播报 S1（RULINGS §2.5）：`--no-feel` 完全不播报，退 v2.0 静态页。
        # 时间轴只吃 `time.monotonic()` 的**绝对时间**（不消费 dt、不调 `feel.scaled()`）。
        self.bc = Broadcast(enabled=not no_feel)
        pygame.init()
        try:
            pygame.font.init()
        except pygame.error:  # pragma: no cover - 防御性
            log.warning("字体模块初始化失败")
        self.fonts = T.FontBook()
        self.clock = pygame.time.Clock()
        # 音效引擎（FEEL_SPEC §5）：启动期一次性预加载 16 条；无声卡/缺资产静默降级。
        self.audio = AudioEngine()
        self.audio.set_muted(self.feel.muted)
        # 延迟音效队列：`(到期 monotonic, sound_id, seq)`（F-AU-11/12/15/16 的 60/80/100/120 ms）
        self._pending_sfx: list[tuple[float, str, object]] = []
        self._turn_start_announced = False
        self._last_countdown_bucket: int | None = None
        self._was_tenpai = False
        # 播报语音：`_voice_live` 记录「上一帧有语音在响」，用于一次性收声（不每帧 stop）
        self._voice_live = False
        self._bc_trace_step: int | None = None
        self.running = True
        self.now = time.monotonic()

        self.screen_surface: pygame.Surface | None = None
        self.logical = pygame.Surface((T.LOGICAL_W, T.LOGICAL_H))
        self.scale = 1.0
        self.offset = (0, 0)
        if enable_display:
            self._init_display(auto_scale)
        # 打开 Unicode 文本输入后，已提交的字符走 TEXTINPUT。拼音等组字串走 TEXTEDITING，
        # 不能再从 KEYDOWN.unicode 收，否则「爱」会连同前面的 "ai" 一起进昵称。
        pygame.key.start_text_input()

        # 状态
        self.state = GameState()
        self.net: NetClient | None = None
        self.nickname = ""
        self.ime_composition = ""
        self.input_focus = True
        # ACCOUNT_DESIGN §5：默认游客模式，既有的握手链路与既有测试因此原样成立。
        self.login_mode = "guest"
        self.account = ""
        self.password = ""
        self.login_focus = "name"
        self.login_password_error = False
        # 提交时定下的登录意图（("login", {...}) / ("register", {...})），连接建立后发出。
        self.pending_auth: tuple[str, dict] | None = None
        # 战绩（ACCOUNT_DESIGN §3.4）：保存最近的 `stats` 载荷本身，渲染时才格式化。
        self.stats: dict | None = None
        self.stats_open = False
        # 结算页「再来一局」置位：登录成功后跳过大厅直接建桌（§9.5 第 7 条）。
        self.auto_start_after_login = False
        # 仅 `start_game()`（再来一局 / 旧握手）在 `room_joined` 后自动 `ready`。
        # 大厅「新建房间」和点击加入都不置这个标记。
        self.auto_ready_on_join = False
        # 大厅玩法选择（classic / riichi）：只影响 create_room 的 mode 字段。
        self.lobby_mode = "classic"
        # 日麻局数（tonpu 东场 / hanchan 半庄）：只影响 create_room 的 length 字段。
        self.lobby_length = "tonpu"
        self.open_rooms: list[dict] = []
        self.last_list_rooms_at = 0.0
        # 只有账号会话才有战绩可查（游客的 stats 是零值，显示出来只会让人困惑）。
        self.has_account = False
        # 「再来一局」的 ALREADY_ONLINE 短重试余量（§9.5 第 7 条）。
        self.rematch_retries_left = 0
        self.login_status = "请输入昵称后点击「进入牌桌」"
        self.login_status_color = T.TEXT_DIM
        self.login_input_error = False
        self.login_stage = "IDLE"
        self.login_retry_visible = False
        self.connect_attempts = 0
        self.retry_at: float | None = None
        self.stage_since = 0.0
        self.connected = False
        self.invalid_name_count = 0
        self.rtt_ms: float | None = None
        self.last_ping: float | None = None
        self.ping_sent_at: float | None = None
        # 「已收到 login_ok」的状态位（t28）：握手完成前禁止发送心跳等非 login 帧。
        self.logged_in = False
        self.throttle_until = 0.0
        self.error_until = 0.0
        self.error_code: str | None = None
        self.error_message: str | None = None
        self.toast_text = ""
        self.toast_until = 0.0
        self.banner_text: str | None = None
        self.banner_color = T.WARN
        self.banner_action: str | None = None
        self.disconnect_reason: str | None = None
        self.zero_since: float | None = None
        self.inbound_stall_since: float | None = None
        self.pending_flash_until = 0.0
        self._empty_hand_synced = False
        self.center_note_until = 0.0
        self.mouse_pos = (0, 0)
        self.mouse_down = False
        self.debug = False
        self._pressed_button: tuple[str, list[str]] | None = None

        # 登录凭据记忆（UI_UX_SPEC §3.5）：`login_store` 由入口注入，默认 `None` = 不落盘
        # （测试与嵌入式调用因此零副作用）。`auto_login` 只在有存档时生效。
        self.login_store = login_store
        self.remember_password = True
        # 提交出去的那一对凭据。握手期间输入框仍可编辑，落盘时不能拿 `self.password` 兜底。
        self._submitted_credentials: tuple[str, str] | None = None
        self._restore_saved_login(auto_login)

    # ------------------------------------------------------------ 显示

    def _init_display(self, auto_scale: bool) -> None:
        flags = pygame.FULLSCREEN if self.fullscreen else 0
        size = (T.LOGICAL_W, T.LOGICAL_H)
        if auto_scale and not self.fullscreen:
            try:
                info = pygame.display.Info()
                sw, sh = int(info.current_w or 0), int(info.current_h or 0)
            except pygame.error:
                sw = sh = 0
            if sw > 0 and sh > 0 and (sw < 1360 or sh < 800):
                self.scale = T.compute_scale(sw, sh)
                size = T.scaled_size(self.scale)
                log.info("屏幕 %dx%d 偏小，按 scale=%.3f 缩放并居中留黑边", sw, sh, self.scale)
        try:
            self.screen_surface = pygame.display.set_mode(size, flags)
        except pygame.error as exc:  # pragma: no cover - 无显示设备时的兜底
            log.error("创建窗口失败（%s），退化为离屏渲染", exc)
            self.screen_surface = None
        if self.fullscreen and self.screen_surface is not None:
            self.scale = min(
                self.screen_surface.get_width() / T.LOGICAL_W,
                self.screen_surface.get_height() / T.LOGICAL_H,
            )
        if self.screen_surface is not None:
            self.offset = (
                (self.screen_surface.get_width() - int(T.LOGICAL_W * self.scale)) // 2,
                (self.screen_surface.get_height() - int(T.LOGICAL_H * self.scale)) // 2,
            )
        pygame.display.set_caption(T.WINDOW_TITLE)

    def to_logical(self, pos: tuple[float, float]) -> tuple[float, float]:
        """§1.1：鼠标输入必须用与渲染同一变换的反函数。"""
        if self.scale <= 0:
            return pos
        return ((pos[0] - self.offset[0]) / self.scale, (pos[1] - self.offset[1]) / self.scale)

    # ------------------------------------------------------------ 网络

    def connect(self) -> None:
        """开始一次连接（后台线程，主循环不阻塞）。"""
        self.close_socket("重新连接")
        self.login_stage = "CONNECTING"
        self.logged_in = False
        self.last_ping = None
        self.stage_since = time.monotonic()
        self.login_status = f"连接中… {self.host}:{self.port}"
        self.login_status_color = T.TEXT_DIM
        self.login_retry_visible = False
        self.net = NetClient(self.host, self.port)
        self.net.connect_async(3.0)

    def close_socket(self, reason: str) -> None:
        if self.net is not None:
            self.net.close(reason)
            self.net.join(0.5)
        self.connected = False
        self.logged_in = False
        self.last_ping = None

    def send_frame(self, ftype: str, **fields) -> bool:
        if self.net is None or not self.connected:
            log.debug("未连接，拒绝发送 %s", ftype)
            return False
        if time.monotonic() < self.throttle_until and ftype not in ("ping",):
            log.warning("限速节流中，丢弃出站帧 %s", ftype)
            return False
        frame = {"type": ftype}
        frame.update(fields)
        ok = self.net.send(frame)
        if ok:
            log.debug("→ %s", frame)
        return ok

    # ------------------------------------------------------------ 主循环

    def run(self) -> None:
        while self.running:
            self.now = time.monotonic()
            for event in pygame.event.get():
                self.handle_event(event)
            self.update()
            self.render_frame(present=True)
            self.clock.tick(T.TARGET_FPS)
        self.shutdown()

    def shutdown(self) -> None:
        self.close_socket("窗口关闭")
        pygame.quit()

    # ------------------------------------------------------------ 事件

    def handle_event(self, event: pygame.event.Event) -> None:
        if event.type == pygame.QUIT:
            self.running = False
            return
        if event.type == pygame.MOUSEMOTION:
            self.mouse_pos = self.to_logical(event.pos)
            return
        if event.type == pygame.MOUSEBUTTONDOWN:
            self.mouse_pos = self.to_logical(event.pos)
            self.mouse_down = True
            if event.button == 1:
                self._on_left_click(self.mouse_pos)
            elif event.button == 3:
                self._on_right_click()
            return
        if event.type == pygame.MOUSEBUTTONUP:
            self.mouse_pos = self.to_logical(event.pos)
            self.mouse_down = False
            self._pressed_button = None
            return
        if event.type == pygame.TEXTEDITING:
            self.ime_composition = event.text
            return
        if event.type == pygame.TEXTINPUT:
            self.ime_composition = ""
            if self.state.screen == SCREEN_LOGIN:
                self._insert_login_text(event.text)
            return
        if event.type == pygame.KEYDOWN:
            self._on_key(event)
            return

    def _on_left_click(self, pos: tuple[float, float]) -> None:
        if self.disconnect_reason:
            if T.rect_contains(overlays.DISCONNECT_BTN, pos):
                self.return_to_login()
            return
        # 播报期左键任意位置 = 跳过（RULINGS §2.5）。**不发任何帧**（局间出站帧必须为 0）。
        if self._broadcast_skippable() and self.bc.skip(time.monotonic()):
            return
        if self.banner_action:
            btn = overlays.banner_button_rect(self._bar_visible())
            if btn is not None and T.rect_contains(btn, pos):
                self._run_banner_action()
                return
        if self.state.screen == SCREEN_LOGIN:
            self._login_click(pos)
        elif self.state.screen == SCREEN_LOBBY:
            self._lobby_click(pos)
        elif self.state.screen == SCREEN_RESULT:
            # v2.0（§9.5）：两出口**只在场终页**可用；局终页（match_end 未到）不响应，
            # 避免局间发帧撞上服务端 BAD_REQUEST。
            if self.state.match_end is None:
                return
            if T.rect_contains(T.R_RESULT_REMATCH_BTN, pos):
                self._sfx("ui_click")   # F-AU-01
                self.rematch()
                return
            if T.rect_contains(T.R_RESULT_BACK_BTN, pos):
                self._sfx("ui_click")   # F-AU-01
                self.go_to_lobby()
        elif self.state.screen == SCREEN_TABLE:
            self._table_click(pos)

    def _on_right_click(self) -> None:
        if self.state.screen == SCREEN_TABLE and not self.state.pending:
            self.state.selected_index = None

    def _on_key(self, event: pygame.event.Event) -> None:
        if event.key == pygame.K_F1:
            self.debug = not self.debug
            return
        # B 批四个开关（FEEL_SPEC §8 C6）：只在牌桌生效——登录页的 T/H/M/N 是账号/昵称的字符。
        if self.state.screen == SCREEN_TABLE and self._handle_feel_key(event):
            return
        if self.disconnect_reason:
            if event.key in (pygame.K_RETURN, pygame.K_ESCAPE):
                self.return_to_login()
            return
        if self.state.screen == SCREEN_LOGIN:
            self._login_key(event)
        elif self.state.screen == SCREEN_LOBBY:
            self._lobby_key(event)
        elif self.state.screen == SCREEN_TABLE:
            self._table_key(event)
        elif self.state.screen == SCREEN_RESULT:
            # 播报期：SPACE / RETURN / ESC 一律是「跳过播报」，且**不发任何帧**（RULINGS §2.5）。
            if self._broadcast_skippable() and event.key in (
                pygame.K_SPACE, pygame.K_RETURN, pygame.K_KP_ENTER, pygame.K_ESCAPE,
            ):
                if self.bc.skip(time.monotonic()):
                    return
            # Enter 是最常用的那个（再来一局），ESC 走「返回大厅」（§9.5）。
            # D-1 守卫（§9.5 v2.0）：出口只在**场终页**可用——键盘与鼠标同规则，
            # 局终页（match_end 未到）按 Enter/ESC 不得触发任何出口。
            if self.state.match_end is None:
                return
            if event.key == pygame.K_RETURN:
                self.rematch()
            elif event.key == pygame.K_ESCAPE:
                self.go_to_lobby()

    # ------------------------------------------------------------ 登录

    def _restore_saved_login(self, auto_login: bool) -> None:
        """启动时把上次**成功登录**的账号口令填回输入框；`auto_login` 时顺带提交一次。

        没存档就什么都不做（游客界面原样）。存档的长度/字符集与 §3.3 不符时（例如换了
        版本口径）只预填不提交，避免一开机就弹一行红字。
        """
        if self.login_store is None:
            return
        saved = self.login_store.load(self.host, self.port)
        if saved is None:
            return
        account, password = saved
        self.login_mode = "account"
        self.account = account
        self.password = password
        self.login_focus = "name"
        self.login_status = f"已记住 {account}，点「登录」继续"
        self.login_status_color = T.TEXT_DIM
        if auto_login and self._account_credentials_valid(account, password):
            self.submit_login()

    @staticmethod
    def _account_credentials_valid(account: str, password: str) -> bool:
        """§3.3 的账号名/口令口径，纯判定、不写状态行（给「要不要自动登录」用）。"""
        return (
            ACCOUNT_NAME_MIN <= len(account) <= ACCOUNT_NAME_MAX
            and all(c.isascii() and (c.isalnum() or c == "_") for c in account)
            and PASSWORD_MIN <= len(password) <= PASSWORD_MAX
            and all(0x20 <= ord(c) <= 0x7E for c in password)
        )

    def _remember_credentials(self) -> None:
        """登录成功且勾着「记住密码」→ 落盘；取消勾选时归档已在 `_toggle_remember` 删掉。"""
        if self.login_store is None or not self.remember_password:
            return
        credentials = self._submitted_credentials
        if credentials is None:
            return
        self.login_store.save(self.host, self.port, credentials[0], credentials[1])

    def _toggle_remember(self) -> None:
        """「记住密码」勾选框：勾上 = 下次自动填入；取消 = 立刻删掉本机存档。"""
        self.remember_password = not self.remember_password
        if self.remember_password:
            self.login_status = "已勾选：登录成功后记住账号，下次自动填入"
            self.login_status_color = T.TEXT_DIM
            return
        if self.login_store is None:
            self.login_status = "已取消：这次登录不记住账号"
        else:
            self.login_store.clear(self.host, self.port)
            self.login_status = "已取消记住密码：本机存档已删除"
        self.login_status_color = T.TEXT_DIM

    def _login_key(self, event: pygame.event.Event) -> None:
        # 账号模式下有两个输入框，按键按焦点路由（input_focus 到 UiView 的 login_focus）。
        field = "password" if (self.login_mode == "account" and self.login_focus == "password") else "name"
        # 组字过程中的退格/回车交给输入法；回车确认候选时不能顺势提交登录。
        if self.ime_composition and event.key in (
            pygame.K_BACKSPACE,
            pygame.K_RETURN,
            pygame.K_KP_ENTER,
            pygame.K_ESCAPE,
        ):
            return
        if event.key == pygame.K_BACKSPACE:
            self._set_login_field(field, self._login_field(field)[:-1])
        elif event.key == pygame.K_ESCAPE:
            self._set_login_field(field, "")
        elif event.key in (pygame.K_RETURN, pygame.K_KP_ENTER):
            self.submit_login()
        elif event.key == pygame.K_TAB and self.login_mode == "account":
            self.login_focus = "password" if field == "name" else "name"

    def _insert_login_text(self, text: str) -> None:
        field = "password" if (self.login_mode == "account" and self.login_focus == "password") else "name"
        limit = PASSWORD_MAX if field == "password" else NICKNAME_MAX
        value = self._login_field(field)
        for char in text:
            if not char.isprintable() or ord(char) < 0x20:
                continue
            if len(value) >= limit:
                self.login_status = f"最多 {limit} 个字符"
                self.login_status_color = T.NEG
                break
            value += char
        self._set_login_field(field, value)

    def _login_field(self, field: str) -> str:
        if field == "password":
            return self.password
        return self.account if self.login_mode == "account" else self.nickname

    def _set_login_field(self, field: str, value: str) -> None:
        if field == "password":
            self.password = value
            self.login_password_error = False
        elif self.login_mode == "account":
            self.account = value
            self.login_input_error = False
        else:
            self.nickname = value
            self.login_input_error = False

    def _login_click(self, pos: tuple[float, float]) -> None:
        if T.rect_contains(T.L_MODE_GUEST, pos):
            self._set_login_mode("guest")
            return
        if T.rect_contains(T.L_MODE_ACCOUNT, pos):
            self._set_login_mode("account")
            return
        if T.rect_contains(T.L_INPUT, pos):
            self.input_focus = True
            self.login_focus = "name"
            return
        if self.login_mode == "account" and T.rect_contains(T.L_INPUT2, pos):
            self.input_focus = True
            self.login_focus = "password"
            return
        if self.login_mode == "account" and T.rect_contains(T.L_REMEMBER, pos):
            self._toggle_remember()
            return
        if T.rect_contains(T.L_QUIT, pos):
            self.running = False
            return
        if self.login_retry_visible and T.rect_contains(T.L_RETRY, pos):
            self.connect_attempts = 0
            self.connect()
            return
        if T.rect_contains(T.L_MAIN_BTN, pos):
            self.submit_login()
            return
        if self.login_mode == "account" and T.rect_contains(T.L_REGISTER_BTN, pos):
            self.submit_login(register=True)

    def _set_login_mode(self, mode: str) -> None:
        if self.login_stage not in ("IDLE",):
            return  # 握手中不允许换模式
        self.login_mode = mode
        self.login_focus = "name"
        self.login_input_error = False
        self.login_password_error = False
        # 切到账号模式且两个框都还空着时，把本机记住的账号填回来（§3.5）：
        # 这样先逛了一眼游客页也不至于白填一次。
        if mode == "account" and not self.account and not self.password and self.login_store is not None:
            saved = self.login_store.load(self.host, self.port)
            if saved is not None:
                self.account, self.password = saved
        self.login_status = (
            "输入账号与密码后点击「登录」，或点「注册」创建新账号"
            if mode == "account"
            else "请输入昵称后点击「进入牌桌」"
        )
        self.login_status_color = T.TEXT_DIM

    def _validate_login_inputs(self, register: bool) -> tuple[str, dict] | None:
        """本地预校验（ACCOUNT_DESIGN §5.3）：只减少往返，服务端仍是权威。"""
        if self.login_mode == "guest":
            name = self.nickname.strip()
            if not name:
                self.login_status = "昵称不能为空"
                self.login_status_color = T.NEG
                self.login_input_error = True
                return None
            if len(name) > NICKNAME_MAX:
                self.login_status = f"昵称最多 {NICKNAME_MAX} 个字符（当前 {len(name)}）"
                self.login_status_color = T.NEG
                self.login_input_error = True
                return None
            if any(ord(c) < 0x20 for c in name):
                self.login_status = "昵称不能包含换行或控制字符"
                self.login_status_color = T.NEG
                self.login_input_error = True
                return None
            self.nickname = name
            return ("login", {"name": name})

        account = self.account.strip()
        self.account = account
        if not (ACCOUNT_NAME_MIN <= len(account) <= ACCOUNT_NAME_MAX) or not all(
            c.isascii() and (c.isalnum() or c == "_") for c in account
        ):
            self.login_status = (
                f"账号名需 {ACCOUNT_NAME_MIN}..{ACCOUNT_NAME_MAX} 个字符，"
                "仅限字母、数字与下划线"
            )
            self.login_status_color = T.NEG
            self.login_input_error = True
            self.login_focus = "name"
            return None
        if not (PASSWORD_MIN <= len(self.password) <= PASSWORD_MAX) or not all(
            0x20 <= ord(c) <= 0x7E for c in self.password
        ):
            self.login_status = f"密码需 {PASSWORD_MIN}..{PASSWORD_MAX} 个可见字符"
            self.login_status_color = T.NEG
            self.login_password_error = True
            self.login_focus = "password"
            return None
        frame = {"username": account, "password": self.password}
        return ("register" if register else "login", frame)

    def submit_login(self, register: bool = False) -> None:
        if self.login_stage not in ("IDLE",):
            return  # 已在连接/握手链路中
        auth = self._validate_login_inputs(register)
        if auth is None:
            return
        self.pending_auth = auth
        # 记住的是**提交出去**的凭据：握手期间输入框仍可编辑，落盘不能用 `self.password` 兜底。
        self._submitted_credentials = (
            (auth[1]["username"], auth[1]["password"]) if "username" in auth[1] else None
        )
        self.state.reset_all()
        self.stats = None
        self.stats_open = False
        self.auto_start_after_login = False
        self.invalid_name_count = 0
        self.connect_attempts = 0
        self.login_input_error = False
        self.login_password_error = False
        self.connect()

    # ------------------------------------------------------------ 帧处理

    def update(self) -> None:
        now = time.monotonic()
        self._drain_net(now)
        self._heartbeat(now)
        self._poll_room_list(now)
        self._stage_timeouts(now)
        self._window_watchdog(now)
        self._auto_retry(now)
        self._pump_sfx(now)
        self.feel.update(now)
        # 播报状态机（ARCH §4.1）：唯一的时间驱动点，只吃绝对 `now`，不 sleep、不发帧。
        self.bc.update(now)
        # 播报语音（S2 批）：取到点的句子交给 VOICE 通道，并维护 WIN duck。
        # **解码在启动期已做完**，这里只有 `Channel.play()`，没有 `Sound()`。
        self._pump_voice(now)
        self._countdown_tick(now)
        if self.toast_until and now >= self.toast_until:
            self.toast_text = ""
            self.toast_until = 0.0
        if self.error_until and now >= self.error_until:
            self.error_until = 0.0
        state = self.state
        if state.screen == SCREEN_TABLE and state.game_started and not state.hand:
            if not self._empty_hand_synced:
                self._empty_hand_synced = True
                self.send_frame("sync")
        else:
            self._empty_hand_synced = False

    def _drain_net(self, now: float) -> None:
        net = self.net
        if net is None:
            return
        for _ in range(FRAMES_PER_TICK):
            try:
                kind, payload = net.inbox.get_nowait()
            except queue.Empty:
                break
            try:
                self._handle_net_item(kind, payload, now)
            except Exception:  # pragma: no cover - 主循环绝不因单帧异常崩溃
                log.exception("处理入站条目失败：kind=%s", kind)
        if not self.connected and net.connected:
            self.connected = True

    def _handle_net_item(self, kind: str, payload: object, now: float) -> None:
        if kind == "connected":
            self.connected = True
            self.login_stage = "WAIT_LOGIN_OK"
            self.stage_since = now
            self.login_status = "已连接，正在登录…"
            self.login_status_color = T.TEXT_DIM
            # 握手第一帧由提交时定下的意图决定（ACCOUNT_DESIGN §5）：游客是 login{name}，
            # 账号是 login{username,password} 或 register{...}。没有意图时退回游客登录。
            ftype, fields = self.pending_auth or ("login", {"name": self.nickname})
            self.send_frame(ftype, **fields)
        elif kind == "connect_failed":
            self.connected = False
            self.connect_attempts += 1
            message = str(payload)
            if self.connect_attempts < 3:
                delay = RETRY_DELAYS[min(self.connect_attempts - 1, len(RETRY_DELAYS) - 1)]
                self.login_status = f"连接失败：{message}（重试 {self.connect_attempts}/3）"
                self.login_status_color = T.NEG
                self.retry_at = now + delay
                self.login_stage = "IDLE"
            else:
                self.login_status = "无法连接服务器，请确认服务端已启动"
                self.login_status_color = T.NEG
                self.login_retry_visible = True
                self.login_stage = "IDLE"
                self.retry_at = None
        elif kind == "closed":
            self._on_closed(str(payload), now)
        elif kind == "frame":
            self._on_frame(payload, now)

    def _on_frame(self, frame: object, now: float) -> None:
        if not isinstance(frame, dict):
            return
        ftype = frame.get("type")
        log.debug("← %s", ftype)
        if ftype in ("state_sync", "action_request"):
            self.error_until = 0.0
        if self.banner_text and self.banner_action is None:
            self._clear_banner()
        if ftype == "login_ok":
            # 握手完成：从这里开始才允许发心跳（t28 / GD-C1 零 error 帧）
            self.logged_in = True
            self.last_ping = now          # 心跳时钟自登录起算 → 首个 ping 在 HEARTBEAT_INTERVAL 之后
            # 账号会话才有战绩：`u-` 前缀是服务端对账号 id 的既定口径（ACCOUNT_DESIGN §1.2），
            # 游客的 `p-` 会话在界面上不出现战绩入口。
            player_id = frame.get("player_id")
            self.has_account = isinstance(player_id, str) and player_id.startswith("u-")
            if self.has_account:
                # 只有**账号**登录成功才值得记：游客的 `p-` 会话下次也连不回来（§3.5）。
                self._remember_credentials()
            self.stats = None
            self.stats_open = False
            self.login_stage = "IDLE"
            self.stage_since = now
            if self.auto_start_after_login:
                # 结算页「再来一局」：跳过大厅，直接开新局（一键语义，UI_UX_SPEC §9.5 第 7 条）
                self.auto_start_after_login = False
                self.start_game()
                return
            # 默认路径：停在大厅。建桌和加入都等玩家点，不在这里发 create_room。
            self.open_rooms = []
            self.state.enter_lobby()
            self.login_status = "已登录，请点「开始对局」"
            self.login_status_color = T.TEXT_DIM
            if self.has_account:
                self.send_frame("stats_request")  # 大厅要显示战绩，先拉一次
            self._request_room_list(now)
            return
        if ftype == "room_joined":
            self._on_room_joined(frame, now)
            return
        if ftype == "room_list":
            self._apply_room_list(frame)
            return
        if ftype == "pong":
            if self.ping_sent_at is not None:
                self.rtt_ms = (now - self.ping_sent_at) * 1000.0
                self.ping_sent_at = None
            return
        if ftype == "stats":
            # ACCOUNT_DESIGN §3.4：载荷原样留存，交给 render/stats.py 格式化（渲染层不解析协议）。
            games, wins = frame.get("games"), frame.get("wins")
            draws, net = frame.get("draws"), frame.get("net_score")
            recent = frame.get("recent")
            if all(isinstance(value, int) for value in (games, wins, draws, net)) and isinstance(recent, list):
                self.stats = frame
            return
        if ftype == "error":
            self._on_error(frame, now)
            return
        if ftype == "state_sync":
            # §7 R2 / F-AN-03：权威帧到达 ⇒ 动画立即 snap 到终态（不得继续播完再更新，
            # 否则会出现「牌已落在 A 格、动画还飞向 B 格」）。
            self.feel.snap_all()
        before = self.state.screen
        self.state.apply(frame)
        self._side_effects(frame, before, now)

    def _on_room_joined(self, frame: dict, now: float) -> None:
        """建桌或加入成功。大厅路径停在大厅；只有 `start_game` 会接着发 `ready`。"""
        self._cache_room_joined(frame)
        if self.auto_ready_on_join:
            self.auto_ready_on_join = False
            self.login_stage = "WAIT_GAME_START"
            self.stage_since = now
            self.login_status = "准备中…（等待 game_start）"
            self.login_status_color = T.TEXT_DIM
            self.send_frame("ready")
            return
        self.login_stage = "IDLE"
        self.stage_since = now
        if self.state.screen != SCREEN_TABLE:
            self.state.screen = SCREEN_LOBBY
            self.state.phase = "大厅"
        room_id = self.state.room_id or ""
        seat = self.state.my_seat if isinstance(self.state.my_seat, int) else 0
        self.login_status = f"已在 {room_id}，座位 {seat}，点「开始对局」开局"
        self.login_status_color = T.TEXT_DIM

    def _apply_room_list(self, frame: dict) -> None:
        """刷新可加入的房间。不改当前座位（§12.2）。"""
        rooms = frame.get("rooms")
        if not isinstance(rooms, list):
            return
        parsed: list[dict] = []
        for item in rooms:
            if not isinstance(item, dict):
                continue
            room_id = item.get("room_id")
            if not isinstance(room_id, str) or not room_id:
                continue
            host = item.get("host")
            players = item.get("players")
            seats = item.get("seats")
            parsed.append({
                "room_id": room_id,
                "host": host if isinstance(host, str) else "",
                "players": players if isinstance(players, int) else 0,
                "seats": seats if isinstance(seats, int) and seats > 0 else 4,
            })
        self.open_rooms = parsed

    def _request_room_list(self, now: float) -> None:
        self.last_list_rooms_at = now
        self.send_frame("list_rooms")

    def _poll_room_list(self, now: float) -> None:
        if self.state.screen != SCREEN_LOBBY or not self.logged_in or not self.connected:
            return
        if self.last_list_rooms_at != 0.0 and now - self.last_list_rooms_at < LIST_ROOMS_INTERVAL:
            return
        self._request_room_list(now)

    def _cache_room_joined(self, frame: dict) -> None:
        room_id = frame.get("room_id")
        if isinstance(room_id, str):
            self.state.room_id = room_id
        seat = frame.get("seat")
        if isinstance(seat, int):
            self.state.my_seat = seat
        seats = frame.get("seats")
        if isinstance(seats, list):
            for raw in seats:
                if not isinstance(raw, dict):
                    continue
                index = raw.get("seat")
                if not isinstance(index, int) or not 0 <= index <= 3:
                    continue
                view = self.state.seats[index]
                name = raw.get("name")
                if isinstance(name, str):
                    view.name = name
                view.is_bot = bool(raw.get("is_bot", False))

    def _side_effects(self, frame: dict, before_screen: str, now: float) -> None:
        ftype = frame.get("type")
        if ftype == "game_start":
            self.login_stage = "IDLE"
            self.login_retry_visible = False
            self._clear_banner()
            self._turn_start_announced = False
            self._was_tenpai = False
            self.feel.snap_all()          # F-TP-12：跨局隔离，动画状态一并清
            self.feel.tenpai_anim = None
            # 播报在此**直接终止**（RULINGS C8：不缓冲权威帧，`game_start` 到达即切屏）
            self.bc.abort("game_start")
            self.bc.clear()
        elif ftype == "draw":
            tile = frame.get("tile")
            if frame.get("seat") == self.state.my_seat:
                # §4.10（v1.4）：提示条牌名用 §5.3 文案公式（tile_label），不再显示线上牌码
                self._toast(f"你摸到 {tile_label(tile)}", now)
                # F-AU-02 摸牌音（P1）+ F-AN-20 摸牌进场动画（120 ms，**不 gate 输入**）
                self._sfx("tile_draw")
                if self.state.drawn_index is not None:
                    self.feel.start_draw(self.state.drawn_index, now)
        elif ftype == "discard_event":
            seat = frame.get("seat")
            tile = frame.get("tile")
            # F-AU-03：任一 discard_event（含三家）都响；与出牌动画同时起，0 ms 延迟
            self._sfx("tile_discard")
            self._start_discard_flight(seat)
            if seat == self.state.my_seat:
                self._toast(f"你打出 {tile_label(tile)}", now)
                self.feel.note_operation(now)   # F-AN-04 加速态计时基准
            elif isinstance(seat, int):
                self._toast(f"{self.state.rel_label(seat)} 打出 {tile_label(tile)}", now)
        elif ftype == "action_event":
            self._toast(self._action_toast(frame), now)
            self._action_event_feel(frame, now)
        elif ftype == "tenpai":
            self._tenpai_feel(now)
        elif ftype == "game_end":
            self._clear_banner()
            self.state.window = None
            self.feel.snap_all()
            # 结算播报（RULINGS §2.1）：`game_end` 到达即起播；能否播由 `budget` 三档决定，
            # 不播报时 `view().plan` 仍在（阶段 B 静态页照常渲染）。
            self.bc.start(
                frame,
                time.monotonic(),
                name_of=self.state.name_of,
                rel_label=self.state.rel_label,
            )
            # F-AU-11 荒庄 / F-AU-15 局终（互斥：`draw_exhausted` 走收束音，不走「一局结束」音）
            reason = frame.get("reason")
            if reason == "draw_exhausted":
                self._sfx_later(60.0, "draw_hand")
            else:
                self._sfx_later(100.0, "hand_end")
            if self.login_mode == "account":
                # 结算页要显示累计战绩，且本局刚落库（ACCOUNT_DESIGN §4.1/§5.5）。
                self.send_frame("stats_request")
        elif ftype == "match_end":
            # v2.0（§9.6）：场终页恢复两出口；此后不再有对局帧。
            # F-AU-16（P0）+ F-AU-27：场终音**立即抢占** WIN 通道上未播完的和牌音。
            self._clear_banner()
            self.state.window = None
            self.feel.snap_all()
            # 播报不缓存 `match_end`（RULINGS C8）：终局由服务端延迟，客户端照帧即切场终页
            self.bc.abort("match_end")
            self.bc.clear()
            self._sfx_later(120.0, "match_end")
        elif ftype == "action_request":
            # F-AU-12：本家打牌窗口**首次**出现 ⇒ 操作栏显示后 80 ms 才落声
            if (
                frame.get("window") == WINDOW_DISCARD
                and frame.get("seat", self.state.my_seat) == self.state.my_seat
                and not self._turn_start_announced
            ):
                self._turn_start_announced = True
                self._sfx_later(80.0, "turn_start")
        elif ftype == "state_sync":
            pass
        self._on_screen_changed(before_screen, self.state.screen)
        if before_screen != self.state.screen:
            log.info("界面迁移：%s → %s", before_screen, self.state.screen)

    # ------------------------------------------------------------ 手感：帧触发细节

    def _action_event_feel(self, frame: dict, now: float) -> None:
        """副露音（F-AU-04..08）与鸣牌动画（§6.4）；和牌音（F-AU-09/10）走 `from_seat` 判定。"""
        action = frame.get("action")
        if action == "win":
            # v1.7 判定：`win.tsumo` 只存在于 game_end，而和牌音必须在 action_event 时就响。
            # 协议已冻结：有 `from_seat` = 荣和；无（`None`）= 自摸（§11.2.1-B）。
            self._sfx("win_ron" if frame.get("from_seat") is not None else "win_tsumo")
            self.feel.snap_all()
            return
        sfx_id = {
            "pong": "call_pong", "chow": "call_chow", "kong": "call_kong",
            "ankan": "call_ankan", "kakan": "call_kakan",
        }.get(action)
        if sfx_id is None:
            return
        self._sfx(sfx_id)
        seat = frame.get("seat")
        # §6.4：ankan/kakan 没有「被鸣的那张牌」（不从牌河拿）⇒ 跳过 ①阶段
        self.feel.start_meld(seat if isinstance(seat, int) else -1,
                             has_claimed_tile=action in ("pong", "chow", "kong"), now=now)

    def _tenpai_feel(self, now: float) -> None:
        """F-TP-06/07/08 与 F-AU-14：听牌条出现/更新/消失时播动画与音。"""
        is_tenpai = self.state.is_tenpai
        previous = list(getattr(self, "_tenpai_prev_tiles", []))
        current = [str(it.get("tile")) for it in self.state.tenpai_waits]
        if is_tenpai and not self._was_tenpai:
            self.feel.start_tenpai("appear", current, now=now)
            self._sfx("tenpai_on")            # F-AU-14
        elif is_tenpai and self._was_tenpai and set(current) != set(previous):
            added = [t for t in current if t not in previous]
            self.feel.start_tenpai("update", added, now=now)
        elif not is_tenpai and self._was_tenpai:
            self.feel.start_tenpai("fade", [], now=now)
        self._was_tenpai = is_tenpai
        self._tenpai_prev_tiles = current

    def _start_discard_flight(self, seat: object) -> None:
        """F-AN-10..16：出牌飞向牌河。飞不起来（超 3 个 / 关动画）就直接按终态绘制。"""
        if not isinstance(seat, int) or not self.feel.animations_active:
            return
        state = self.state
        view = state.seat_view(seat)
        if not view.discards:
            return
        tile = view.discards[-1]
        rel = state.rel_index(seat)
        side = rel in (1, 3)
        end = table_view.discard_flight_end(state, seat, rel, len(view.discards))
        if end is None:
            return
        if rel == 0 and state.selected_index is not None:
            layout = table_view.hand_layout(state)
            idx = min(state.selected_index, len(layout) - 1) if layout else None
            start = layout[idx] if idx is not None and idx >= 0 else T.R_SELF_HAND
        else:
            start = table_view.discard_flight_start(state, rel)
        self.feel.start_discard(seat, tile, start, end, side=side)

    def _action_toast(self, frame: dict) -> str:
        """事件提示条文案（UI_UX_SPEC v1.4 §4.10）：结构 `<座位> <动作> <牌名>`。

        牌名与动作文案统一取自 `tiles.button_label()` / `tiles.tile_label()`（§5.3 公式），
        调用点**不得**自行拼接牌名；载荷（出站/入站帧的 `tiles`）仍是线上牌码。
        """
        seat = frame.get("seat")
        action = frame.get("action")
        tiles = [t for t in (frame.get("tiles") or []) if isinstance(t, str)]
        who = "你" if seat == self.state.my_seat else (
            self.state.rel_label(seat) if isinstance(seat, int) else "？"
        )
        if action == "win":
            name = tile_label(tiles[0]) if tiles else ""
            return f"{who} {button_label('win')} {name}".strip()
        if action in ("pong", "kong", "chow"):
            return f"{who} {button_label(action, tiles)}".strip()
        detail = tile_label(tiles[0]) if tiles else ""
        return f"{who} {action} {detail}".strip()

    # ------------------------------------------------------------ 错误

    def _on_error(self, frame: dict, now: float) -> None:
        code = frame.get("code")
        message = frame.get("message")
        if not isinstance(code, str):
            code = "INTERNAL"
        self.state.error_count += 1
        self.state.last_error = {"code": code, "message": message}
        self.error_until = now + ERROR_STRIP_DURATION
        log.warning(
            "收到 error：code=%s message=%s seq=%s",
            code, message, self.state.seq,
        )
        self.error_code = code
        self.error_message = message if isinstance(message, str) and message else ERROR_TEXT.get(code, code)
        if code == "ILLEGAL_ACTION" and isinstance(message, str) and "riichi" in message:
            # 立直被拒的最常见原因：选中的宣言牌打完不听牌（服务端用门前手牌重算过，
            # 不听/振听/点数不足/牌山不足都归到同一句英文 message）。中文说清「换一张」，
            # 否则玩家只会看到 cannot declare riichi now，不知道下一步该做什么。
            self.error_message = "立直失败：这张牌打出后不听牌，换一张再点立直"
            self._toast("立直失败：这张牌打出后不听牌", now)
        # 建桌/加入没成功时把按钮放开，否则会一直停在「正在建桌…」。
        if self.login_stage == "WAIT_ROOM_JOINED":
            self.login_stage = "IDLE"
            self.auto_ready_on_join = False

        if code == "ALREADY_ONLINE" and self.rematch_retries_left > 0:
            # §9.5 第 7 条：上一连接刚关，服务端可能还没释放该账号。短重试而不是报错，
            # 让「再来一局」这一下点击在账号模式下也可靠。
            self.rematch_retries_left -= 1
            self.close_socket("再来一局：等待账号释放")
            self.state.reset_all()
            self.login_stage = "IDLE"
            self.logged_in = False
            self.error_until = 0.0
            self.login_status = "再来一局：等待上一局连接释放…"
            self.login_status_color = T.TEXT_DIM
            self.retry_at = now + REMATCH_RETRY_DELAY
            return
        if code in ("AUTH_FAILED", "USERNAME_TAKEN", "ALREADY_ONLINE"):
            # 账号类错误（ACCOUNT_DESIGN §5.4）：服务端保持连接，但客户端按「重来一次」处理——
            # 断开并回到可编辑的登录界面，与 NOT_LOGGED_IN 的恢复路径一致。
            self.close_socket(ERROR_TEXT[code])
            self.state.reset_all()
            self.login_stage = "IDLE"
            self.logged_in = False
            self.pending_auth = None
            self.login_status = ERROR_TEXT[code]
            self.login_status_color = T.NEG
            self.login_password_error = code == "AUTH_FAILED"
            self.state.screen = SCREEN_LOGIN
        elif code == "INTERNAL" and message == "account service unavailable":
            self.close_socket("账号服务不可用")
            self.state.reset_all()
            self.login_stage = "IDLE"
            self.logged_in = False
            self.pending_auth = None
            self.login_status = ACCOUNT_UNAVAILABLE_TEXT
            self.login_status_color = T.NEG
            self.state.screen = SCREEN_LOGIN
        elif code == "INVALID_NAME":
            self.invalid_name_count += 1
            self.login_input_error = True
            self.login_stage = "IDLE"
            self.logged_in = False
            note = f"昵称非法：需 1..16 个字符，且不含换行或控制字符（第 {self.invalid_name_count}/3 次）"
            if self.invalid_name_count >= 3:
                note += "，再提交一次将被断开连接"
            self.login_status = note
            self.login_status_color = T.NEG
            self.state.screen = SCREEN_LOGIN
        elif code == "NOT_LOGGED_IN":
            self.close_socket("会话未建立")
            self.state.reset_all()
            self.login_stage = "IDLE"
            self.login_status = ERROR_TEXT[code]
            self.login_status_color = T.NEG
        elif code == "RATE_LIMITED":
            self.throttle_until = now + 1.0
            self._set_banner("操作过于频繁，请稍候", T.WARN)
        elif code in ("NOT_YOUR_TURN", "NO_PENDING_ACTION", "ILLEGAL_ACTION", "UNKNOWN_TILE"):
            self.state.clear_pending()
            if code in ("NOT_YOUR_TURN", "NO_PENDING_ACTION", "UNKNOWN_TILE"):
                self.send_frame("sync")
            if code == "ILLEGAL_ACTION":
                self.state.selected_index = None
        elif code == "BAD_REQUEST":
            self.error_message = f"{self.error_message}（继续出现该错误将被断开）"
            self.send_frame("sync")
        elif code == "FRAME_TOO_LARGE":
            self.close_socket("发送帧过大，主动断开")
        elif code == "INTERNAL":
            self._set_banner("服务端内部错误，牌局可能无法继续", T.NEG, action="返回登录")
        elif code == "ROOM_NOT_FOUND":
            if self.login_stage == "WAIT_GAME_START":
                self.login_stage = "IDLE"
            self.auto_ready_on_join = False
            self.state.room_id = None
            self.state.my_seat = None
            self._set_banner("房间不存在或已关闭", T.WARN, action="重新建桌")

    def _on_closed(self, reason: str, now: float) -> None:
        self.connected = False
        self.login_stage = "IDLE"
        self.logged_in = False
        self.last_ping = None
        self.bc.abort(f"closed:{reason}")   # 断线 = 播报终态（不发帧、不缓冲）
        if self.state.result is not None:
            # §8.4：断线前已收到 game_end → 不显示断线蒙层，直接停留在结算页
            self.disconnect_reason = None
            return
        if self.state.screen == SCREEN_LOGIN:
            self.login_status = f"连接失败：{reason}"
            self.login_status_color = T.NEG
            self.login_retry_visible = True
            return
        if self.state.screen == SCREEN_LOBBY:
            # 大厅里断线时还没有对局在进行，套不上「对局进行中，服务端将托管你的座位」
            # 那层蒙层：直接回登录界面并把原因写在状态行（§8.4 的口径只适用于牌桌/结算）。
            self.return_to_login()
            self.login_status = f"连接已断开：{reason}"
            self.login_status_color = T.NEG
            return
        self.disconnect_reason = reason
        self.state.window = None
        self.state.clear_pending()
        log.info("对局中断线：%s", reason)

    # ------------------------------------------------------------ 定时逻辑

    def _heartbeat(self, now: float) -> None:
        if not self.connected or self.net is None:
            return
        # PROTOCOL §4.1：未登录的连接收到除 login 外的任何帧 → error{NOT_LOGGED_IN}。
        # 因此**必须等 login_ok 到达后才允许发心跳**：TCP 一连上 `connected` 即为真，
        # 若此处只判断 connected，第一次 tick（last_ping is None）就会在
        # `connect()` 与 `send_frame("login")` 之间那十几毫秒窗口内发出 ping，
        # 服务端回 NOT_LOGGED_IN，破坏 GD-C1「全程零 error 帧」（t28 修复）。
        if self.logged_in and (self.last_ping is None or now - self.last_ping >= HEARTBEAT_INTERVAL):
            self.last_ping = now
            self.ping_sent_at = now
            self.send_frame("ping")
        last_inbound = self.net.last_inbound
        if (
            self.disconnect_reason is None
            and self.state.result is None
            and last_inbound is not None
        ):
            silence = now - last_inbound
            if silence > 16.0:
                # §8.2：ping 连续 3 次未收到 pong（>16 s）→ 横幅提示，不主动断开
                self._set_banner("连接不稳定（心跳无响应）", T.WARN)
            if silence > INBOUND_SILENCE_LIMIT:
                # §8.4：先出底部横幅，再升级成断线蒙层。
                if self.inbound_stall_since is None:
                    self.inbound_stall_since = now
                    self._set_banner("网络无响应（20 秒）", T.WARN)
                elif now - self.inbound_stall_since >= 0.5:
                    self._on_closed("网络无响应（20 秒）", now)
            else:
                self.inbound_stall_since = None

    def _stage_timeouts(self, now: float) -> None:
        screen = self.state.screen
        if self.login_stage == "WAIT_LOGIN_OK" and screen == SCREEN_LOGIN:
            timeout = ACCOUNT_LOGIN_TIMEOUT if self.login_mode == "account" else LOGIN_OK_TIMEOUT
            if now - self.stage_since > timeout:
                self.login_status = "服务器无响应（未返回 login_ok）"
                self.login_status_color = T.WARN
                self.login_retry_visible = True
                self.login_stage = "IDLE"
        elif self.login_stage == "WAIT_ROOM_JOINED" and now - self.stage_since > 3.0:
            # §8.6：未收到 room_joined 时不能把按钮卡死。登录界面仍给「返回登录」。
            self.auto_ready_on_join = False
            if screen == SCREEN_LOBBY:
                self.login_stage = "IDLE"
                self.login_status = "等待 room_joined 超时（3 秒）"
                self.login_status_color = T.WARN
            elif screen == SCREEN_LOGIN:
                self._set_banner("等待 room_joined 超时（3 秒）", T.WARN, action="返回登录")
        elif (
            self.login_stage == "WAIT_GAME_START"
            and screen in (SCREEN_LOGIN, SCREEN_LOBBY)
            and now - self.stage_since > GAME_START_TIMEOUT
        ):
            self.login_status = "等待开局超时（10 秒）"
            self.login_status_color = T.WARN
            self._set_banner("等待开局超时（10 秒）", T.WARN, action="返回登录")

    def _auto_retry(self, now: float) -> None:
        if self.retry_at is None:
            return
        if now < self.retry_at:
            return
        self.retry_at = None
        self.connect()

    def _window_watchdog(self, now: float) -> None:
        state = self.state
        if state.screen != SCREEN_TABLE or state.window is None:
            self.zero_since = None
            return
        remaining = state.remaining_ms(now, time.time() * 1000.0)
        if remaining is None or remaining > 0:
            self.zero_since = None
            return
        if state.selected_index is not None:
            state.selected_index = None
        if self.zero_since is None:
            self.zero_since = now
            self.center_note_until = now + 1.5
        elif now - self.zero_since > 5.0 and self.disconnect_reason is None:
            self._set_banner("服务器无响应（5 秒无状态更新）", T.WARN, action="返回登录")

    # ------------------------------------------------------------ 手感：音效 / 动画

    def _sfx(self, sound_id: str, seq: object = None) -> None:
        """播一条音效。静音/缺资产时**仍走判定**（F-AU-25），只是不出声。"""
        self.audio.play(sound_id, seq)

    def _sfx_later(self, delay_ms: float, sound_id: str, seq: object = None) -> None:
        self._pending_sfx.append((time.monotonic() + delay_ms / 1000.0, sound_id, seq))

    def _pump_sfx(self, now: float) -> None:
        if not self._pending_sfx:
            return
        due = [item for item in self._pending_sfx if item[0] <= now]
        if not due:
            return
        self._pending_sfx = [item for item in self._pending_sfx if item[0] > now]
        for _at, sound_id, seq in due:
            self._sfx(sound_id, seq)

    def _pump_voice(self, now: float) -> None:
        """播报语音：取出到点的句子、维护 WIN duck、终态时收声。

        **缺资产 ⇒ `play_voice` 返回 False**，这里不做任何补偿（不重试、不等待），
        所以「`assets/vo/` 整个改名」与「有 wav」两种状态下，时间轴与画面完全一致。
        """
        live = VOICE_ENABLED and self.bc.running and not self.bc.fast_forward
        if live:
            for cue in self.bc.collect_voice(now):
                self.audio.play_voice(cue, now)
                if BROADCAST_TRACE:
                    print(f"[BC] cue {cue}", flush=True)
            self.audio.update(now, self.bc.next_voice_in(now))
            self._voice_live = True
        else:
            if self._voice_live:
                self.audio.stop_voice()      # 跳过 / 播完 / 打断：一次收声
                self._voice_live = False
            self.audio.update(now, None)
        if BROADCAST_TRACE:
            self._trace_broadcast(now)

    def _trace_broadcast(self, now: float) -> None:
        """`MAHJONG_BC_TRACE=1` 时打印播报进度（t0 / end_at / 第几步 / 每步实际触发时刻）。"""
        bc = self.bc
        timeline = bc.timeline
        if not bc.running:
            if self._bc_trace_step is not None:
                elapsed = (now - bc.started_at) * 1000.0
                print(f"[BC] 终态 {bc.state} t=+{elapsed:.1f}ms", flush=True)
                self._bc_trace_step = None
            return
        if timeline is None:
            return
        if self._bc_trace_step is None:
            self._bc_trace_step = -1
            cues = timeline.cues
            print(
                f"[BC] 起播 t0={bc.started_at:.3f} end_at={bc.end_at:.3f} "
                f"TOTAL={timeline.end_ms}ms budget={timeline.budget_ms}ms "
                f"语音={len(cues)}句 删句={list(timeline.deleted)}",
                flush=True,
            )
        view = bc.view()
        if view.step_index == self._bc_trace_step:
            return
        self._bc_trace_step = view.step_index
        elapsed = (now - bc.started_at) * 1000.0
        if view.step_index < 0:
            print(f"[BC] step=-1（S0 开场）t=+{elapsed:.1f}ms", flush=True)
            return
        step = timeline.steps[view.step_index]
        print(
            f"[BC] step={view.step_index:2d}/{len(timeline.steps)} t=+{elapsed:7.1f}ms "
            f"计划={step.at_ms:5d}ms 差={elapsed - step.at_ms:+6.1f}ms "
            f"focus={step.focus} caption={step.caption!r}",
            flush=True,
        )

    def _countdown_tick(self, now: float) -> None:
        """F-AU-13：倒计时进入最后 3 秒，每秒一次（默认关，`N` 键打开）。

        播报期一律不播（RULINGS §2.5：播报期倒计时行只写「播报中…」）。
        """
        if not self.feel.countdown_on:
            self._last_countdown_bucket = None
            return
        if self.bc.plan is not None:
            # 结算页（含播报期）不滴答：倒计时在播报期是暂停的，滴答会与语音打架
            self._last_countdown_bucket = None
            return
        if self.state.screen != SCREEN_TABLE:
            self._last_countdown_bucket = None
            return
        remaining = self._remaining_ms()
        if remaining is None or remaining <= 0:
            self._last_countdown_bucket = None
            return
        bucket = int((remaining + 999) // 1000)  # 跨过 3000/2000/1000 时换桶
        if bucket > 3:
            self._last_countdown_bucket = bucket
            return
        if self._last_countdown_bucket is not None and bucket != self._last_countdown_bucket:
            self._sfx("countdown_tick")
        self._last_countdown_bucket = bucket

    def _on_screen_changed(self, before: str, after: str) -> None:
        """F-AU-26：任何屏切换 ⇒ 三条通道全部 `fadeout(40 ms)`。"""
        if before != after:
            self.audio.stop_all()
            # 播报只在结算页有意义：离开结算页（切屏）即终态，不缓冲任何帧（ARCH §3.7 X3）
            if after != SCREEN_RESULT:
                self.bc.abort(f"screen:{after}")

    def _feel_toggles_help(self) -> str:
        return "T 听牌 H 提示 M 静音 N 计时"

    def _handle_feel_key(self, event: pygame.event.Event) -> bool:
        """T/H/M/N 四个开关（FEEL_SPEC §8 C6：有快捷键、无面板）。返回是否已消费。"""
        key = getattr(event, "key", None)
        if key == pygame.K_t:
            on = self.feel.toggle_tenpai()
            self._toast(f"听牌提示：{'开' if on else '关'}", time.monotonic())
            return True
        if key == pygame.K_h:
            on = self.feel.toggle_hints()
            self._toast(f"可吃碰提示：{'开' if on else '关'}", time.monotonic())
            return True
        if key == pygame.K_m:
            muted = self.feel.toggle_mute()
            self.audio.set_muted(muted)
            self._toast(f"音效：{'关' if muted else '开'}", time.monotonic())
            return True
        if key == pygame.K_n:
            on = self.feel.toggle_countdown()
            self.audio.countdown_enabled = on
            self._toast(f"倒计时滴答：{'开' if on else '关'}", time.monotonic())
            return True
        return False

    # ------------------------------------------------------------ 提示

    def _toast(self, text: str, now: float) -> None:
        if not text:
            return
        self.toast_text = text
        self.toast_until = now + TOAST_DURATION

    def _set_banner(self, text: str, color: tuple, action: str | None = None) -> None:
        self.banner_text = text
        self.banner_color = color
        self.banner_action = action

    def _clear_banner(self) -> None:
        self.banner_text = None
        self.banner_action = None

    def _run_banner_action(self) -> None:
        action = self.banner_action
        self._clear_banner()
        if action == "返回登录":
            self.return_to_login()
        elif action == "重新建桌":
            # §12.2：重新建桌 = 再点一次「新建房间」，不自动 ready。
            self.login_stage = "IDLE"
            self.auto_ready_on_join = False
            self.create_lobby_room()

    def return_to_login(self) -> None:
        """§9.5：关闭 socket、清空全部本地状态、回到登录界面（昵称保留）。

        这是**错误/断线**的出口（断线蒙层、账号类错误、返回登录按钮）。正常打完一局想
        继续玩的话走 `go_to_lobby()`：同样换一条连接，但直接回大厅、不用重输账号密码。
        """
        self.close_socket("返回登录")
        self.state.reset_all()
        self.disconnect_reason = None
        self.connected = False
        self.login_stage = "IDLE"
        self.login_retry_visible = False
        self.error_until = 0.0
        self.zero_since = None
        self._clear_banner()
        self.login_status = "已返回登录界面"
        self.login_status_color = T.TEXT_DIM
        # 会话结束：战绩入口随会话一起消失（快照留着也无意义，清掉避免下次误显示）。
        self.has_account = False
        self.stats_open = False
        self.stats = None
        self.open_rooms = []
        self.auto_ready_on_join = False
        self.last_list_rooms_at = 0.0

    def _reconnect_for_new_game(self, status: str) -> None:
        """换一条连接重放握手（§9.5 第 1–3/7 条）：`go_to_lobby` 与 `rematch` 的共同前半段。

        协议里没有「离开房间」帧，而结算后的连接在服务端仍算作「在该房间里」，所以想再开
        一局只能**先断开、再重连**。凭据复用上次提交的握手帧（`pending_auth` 保留着），
        因此账号会话不必重输口令；服务端与协议零改动。
        """
        self.close_socket(status)
        self.state.reset_all()
        self.disconnect_reason = None
        self.connected = False
        self.login_stage = "IDLE"
        self.login_retry_visible = False
        self.error_until = 0.0
        self.zero_since = None
        self._clear_banner()
        self.stats = None
        self.stats_open = False
        self.open_rooms = []
        self.auto_ready_on_join = False
        self.last_list_rooms_at = 0.0
        # 账号模式下上一连接刚关、服务端可能还没释放该账号：撞上 ALREADY_ONLINE 时自动短重试。
        self.rematch_retries_left = REMATCH_RETRY_LIMIT
        self.login_status = status
        self.login_status_color = T.TEXT_DIM
        self.connect_attempts = 0
        self.connect()

    def go_to_lobby(self) -> None:
        """结算页「返回大厅」（UI_UX_SPEC §9.5 第 7′ 条）：断开重连后停在**大厅**，不是登录界面。

        因为换的是新连接，服务端那条「一个玩家一个房间」的记录也随之翻篇——大厅里点
        「开始对局」立刻就能开新局（老房间由服务端在 game_end 后 30 秒自行回收）。
        """
        self.auto_start_after_login = False
        self._reconnect_for_new_game("返回大厅：正在重新连接…")

    def rematch(self) -> None:
        """「再来一局」（§9.5 第 7 条）：重连后**跳过大厅**直接开局，保持一键语义。"""
        self.auto_start_after_login = True
        self._reconnect_for_new_game("再来一局：正在重新连接…")

    def start_game(self) -> None:
        """再来一局与旧握手：`create_room`，`room_joined` 后自动 `ready`。

        大厅「新建房间」走 `create_lobby_room`，不会自动开局。
        """
        if not self.connected or self.login_stage != "IDLE":
            return
        self.auto_ready_on_join = True
        self.login_stage = "WAIT_ROOM_JOINED"
        self.stage_since = self.now
        self.login_status = "正在建桌…"
        self.login_status_color = T.TEXT_DIM
        self._send_create_room()

    def create_lobby_room(self) -> None:
        """大厅「新建房间」：只发 `create_room`，成功后仍停在大厅。"""
        if not self._lobby_create_enabled():
            return
        self.auto_ready_on_join = False
        self.login_stage = "WAIT_ROOM_JOINED"
        self.stage_since = self.now
        self.login_status = "正在建桌…"
        self.login_status_color = T.TEXT_DIM
        self._send_create_room()

    def _stats_view(self) -> dict | None:
        """按大厅当前玩法取战绩分桶。

        服务端把统计拆成 `classic` / `riichi` 两个子对象（两套玩法的分数不可比）；
        老服务端没有这两个键时退回顶层字段（= classic），行为与之前一致。
        """
        stats = self.stats
        if not isinstance(stats, dict):
            return None
        section = stats.get(self.lobby_mode)
        if isinstance(section, dict):
            merged = dict(section)
            merged["mode"] = self.lobby_mode
            return merged
        return stats

    def _send_create_room(self) -> None:
        """建桌：classic 不带 mode（帧与之前逐字节一致），只有日麻才带上 mode / length。"""
        if self.lobby_mode == "classic":
            self.send_frame("create_room")
        else:
            self.send_frame("create_room", mode=self.lobby_mode, length=self.lobby_length)

    def begin_match(self) -> None:
        """大厅「开始对局」：人已经入座，只发 `ready`。"""
        if not self._lobby_begin_enabled():
            return
        self.login_stage = "WAIT_GAME_START"
        self.stage_since = self.now
        self.login_status = "准备中…（等待 game_start）"
        self.login_status_color = T.TEXT_DIM
        self.send_frame("ready")

    def _lobby_click(self, pos: tuple[float, float]) -> None:
        if self.disconnect_reason:
            return
        if T.rect_contains(T.LB_MODE_CHIPS[0], pos):
            self.lobby_mode = "classic"
            return
        if T.rect_contains(T.LB_MODE_CHIPS[1], pos):
            self.lobby_mode = "riichi"
            return
        if self.lobby_mode == "riichi":
            # 局数只在日麻下可见/可点：classic 的局数由服务端冻结为 4（PROTOCOL §12.1）
            if T.rect_contains(T.LB_LEN_CHIPS[0], pos):
                self.lobby_length = "tonpu"
                return
            if T.rect_contains(T.LB_LEN_CHIPS[1], pos):
                self.lobby_length = "hanchan"
                return
        if T.rect_contains(T.LB_NEW_BTN, pos):
            self.create_lobby_room()
            return
        if T.rect_contains(T.LB_START_BTN, pos):
            self.begin_match()
            return
        if T.rect_contains(T.LB_BACK_BTN, pos):
            self.return_to_login()
            return
        if T.rect_contains(T.LB_QUIT, pos):
            self.running = False
            return
        room_id = self._room_row_at(pos)
        if room_id is not None:
            self._sfx("ui_click")   # F-AU-01：房间行
            self._click_room(room_id)

    def _room_row_at(self, pos: tuple[float, float]) -> str | None:
        for index, room in enumerate(self.open_rooms[:T.LB_ROOM_ROWS]):
            rect = T.lobby_room_row_rect(index)
            if T.rect_contains(rect, pos):
                room_id = room.get("room_id")
                return room_id if isinstance(room_id, str) else None
        return None

    def _click_room(self, room_id: str) -> None:
        """点房间行：已在该房则忽略；已在别的房则只改状态行；否则 `join_room`。"""
        if self.state.room_id == room_id:
            return
        if self.state.room_id:
            self.login_status = "已经在房间里"
            self.login_status_color = T.WARN
            return
        if not self._lobby_create_enabled():
            return
        self.auto_ready_on_join = False
        self.login_stage = "WAIT_ROOM_JOINED"
        self.stage_since = self.now
        self.login_status = "正在加入房间…"
        self.login_status_color = T.TEXT_DIM
        self.send_frame("join_room", room_id=room_id)

    def _lobby_key(self, event: pygame.event.Event) -> None:
        if event.key in (pygame.K_RETURN, pygame.K_KP_ENTER):
            # 已入座：开始对局。还没入座：新建房间。
            if self._seated_in_lobby():
                self.begin_match()
            else:
                self.create_lobby_room()
        elif event.key == pygame.K_ESCAPE:
            self.return_to_login()

    # ------------------------------------------------------------ 牌桌交互

    def _bar_visible(self) -> bool:
        return actionbar.bar_visible(self.state)

    def _broadcast_skippable(self) -> bool:
        """播报期是否可跳（RULINGS §2.5）：只在局终页、且播报正在跑。

        `match_end` 已到（场终页）时不可跳——场终页没有任何播报内容。
        """
        return (
            self.state.screen == SCREEN_RESULT
            and self.state.match_end is None
            and self.bc.running
        )

    def _remaining_ms(self) -> float | None:
        return self.state.remaining_ms(self.now, time.time() * 1000.0)

    def _can_select_hand(self) -> bool:
        state = self.state
        if state.screen != SCREEN_TABLE or state.frozen or self.disconnect_reason:
            return False
        if state.window is None or state.window.window != WINDOW_DISCARD or state.pending:
            return False
        remaining = self._remaining_ms()
        if remaining is None or remaining <= 0:
            return False
        return len(state.hand) > 0

    def _table_click(self, pos: tuple[float, float]) -> None:
        state = self.state
        if state.frozen or self.disconnect_reason:
            return
        if self.stats_open:
            # 战绩面板是模态的：点面板内不吃事件，点面板外关闭。牌桌上的点击一概不透传
            # （避免隔着一层蒙层误出牌）。
            if not T.rect_contains(T.R_STATS_PANEL, pos):
                self.stats_open = False
            return
        if self.has_account and T.rect_contains(T.R_HUD_STATS_BTN, pos):
            self.toggle_stats()
            return
        buttons = actionbar.build_buttons(state, self._remaining_ms())
        for spec in buttons:
            if T.rect_contains(spec.rect, pos):
                if spec.enabled:
                    self._dispatch_button(spec)
                elif spec.reason:
                    # 禁用态不能静默吞点击：日麻立直是「选宣言牌 + 打牌」两段式，
                    # 没选牌时按钮必然灰着，不解释一句玩家会以为功能缺失。
                    self._toast(spec.reason, self.now)
                return
        if state.pending and table_view.hand_hit_test(state, pos) is not None:
            # §5.4：PENDING 期间再点手牌只抖一帧红框，不发牌。
            self.pending_flash_until = self.now + (1.0 / 60.0)
            return
        if not self._can_select_hand():
            return
        index = table_view.hand_hit_test(state, pos)
        if index is None:
            return
        if state.riichi_arming:
            # 宣言模式：点一张牌 = 直接宣言立直并打出它（只有 riichi_tiles 里的牌可点）
            self._commit_riichi_with(index)
            return
        if state.selected_index == index:
            self._confirm_discard()
        else:
            state.selected_index = index

    def toggle_stats(self) -> None:
        """牌桌「战绩」按钮（UI_UX_SPEC §4.11）：打开时顺手拉一次最新战绩。

        请求是异步的——`stats` 还没到时面板显示「正在读取战绩…」，不阻塞牌局；
        打开期间窗口照常计时（服务端权威，客户端不得暂停），超时按 PROTOCOL §5.6.2 代打。
        """
        self.stats_open = not self.stats_open
        if self.stats_open:
            self.send_frame("stats_request")

    def _table_key(self, event: pygame.event.Event) -> None:
        state = self.state
        if event.key == pygame.K_ESCAPE:
            if self.stats_open:
                self.stats_open = False  # ESC 先关面板，再清选中
                return
            state.selected_index = None
            state.riichi_arming = False  # Esc 同时退出立直宣言模式
            return
        if state.frozen or self.disconnect_reason:
            return
        if event.key == pygame.K_RETURN:
            self._confirm_discard()
            return
        if event.key == pygame.K_SPACE:
            for spec in actionbar.build_buttons(state, self._remaining_ms()):
                if spec.kind == "pass" and spec.enabled:
                    self._dispatch_button(spec)
                    return
            return
        digits = {
            pygame.K_1: 0, pygame.K_2: 1, pygame.K_3: 2, pygame.K_4: 3, pygame.K_5: 4,
            pygame.K_6: 5, pygame.K_7: 6, pygame.K_8: 7, pygame.K_9: 8, pygame.K_0: 9,
        }
        if event.key in digits and self._can_select_hand():
            index = digits[event.key]
            if index < len(state.hand):
                state.selected_index = index

    def _dispatch_button(self, spec: actionbar.ButtonSpec) -> None:
        self._sfx("ui_click")       # F-AU-01：点击**按下**帧
        if spec.kind == "discard":
            self._confirm_discard()
        elif spec.kind == "riichi":
            self._on_riichi_click()
        else:
            self._do_action(spec.kind, spec.tiles)

    def _riichi_candidates(self) -> set[str]:
        """本窗口能作宣言牌的牌面（服务端 `riichi_tiles`，v1.21 §4.4）。"""
        window = self.state.window
        if window is None:
            return set()
        return {tile_face(code) for code in window.riichi_tiles}

    def _on_riichi_click(self) -> None:
        """点「立直」：已选好合法宣言牌就直接宣言，否则进入宣言模式并在手牌上打标记。"""
        state = self.state
        window = state.window
        if window is None or not window.has("riichi"):
            return
        index = state.selected_index
        candidates = self._riichi_candidates()
        if not candidates:
            # 老服务端（v1.20 及以前）不下发 riichi_tiles：退回「先选牌再点立直」的旧路径，
            # 不做标记（没有数据来源，标错比不标更糟）。
            if index is None:
                self._toast("立直：先点选一张手牌作为宣言牌", self.now)
                return
            self._confirm_riichi()
            return
        if index is not None and 0 <= index < len(state.hand):
            if candidates and tile_face(state.hand[index]) not in candidates:
                # 选中的那张打完不听牌 —— 直接进模式，让玩家点高亮的那几张
                state.selected_index = None
                state.riichi_arming = True
                self._toast("这张牌打完不听牌：请点高亮的牌", self.now)
                return
            if self._confirm_riichi():
                return
        state.riichi_arming = True
        state.selected_index = None
        self._toast("立直：点一张高亮的牌（打出去仍听牌）", self.now)

    def _commit_riichi_with(self, index: int) -> bool:
        """宣言模式里点手牌：命中 `riichi_tiles` 才发帧，否则只提示（不碰服务端）。"""
        state = self.state
        if not 0 <= index < len(state.hand):
            return False
        candidates = self._riichi_candidates()
        if candidates and tile_face(state.hand[index]) not in candidates:
            self._toast("这张牌打完不听牌，换一张高亮的牌", self.now)
            return False
        state.selected_index = index
        return self._confirm_riichi()

    def _confirm_discard(self) -> bool:
        state = self.state
        if not self._can_select_hand():
            return False
        index = state.selected_index
        if index is None or not 0 <= index < len(state.hand):
            return False
        tile = state.hand[index]
        seq = state.window.seq if state.window else None
        if seq is None:
            return False
        if not self.send_frame("discard", tile=tile, seq=seq):
            return False
        state.mark_pending("打牌", index)
        log.info("提交打牌：tile=%s seq=%s", tile, seq)
        return True

    def _confirm_riichi(self) -> bool:
        """立直宣言：选中的那张牌既是宣言牌也是打出的牌，走同一条 `action` 帧。"""
        state = self.state
        if not self._can_select_hand():
            return False
        index = state.selected_index
        if index is None or not 0 <= index < len(state.hand):
            return False
        tile = state.hand[index]
        # 客户端先按服务端下发的候选拦一道：不在 `riichi_tiles` 里的牌打完不听，
        # 发出去只会换回一条 ILLEGAL_ACTION。窗口没带候选时（老服务端）放行。
        candidates = self._riichi_candidates()
        if candidates and tile_face(tile) not in candidates:
            self._toast("这张牌打完不听牌，换一张高亮的牌", self.now)
            return False
        seq = state.window.seq if state.window else None
        if seq is None:
            return False
        if not self._do_action("riichi", [tile]):
            return False
        state.mark_pending("立直", index)
        log.info("提交立直：tile=%s seq=%s", tile, seq)
        return True

    def _do_action(self, action: str, tiles: list[str]) -> bool:
        """§5.5 守卫：无窗口不发帧、动作必须在 `options` 内、`seq` 取 `current_seq`。"""
        state = self.state
        window = state.window
        if window is None or state.pending or state.frozen:
            return False
        remaining = self._remaining_ms()
        if remaining is not None and remaining <= 0:
            return False
        if action == "riichi":
            # 日麻：窗口必须提供 riichi，且恰好给出 1 张宣言牌
            if not window.has("riichi") or len(tiles) != 1:
                log.warning("立直需要窗口提供 riichi 且恰好 1 张宣言牌，拒绝发送：%r", tiles)
                return False
        elif action in ("kong", "pong", "chow", "ankan", "kakan"):
            # v2.0：ankan/kakan 的 tiles 恰好 1 个牌面元素（PROTOCOL v1.6 §4.6）
            if action in ("ankan", "kakan") and len(tiles) != 1:
                log.warning("%s 的 tiles 必须恰好 1 个牌面元素，拒绝发送：%r", action, tiles)
                return False
            if not window.has(action, tiles):
                log.warning("动作 %s%s 不在 options 中，拒绝发送", action, tiles)
                return False
        elif action == "win":
            if not window.has("win"):
                return False
        elif action == "pass":
            if window.window not in (WINDOW_CLAIM, WINDOW_CHANKAN) or not window.has("pass", []):
                return False
        if not self.send_frame("action", action=action, tiles=list(tiles), seq=window.seq):
            return False
        # 「已提交：<动作>」与操作栏按钮共用同一文案公式（UI_UX_SPEC v1.4 §5.3 → tiles.button_label）
        state.mark_pending(button_label(action, tiles))
        log.info("提交动作：%s tiles=%s seq=%s", action, tiles, window.seq)
        return True

    # ------------------------------------------------------------ 渲染

    def build_ui(self) -> UiView:
        state = self.state
        now = self.now
        toast_alpha = 0
        if self.toast_text and self.toast_until > 0:
            left = self.toast_until - now
            if left > 0.3:
                toast_alpha = 235
            else:
                toast_alpha = int(235 * max(0.0, left / 0.3))
        notes: list[str] = []
        if self.zero_since is not None and now < self.center_note_until:
            notes.append("本轮将由服务端代为行动")
        if getattr(state, "protocol_warning", False):
            notes.append("协议异常：收到他家手牌字段（已忽略）")
        hover = None
        if self._can_select_hand():
            hover = table_view.hand_hit_test(state, self.mouse_pos)
        return UiView(
            fonts=self.fonts,
            state=state,
            now=now,
            now_epoch_ms=time.time() * 1000.0,
            mouse=self.mouse_pos,
            mouse_down=self.mouse_down,
            lobby_mode=self.lobby_mode,
            lobby_length=self.lobby_length,
            nickname=self.nickname,
            ime_composition=self.ime_composition if self.state.screen == SCREEN_LOGIN else "",
            caret_on=(now % 1.0) < 0.5,
            login_status=self.login_status,
            login_status_color=self.login_status_color,
            login_submit_enabled=self._login_submit_enabled(),
            login_submit_label=self._login_submit_label(),
            login_input_error=self.login_input_error,
            login_retry_visible=self.login_retry_visible,
            login_mode=self.login_mode,
            login_account=self.account,
            login_password_masked="●" * len(self.password),
            login_password_error=self.login_password_error,
            login_focus=self.login_focus,
            remember_password=self.remember_password,
            login_register_enabled=self._login_register_enabled(),
            stats=self._stats_view(),
            stats_open=self.stats_open,
            has_account_session=self.has_account,
            lobby_create_enabled=self._lobby_create_enabled(),
            lobby_begin_enabled=self._lobby_begin_enabled(),
            open_rooms=list(self.open_rooms),
            host=self.host,
            port=self.port,
            connected=self.connected,
            rtt_ms=self.rtt_ms,
            illegal_count=state.error_count,
            banner_text=self.banner_text,
            banner_color=self.banner_color,
            banner_action=self.banner_action,
            disconnect_reason=self.disconnect_reason,
            error_code=(state.last_error or {}).get("code") if self.error_until > now else None,
            error_message=self.error_message if self.error_until > now else None,
            error_visible=self.error_until > now and state.last_error is not None,
            pending_label=state.pending_label,
            hand_hover_index=hover,
            debug=self.debug,
            toast_text=self.toast_text,
            toast_alpha=toast_alpha,
            extra_notes=notes,
            pending_flash=self.now < self.pending_flash_until,
            # 结算播报的只读快照（`Broadcast.view()` 是纯读取，不含写接口）
            bc=self.bc.view(),
        )

    def _seated_in_lobby(self) -> bool:
        return (
            self.state.screen == SCREEN_LOBBY
            and isinstance(self.state.room_id, str)
            and bool(self.state.room_id)
            and isinstance(self.state.my_seat, int)
            and not self.state.game_started
        )

    def _lobby_create_enabled(self) -> bool:
        """「新建房间」：还没入座，且没有在途的建桌/加入。"""
        return (
            self.connected
            and self.login_stage == "IDLE"
            and not self.disconnect_reason
            and not self._seated_in_lobby()
        )

    def _lobby_begin_enabled(self) -> bool:
        """「开始对局」：已经 `room_joined`，对局还没开始。"""
        return (
            self.connected
            and self.login_stage == "IDLE"
            and not self.disconnect_reason
            and self._seated_in_lobby()
        )

    def _login_submit_enabled(self) -> bool:
        if self.login_stage != "IDLE":
            return False
        if self.login_mode == "account":
            return bool(self.account.strip()) and bool(self.password)
        name = self.nickname.strip()
        return bool(name) and len(name) <= NICKNAME_MAX

    def _login_register_enabled(self) -> bool:
        # 注册要的是同两个字段（账号名 + 密码），本地先要求非空，规则细节交给服务端判定。
        return self._login_submit_enabled()

    def _login_submit_label(self) -> str:
        if self.login_stage == "CONNECTING":
            return "连接中…"
        if self.login_stage in ("WAIT_LOGIN_OK", "WAIT_ROOM_JOINED", "WAIT_GAME_START"):
            return "准备中…"
        return "登录" if self.login_mode == "account" else "进入牌桌"

    def render_frame(self, present: bool = True) -> pygame.Surface:
        """绘制一帧到逻辑分辨率画布；`present=True` 时再缩放呈现到窗口。"""
        ui = self.build_ui()
        surface = self.logical
        if ui.state.screen == SCREEN_LOGIN:
            login_view.render(surface, ui)
        elif ui.state.screen == SCREEN_TABLE:
            table_view.render(surface, ui, self.feel)
            tenpai_view.render(surface, ui, self.feel)
            self._render_flights(surface, ui)
            actionbar.render(surface, ui, self._remaining_ms())
        elif ui.state.screen == SCREEN_LOBBY:
            lobby_view.render(surface, ui)
        elif ui.state.screen == SCREEN_RESULT:
            table_view.render(surface, ui, self.feel)
            result_view.render(surface, ui)
        overlays.render(surface, ui, self._bar_visible())
        # 战绩弹窗画在最上层（但断线蒙层优先：断线时 `overlays.render` 已经接管了整屏）
        if ui.stats_open and ui.state.screen == SCREEN_TABLE and not ui.disconnect_reason:
            stats_view.render_overlay(surface, ui)
        if present and self.screen_surface is not None:
            self._present(surface)
        return surface

    def _render_flights(self, surface: pygame.Surface, ui: UiView) -> None:
        """F-AN-10..17：飞行中的出牌画在牌桌之上（牌河对应格在 `table_view` 里被跳过）。"""
        if not self.feel.flights:
            return
        now = ui.now
        for flight in self.feel.flights:
            table_view.draw_flight_tile(
                surface,
                ui,
                flight.rect(now),
                flight.tile,
                side=flight.side,
                seat=flight.seat,
                feel=self.feel,
            )

    def _present(self, surface: pygame.Surface) -> None:
        target = self.screen_surface
        if target is None:
            return
        if self.scale == 1.0:
            target.blit(surface, self.offset)
        else:
            scaled = pygame.transform.smoothscale(surface, T.scaled_size(self.scale))
            target.fill((0, 0, 0))
            target.blit(scaled, self.offset)
        pygame.display.flip()
