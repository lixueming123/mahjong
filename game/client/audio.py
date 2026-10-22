"""音效引擎（FEEL_SPEC v1.7 §5，16 条开源包样本）。

分工边界（§5 权威划分，本模块只实现**播放口径**）：
- 命名与路径（`sfx_<id>.ogg` → `assets/sfx/`）＋ 格式参数：**FEEL_SPEC §5.3 D1/D2**；
- 音色选择（哪个样本配哪个 id）＋ 授权归档：**AUDIO_SPEC.md**；
- 触发点 / 优先级 / 打断 / 时长上界 / 音量与开关：**本模块**实现的硬规则。

实现的常量（逐条对应 §5.1 / §5.4）：
- `F-AU-00`  `mixer.pre_init(44100, -16, 1, 512)` + `set_num_channels(16)`；
- `F-AU-00b` 主音量 0.70，组乘子 WIN 1.00 / ACTION 0.80 / UI 0.55；
- `F-AU-00d` 启动期一次性预加载 16 条，运行时零解码、零磁盘 IO；
- `F-AU-20` 并发上限 3（WIN 1 + ACTION 1 + UI 1），各占独立通道；
- `F-AU-21` P0 起播 ⇒ ACTION/UI 通道 `fadeout(30ms)` 后停；
- `F-AU-22` ACTION 组内新音立即抢占（`stop()` 后起播，不交叉淡入）；
- `F-AU-23` UI 组只与自己抢，不打断 ACTION/WIN；
- `F-AU-24` `(id, seq)` 200 ms 去重；
- `F-AU-25` 静音 / 缺资产 ⇒ 不出声，**但仍走时机判定**（不补播积压）；
- `F-AU-26` 切屏 ⇒ `ch0..2` 全部 `fadeout(40ms)`；
- `F-AU-27` WIN 通道只容 1 条 P0 ⇒ `match_end` 立即抢占 `win_tsumo`（不交叉淡入）。

健壮性底线（§7 R5）：`mixer` 初始化失败（无声卡）时**静默降级**——不崩溃、不阻塞，
其余表现照旧。
"""

from __future__ import annotations

import logging
import os
import time

import pygame

log = logging.getLogger("mahjong.client.audio")

# ---------------------------------------------------------------- 常量

MASTER_VOLUME = 0.70          # F-AU-00b
GROUP_VOLUME = {              # F-AU-00b
    "WIN": 1.00,
    "ACTION": 0.80,
    "UI": 0.55,
}
CHANNEL_WIN = 0
CHANNEL_ACTION = 1
CHANNEL_UI = 2
CHANNEL_COUNT = 16            # F-AU-00
P0_FADE_MS = 30               # F-AU-21
SCREEN_FADE_MS = 40           # F-AU-26
DEDUP_MS = 200                # F-AU-24

#: 16 条样本 id（FEEL_SPEC §5.2）→ (文件 id, 通道组, 优先级)
SFX: dict[str, tuple[str, str, int]] = {
    "ui_click": ("ui_click", "UI", 3),
    "tile_draw": ("tile_draw", "ACTION", 1),
    "tile_discard": ("tile_discard", "ACTION", 1),
    "call_pong": ("call_pong", "ACTION", 1),
    "call_chow": ("call_chow", "ACTION", 1),
    "call_kong": ("call_kong", "ACTION", 1),
    "call_ankan": ("call_ankan", "ACTION", 1),
    "call_kakan": ("call_kakan", "ACTION", 1),
    "win_ron": ("win_ron", "WIN", 0),
    "win_tsumo": ("win_tsumo", "WIN", 0),
    "draw_hand": ("draw_hand", "ACTION", 2),
    "turn_start": ("turn_start", "UI", 1),
    "countdown_tick": ("countdown_tick", "UI", 2),
    "tenpai_on": ("tenpai_on", "UI", 2),
    "hand_end": ("hand_end", "ACTION", 2),
    "match_end": ("match_end", "WIN", 0),
}

#: 三条硬编码的时长上界（ms），只用于日志巡检——**不做裁剪**（裁剪属资产侧 D3）
MAX_DURATION_MS = {
    "ui_click": 80, "tile_draw": 180, "tile_discard": 100, "call_pong": 180,
    "call_chow": 180, "call_kong": 260, "call_ankan": 260, "call_kakan": 240,
    "win_ron": 1200, "win_tsumo": 1400, "draw_hand": 900, "turn_start": 220,
    "countdown_tick": 60, "tenpai_on": 300, "hand_end": 400, "match_end": 1600,
}

_pre_init_done = False


def ensure_pre_init() -> None:
    """F-AU-00：必须在 `pygame.init()`（它会连带 `mixer.init()`）**之前**调用。

    本模块在 import 时就调用一次；`app.py` 的 `pygame.init()` 在其 `__init__` 里，
    因此 import 顺序天然满足。
    """
    global _pre_init_done
    if _pre_init_done:
        return
    _pre_init_done = True
    try:
        pygame.mixer.pre_init(frequency=44100, size=-16, channels=1, buffer=512)
    except Exception as exc:  # pragma: no cover - 无音频设备
        log.warning("mixer.pre_init 失败，将静默降级：%s", exc)


