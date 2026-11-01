"""渲染层只读快照（UI_UX_SPEC §11：渲染函数签名保持纯读取）。

`GameApp` 每帧构造一个 `UiView` 交给 `render/*`；渲染层不得访问网络或状态写接口。
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .broadcast import BroadcastView
from .game_state import GameState
from .theme import FontBook


@dataclass
class UiView:
    fonts: FontBook
    state: GameState
    now: float
    now_epoch_ms: float
    mouse: tuple[float, float] = (0.0, 0.0)
    mouse_down: bool = False

    # 登录界面
    nickname: str = ""
    # 输入法组字预览（拼音），不计入昵称，确认后才进入 nickname。
    ime_composition: str = ""
    caret_on: bool = True
    login_status: str = ""
    login_status_color: tuple = (176, 180, 176)
    login_submit_enabled: bool = False
    login_submit_label: str = "进入牌桌"
    login_input_error: bool = False
    login_retry_visible: bool = False
    host: str = "127.0.0.1"
    port: int = 9527
    # 账号模式（ACCOUNT_DESIGN §5）。密码**只以掩码字符串进入视图**，明文留在 app 内部，
    # 既不进 UiView（可能被打印/快照），也不进日志。
    login_mode: str = "guest"  # guest | account
    login_account: str = ""
    login_password_masked: str = ""
    login_password_error: bool = False
    login_focus: str = "name"  # name | password
    # 「记住密码」勾选框（UI_UX_SPEC §3.5）：只表示勾选态，真正的凭据文件由 app 读写。
    remember_password: bool = True
    login_register_enabled: bool = False
    login_register_label: str = "注册"
    # 战绩（ACCOUNT_DESIGN §3.4 的 `stats` 帧原样保留，渲染时才格式化）：
    # `stats` 为空表示游客会话或还没收到；`stats_open` 是牌桌弹窗开关。
    stats: dict | None = None
    stats_open: bool = False
    # 账号会话：决定牌桌 HUD 与大厅是否显示战绩（游客的 `stats` 恒为零值，不显示）
    has_account_session: bool = False
    # 大厅（§12）：新建与开局分开。建桌/加入在途时两个按钮都不可再点。
    lobby_start_enabled: bool = True
    lobby_create_enabled: bool = True
    lobby_begin_enabled: bool = False
    open_rooms: list = field(default_factory=list)

    # 连接与全局态
    connected: bool = False
    rtt_ms: float | None = None
    illegal_count: int = 0
    banner_text: str | None = None
    banner_color: tuple = (240, 200, 80)
    banner_action: str | None = None
    disconnect_reason: str | None = None
    error_code: str | None = None
    error_message: str | None = None
    error_visible: bool = False
    pending_label: str | None = None
    hand_hover_index: int | None = None
    debug: bool = False
    toast_text: str = ""
    toast_alpha: int = 0
    extra_notes: list[str] = field(default_factory=list)
    pending_flash: bool = False
    # 结算播报（`client/broadcast.py`）的**只读**快照：渲染层只读它，不推进状态。
    # `None` / `plan is None` ⇒ 本局没有结算页内容（或播报从未起播）。
    bc: BroadcastView | None = None
