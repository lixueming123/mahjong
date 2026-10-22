#!/usr/bin/env python3
"""客户端入口（PROTOCOL.md §1.8 冻结的启动参数）。

    ./.venv/bin/python game/client/main.py --host 127.0.0.1 --port 9527
"""

from __future__ import annotations

import argparse
import logging
import os
import sys

if __package__ in (None, ""):  # 允许 `python game/client/main.py` 直接运行
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from client.app import GameApp
else:
    from .app import GameApp

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9527


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="麻将 · ex_actor pygame 客户端")
    parser.add_argument("--host", default=DEFAULT_HOST, help="目标服务器地址（默认 127.0.0.1）")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="目标服务器端口（默认 9527）")
    parser.add_argument("--fullscreen", action="store_true", help="全屏显示（非验收项）")
    parser.add_argument(
        "--no-feel",
        action="store_true",
        help="关闭 B 批手感表现：不播任何动画（音效仍受 M 键控制），用于自动化与性能比对",
    )
    parser.add_argument("--log-level", default="INFO", help="日志级别（DEBUG/INFO/WARNING）")
    return parser


def cli_main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, str(args.log_level).upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    logging.getLogger("mahjong.client").info(
        "启动客户端：目标 %s:%s", args.host, args.port
    )
    app = GameApp(host=args.host, port=args.port, fullscreen=args.fullscreen, no_feel=args.no_feel)
    try:
        app.run()
    except KeyboardInterrupt:  # pragma: no cover - 交互式中断
        app.shutdown()
    finally:
        # 退出时把三条通道收干净（F-AU-26 的兜底：避免进程尾巴上拖一段余韵）
        try:
            app.audio.stop_all(0)
        except Exception:  # pragma: no cover
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(cli_main())