ensure_pre_init()


def _assets_dir() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "sfx")


class AudioEngine:
    """16 条样本的播放引擎。缺资产 ⇒ 静音占位，不崩溃、不阻塞启动（F-AU-00c / D5）。"""

    def __init__(self, assets_dir: str | None = None, enabled: bool = True) -> None:
        self.assets_dir = assets_dir or _assets_dir()
        self.muted = not enabled      # F-AU-25：`M` 键切换
        self.countdown_enabled = False  # F-AU-13：默认关
        self.available = False
        self.sounds: dict[str, pygame.mixer.Sound | None] = {}
        self._last_play: dict[tuple[str, object], float] = {}
        self._channels: dict[str, pygame.mixer.Channel | None] = {
            "WIN": None, "ACTION": None, "UI": None,
        }
        self._load()

    # ------------------------------------------------------------ 加载

    def _load(self) -> None:
        try:
            if not pygame.mixer.get_init():
                pygame.mixer.init()
        except Exception as exc:  # pragma: no cover
            log.warning("mixer 初始化失败，音效静默降级：%s", exc)
            self.available = False
            for sid in SFX:
                self.sounds[sid] = None
            return
        try:
            pygame.mixer.set_num_channels(CHANNEL_COUNT)
        except Exception as exc:  # pragma: no cover
            log.warning("set_num_channels 失败：%s", exc)
        for sid, (file_id, _group, _prio) in SFX.items():
            path = os.path.join(self.assets_dir, f"sfx_{file_id}.ogg")
            snd = None
            if os.path.isfile(path):
                try:
                    snd = pygame.mixer.Sound(path)
                except Exception as exc:
                    log.warning("音效加载失败（静音占位）：%s — %s", path, exc)
            else:
                log.warning("音效缺失（静音占位）：%s", path)
            self.sounds[sid] = snd
        try:
            self._channels["WIN"] = pygame.mixer.Channel(CHANNEL_WIN)
            self._channels["ACTION"] = pygame.mixer.Channel(CHANNEL_ACTION)
            self._channels["UI"] = pygame.mixer.Channel(CHANNEL_UI)
            self.available = True
        except Exception as exc:  # pragma: no cover
            log.warning("通道分配失败，音效静默降级：%s", exc)
            self.available = False
        loaded = sum(1 for v in self.sounds.values() if v is not None)
        log.info("音效预加载：%d/%d 条（muted=%s）", loaded, len(SFX), self.muted)

    # ------------------------------------------------------------ 播放

    def play(self, sound_id: str, seq: object = None) -> bool:
        """播一条样本。返回是否真的出声。

        **无论是否出声，时机判定都已经走完**（F-AU-25）：静音/缺资产只影响「有没有声音」，
        不影响「有没有判定」，因此重开或补齐后不会补播积压。
        """
        spec = SFX.get(sound_id)
        if spec is None:
            log.warning("未知音效 id：%s", sound_id)
            return False
        _file_id, group, priority = spec
        now = time.monotonic() * 1000.0
        # F-AU-24：同一 (id, seq) 200 ms 内重复到达 ⇒ 丢弃
        key = (sound_id, seq)
        last = self._last_play.get(key)
        if last is not None and now - last < DEDUP_MS:
            return False
        self._last_play[key] = now
        if sound_id == "countdown_tick" and not self.countdown_enabled:
            return False  # F-AU-13 默认关
        if self.muted or not self.available:
            return False
        snd = self.sounds.get(sound_id)
        if snd is None:
            return False
        channel = self._channels.get(group)
        if channel is None:
            return False

        if priority == 0:
            # F-AU-21：P0 起播 ⇒ ACTION / UI 强制静场
            for other in ("ACTION", "UI"):
                ch = self._channels.get(other)
                if ch is not None:
                    try:
                        ch.fadeout(P0_FADE_MS)
                    except Exception:  # pragma: no cover
                        pass
        # F-AU-22 / F-AU-27：同组（含 WIN 组的 P0 vs P0）立即抢占，不做交叉淡入
        try:
            channel.stop()
            channel.play(snd)
            channel.set_volume(MASTER_VOLUME * GROUP_VOLUME[group])
        except Exception as exc:  # pragma: no cover
            log.warning("播放 %s 失败：%s", sound_id, exc)
            return False
        return True

    def stop_all(self, fade_ms: int = SCREEN_FADE_MS) -> None:
        """F-AU-26：切屏静场（`ch0..2` 全部 fadeout）。"""
        if not self.available:
            return
        for channel in self._channels.values():
            if channel is None:
                continue
            try:
                channel.fadeout(fade_ms)
            except Exception:  # pragma: no cover
                pass

    def set_muted(self, muted: bool) -> None:
        self.muted = muted
        if muted:
            self.stop_all(0)

    def toggle_muted(self) -> bool:
        self.set_muted(not self.muted)
        return self.muted

    def toggle_countdown(self) -> bool:
        self.countdown_enabled = not self.countdown_enabled
        return self.countdown_enabled
