#!/usr/bin/env python3
"""侧家牌桌布局 PNG（副露 / 牌河 / 牌山）。改 SIDE_* 或 table 渲染后跑此脚本验图。"""
from __future__ import annotations

import os
import sys
import time

os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
os.environ.setdefault("SDL_VIDEODRIVER", "dummy")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import pygame  # noqa: E402
from protocol.enums import FRAME_TYPE, MELD_TYPE  # noqa: E402

from tests.client_headless_test import (  # noqa: E402
    ARTIFACTS,
    CLAIM_REQUEST,
    GAME_START,
    ROOM_JOINED,
    make_app,
)

SIDE_LAYOUT = {
    "type": FRAME_TYPE.STATE_SYNC,
    "seq": 12,
    "remaining": 24,
    "current_seat": 0,
    "hand": ["1m"] * 13,
    "melds": [],
    "last_discard_seat": 3,
    "seats": [
        {
            "seat": 0,
            "name": "abc",
            "is_bot": False,
            "hand_count": 13,
            "discards": [],
            "melds": [],
            "disconnected": False,
        },
        {
            "seat": 1,
            "name": "AI-1",
            "is_bot": True,
            "hand_count": 6,
            "discards": ["1m", "2m", "3m", "4m", "5m", "6m", "7m", "8m"],
            "melds": [{"type": MELD_TYPE.PONG, "tiles": ["7z", "7z", "7z"]}],
            "disconnected": False,
        },
        {
            "seat": 2,
            "name": "AI-2",
            "is_bot": True,
            "hand_count": 10,
            "discards": ["9p"] * 6,
            "melds": [],
            "disconnected": False,
        },
        {
            "seat": 3,
            "name": "AI-3",
            "is_bot": True,
            "hand_count": 5,
            "discards": ["1s", "2s", "3s", "4s", "5s", "6s", "7s", "8s"],
            "melds": [{"type": MELD_TYPE.CHOW, "tiles": ["4p", "5p", "6p"]}],
            "disconnected": False,
        },
    ],
}


def save(app, name: str) -> str:
    os.makedirs(ARTIFACTS, exist_ok=True)
    path = os.path.join(ARTIFACTS, name)
    app.now = time.monotonic()
    pygame.image.save(app.render_frame(present=True), path)
    return path


def main() -> None:
    app = make_app()
    app.state.apply(GAME_START)
    app._cache_room_joined(ROOM_JOINED)
    app.state.apply(SIDE_LAYOUT)
    paths = [
        save(app, "client_side_meld_layout.png"),
        save(app, "client_side_layout.png"),
    ]
    claim = dict(CLAIM_REQUEST)
    claim["deadline_ms"] = int(time.time() * 1000) + 15000
    claim["seq"] = 20
    sync = dict(SIDE_LAYOUT)
    sync["seq"] = 13
    sync["seats"] = [dict(s) for s in SIDE_LAYOUT["seats"]]
    sync["seats"][3] = dict(sync["seats"][3])
    sync["seats"][3]["discards"] = list(sync["seats"][3]["discards"]) + ["2p"]
    app.state.apply(sync)
    app.state.apply(claim)
    paths.append(save(app, "client_side_claim_highlight.png"))
    for path in paths:
        print(path)


if __name__ == "__main__":
    main()
