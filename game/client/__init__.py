"""麻将 · ex_actor pygame 客户端（t5）。

四类界面（UI_UX_SPEC §2.1）：
- `SCREEN_LOGIN` 登录界面
- `SCREEN_TABLE` 牌桌界面
- `SCREEN_ACTIONBAR` 操作界面（与牌桌同屏叠加，见 `render/actionbar.py`）
- `SCREEN_RESULT` 结算界面

线协议见 `game/docs/PROTOCOL.md`（NDJSON over TCP）。
"""

__all__ = ["app", "game_state", "net_client", "tiles", "theme", "view"]
