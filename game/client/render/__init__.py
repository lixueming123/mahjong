"""渲染层（UI_UX_SPEC §11「renderer」）。

约定：`render(surface, state, ui)` 是**纯函数式**读取，只画不改、不发帧。
"""

from __future__ import annotations

from . import actionbar, login, overlays, result, table, widgets

__all__ = ["actionbar", "login", "overlays", "result", "table", "widgets"]
