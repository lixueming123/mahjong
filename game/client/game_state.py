"""客户端唯一状态源（UI_UX_SPEC §11「game_state」）。

设计约束（冻结文档逐条落实）：

- 只有本模块能改状态；渲染层**只读**（`render/*` 只接收快照对象）。
- `state_sync` 为权威整体覆盖；`discard_event` / `draw` / `action_event` 为即时增量。
- 私有信息隔离（§7.2/§7.3）：`SeatView` 的字段被固定为
  `seat, name, is_bot, hand_count, discards, melds, disconnected`，
  **不存在**任何可存放他家牌面的字段；即便服务端缺陷下发 `seats[].hand`，
  客户端也 **不消费**、不写入状态，只记一条 WARN。
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Any

from .tiles import is_valid_tile, remove_leftmost, sort_tiles, tile_face

log = logging.getLogger("mahjong.client.state")

SCREEN_LOGIN = "SCREEN_LOGIN"
# 大厅（UI_UX_SPEC §12）：登录成功后先到这里。新建/加入只到 `room_joined`，
# 「开始对局」才发 `ready`。
SCREEN_LOBBY = "SCREEN_LOBBY"
SCREEN_TABLE = "SCREEN_TABLE"
SCREEN_RESULT = "SCREEN_RESULT"

REL_LABELS = ("自家", "下家", "对家", "上家")

WINDOW_DISCARD = "discard"
WINDOW_CLAIM = "claim"
# 日麻抢杠窗口（v1.15）：与 claim 同为「只能和/过」，但被抢的那张牌不在牌河里，
# 所以帧里额外带 `tile`，客户端据此显示「抢杠 5m」。
WINDOW_CHANKAN = "chankan"

MELD_TYPES = ("pong", "chow", "kong", "ankan", "kakan")  # v2.0：3 → 5 值（PROTOCOL v1.6 §5.9）

KAN_TYPES = ("kong", "ankan", "kakan")  # 张数第二道校验用：kans = 杠组数（§4.5 v2.0）


@dataclass
class Meld:
    type: str
    tiles: list[str] = field(default_factory=list)

    @property
    def label(self) -> str:
        # §4.8：组框右上角小字标注，v2.0 扩五种
        return {"pong": "碰", "chow": "吃", "kong": "杠", "ankan": "暗杠", "kakan": "加杠"}.get(
            self.type, str(self.type)
        )

    @property
    def is_concealed_kan(self) -> bool:
        """§4.8 暗杠两形态：本家恒 4 张牌面；他家揭示前 tiles == []（渲染 4 张牌背）。"""
        return self.type == "ankan"


def parse_meld(obj: Any) -> Meld | None:
    if not isinstance(obj, dict):
        log.warning("副露元素不是对象，已忽略：%r", type(obj).__name__)
        return None
    mtype = obj.get("type")
    tiles = obj.get("tiles")
    if mtype not in MELD_TYPES or not isinstance(tiles, list):
        log.warning("副露元素形状非法，已忽略：type=%r", mtype)
        return None
    clean = [t for t in tiles if is_valid_tile(t)]
    if len(clean) != len(tiles):
        log.warning("副露含非法牌面，已过滤：%r", tiles)
    return Meld(type=mtype, tiles=clean)


@dataclass
class SeatView:
    """单个座位的**公开**视图（§7.2 数据层保证：无他家牌面字段）。"""

    seat: int
    name: str = ""
    is_bot: bool = False
    hand_count: int = 0
    discards: list[str] = field(default_factory=list)
    melds: list[Meld] = field(default_factory=list)
    disconnected: bool = False
    riichi: bool = False  # 日麻：该座位本局是否立直（公开信息，立直棒/横置据此渲染）


@dataclass
class ActionWindow:
    seq: int
    deadline_ms: int
    window: str
    options: list[dict]
    recv_monotonic: float
    recv_epoch_ms: float
    window_ms: int
    # 只有抢杠窗口带：被抢的那张牌（claim/discard 窗口为 None，语义不变）
    tile: str | None = None
    # v1.21 §4.4：立直宣言牌的候选**牌面**（只有日麻打牌窗口带 `riichi` 选项时才有值）。
    # 服务端按「打出后仍在听牌、且不落振听」算好再下发；空 = 不能立直。
    riichi_tiles: list[str] = field(default_factory=list)

    def has(self, action: str, tiles: list[str] | None = None) -> bool:
        for opt in self.options:
            if opt.get("action") != action:
                continue
            if tiles is None:
                return True
            if list(opt.get("tiles") or []) == list(tiles):
                return True
        return False

    def option_list(self, action: str) -> list[list[str]]:
        out: list[list[str]] = []
        for opt in self.options:
            if opt.get("action") == action:
                out.append(list(opt.get("tiles") or []))
        return out


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


class GameState:
    """所有牌桌数据的唯一持有者。"""

    def __init__(self) -> None:
        self.reset_all()

    # ------------------------------------------------------------ 生命周期

    def reset_all(self) -> None:
        self.screen = SCREEN_LOGIN
        self.phase = "登录"          # HUD 中段文案：登录 / 对局中 / 结算
        self.reset_table()

    def enter_lobby(self) -> None:
        """进入大厅（§12）：清掉上一局的对局状态，但不碰连接与会话身份。"""
        self.reset_table()
        self.phase = "大厅"
        self.screen = SCREEN_LOBBY

    def reset_table(self) -> None:
        self.room_id: str | None = None
        self.my_seat: int | None = None
        self.dealer: int | None = None
        # v2.0 场次（§4.11）：round_index/round_count 来自 game_start；renchan_count 来自
        # game_end 且**跨局保留**（连庄由局终帧累计，game_start 不携带、不得清零）。
        self.round_index: int = 0
        self.round_count: int = 4
        self.renchan_count: int = 0
        self.hand: list[str] = []
        self.melds: list[Meld] = []
        self.seats: list[SeatView] = [SeatView(seat=i) for i in range(4)]
        self.remaining: int | None = None
        # v1.22：当前房间玩法（`classic` / `riichi`）。协议里**没有** mode 字段（game_start
        # 不得为 classic 加键——帧逐字节冻结），所以只能由「日麻独有字段」反推：
        # 日麻恒带 points，classic 不带。用于场况卡徽标与「为什么没有立直」的解释。
        self.mode: str = "classic"
        # 日麻（mode=riichi）：持点 / 宝牌指示牌 / 荒庄听牌标记。classic 下恒为 None/空。
        self.points: list[int] | None = None
        self.dora_indicators: list[str] = []
        self.tenpai_flags: list[bool] = []
        self.current_seat: int | None = None
        self.seq: int | None = None
        self.last_seq: int | None = None
        self.window: ActionWindow | None = None
        self.pending: bool = False
        self.pending_label: str | None = None
        self.pending_index: int | None = None
        self.selected_index: int | None = None
        # v1.21：立直宣言模式（点了「立直」但还没选宣言牌）。开启后只有
        # `window.riichi_tiles` 里的牌可点，点哪张就直接宣言并打出。
        self.riichi_arming: bool = False
        self.drawn_index: int | None = None
        self.frozen: bool = False           # 收到 action_event{win} 后冻结牌桌输入
        self.result: dict | None = None     # game_end 原始载荷（局终页）
        self.result_at: float | None = None  # 局终页倒计时基准（§9.2 下一局提示 5 s）
        self.match_end: dict | None = None  # match_end 原始载荷（场终页，§9.6；None = 未场终）
        self.win_seat: int | None = None
        self.hand_warning: bool = False
        self.protocol_warning: bool = False  # 收到 seats[].hand（服务端缺陷）时置位
        self.game_started: bool = False
        self.error_count: int = 0
        self.last_error: dict | None = None
        self.recv_ms: dict[str, float] = {}
        # B 批（PROTOCOL v1.7 §5.16）：听牌条——**仅本家**、独立 `tenpai_seq`（不掺 room.seq）。
        self.tenpai_seq: int | None = None
        self.tenpai_waits: list[dict] = []
        self.is_tenpai: bool = False
        # 日麻（v1.13 §12.8）：形听但不能和。classic 帧不带该键 ⇒ 恒 False，听牌条行为不变。
        self.tenpai_no_yaku: bool = False
        self.last_discard_seat: int | None = None   # B 批：F-CL-01 被鸣牌定位（公开派生）

    # ------------------------------------------------------------ 派生查询

    def seat_view(self, seat: int) -> SeatView:
        for view in self.seats:
            if view.seat == seat:
                return view
        return SeatView(seat=seat)

    def rel_index(self, seat: int) -> int:
        """座位 → 相对方位下标：0 自家 / 1 下家 / 2 对家 / 3 上家（§4.2）。"""
        if self.my_seat is None:
            return seat
        return (seat - self.my_seat) % 4

    def rel_label(self, seat: int) -> str:
        return REL_LABELS[self.rel_index(seat)]

    def name_of(self, seat: int) -> str:
        view = self.seat_view(seat)
        return view.name or f"座位{seat}"

    def remaining_ms(self, now_mono: float, now_epoch_ms: float) -> float | None:
        """§6.2：显示用剩余 = max(0, min(服务端剩余, 本地兜底剩余))。"""
        win = self.window
        if win is None:
            return None
        remaining_server = win.deadline_ms - now_epoch_ms
        remaining_guard = (win.recv_monotonic + win.window_ms / 1000.0 - now_mono) * 1000.0
        return max(0.0, min(remaining_server, remaining_guard))

    def clock_skew_ms(self, now_mono: float, now_epoch_ms: float) -> float | None:
        win = self.window
        if win is None:
            return None
        remaining_server = win.deadline_ms - now_epoch_ms
        remaining_guard = (win.recv_monotonic + win.window_ms / 1000.0 - now_mono) * 1000.0
        return abs(remaining_server - remaining_guard)

    def kans(self) -> int:
        """杠组数（§4.5 v2.0 第二道校验用）：melds 中 type ∈ {kong, ankan, kakan} 的组数。"""
        return sum(1 for m in self.melds if m.type in KAN_TYPES)

    def check_hand_count(self) -> None:
        """§4.5 张数交叉校验（v2.0 两道）：

        第一道（账面）：`len(hand) + 3*len(melds) ∈ {13,14}`；
        第二道（物理）：`len(hand) + 3*len(melds) + kans`，等待态应为 `13 + kans`、
        行动态 `14 + kans`（kans = 杠组数，第 4 张由 kans 补回）。任一道不过都只 WARN，
        **不修改**本地数据。
        """
        if not self.game_started:
            self.hand_warning = False
            return
        total = len(self.hand) + 3 * len(self.melds)
        kans = self.kans()
        physical = total + kans
        ok = total in (13, 14) and physical in (13 + kans, 14 + kans)
        self.hand_warning = not ok

    # ------------------------------------------------------------ 帧分发

    def apply(self, frame: dict) -> None:
        ftype = frame.get("type")
        handler = {
            "game_start": self._apply_game_start,
            "draw": self._apply_draw,
            "discard_event": self._apply_discard_event,
            "action_request": self._apply_action_request,
            "action_event": self._apply_action_event,
            "state_sync": self._apply_state_sync,
            "tenpai": self._apply_tenpai,          # v1.7 §5.16（B 批）
            "game_end": self._apply_game_end,
            "match_end": self._apply_match_end,  # v2.0：场终帧（§9.6）
        }.get(ftype)
        if handler is None:
            log.debug("忽略未消费的帧类型：%r", ftype)
            return
        handler(frame)

    # ------------------------------------------------------------ 各帧处理

    def _apply_game_start(self, frame: dict) -> None:
        hand = frame.get("hand")
        if not isinstance(hand, list):
            log.warning("game_start.hand 非法，按空手牌处理")
            hand = []
        self.hand = [t for t in hand if is_valid_tile(t)]
        if len(self.hand) != len(hand):
            log.warning("game_start.hand 含非法牌面，已过滤")
        seat = frame.get("seat")
        if isinstance(seat, int):
            self.my_seat = seat
        dealer = frame.get("dealer")
        if isinstance(dealer, int):
            self.dealer = dealer
        # v2.0（§10.1 game_start 行）：整体重置牌桌 + 场次进度。**renchan_count 不在此清零**
        # （连庄由局终帧累计、跨局保留；game_start 不携带该字段）。
        round_index = frame.get("round_index")
        if isinstance(round_index, int) and 0 <= round_index < 64:
            self.round_index = round_index
        round_count = frame.get("round_count")
        if isinstance(round_count, int) and round_count > 0:
            self.round_count = round_count
        # v1.22：玩法反推（协议无 mode 键）。日麻的 game_start 恒写 `points`（4 家持点），
        # classic 不写；这里以「有没有 points 数组」为唯一判据，别的地方不要再各自猜。
        self.mode = "riichi" if isinstance(frame.get("points"), list) else "classic"
        self._absorb_riichi_fields(frame, reset=True)
        self.melds = []
        self.remaining = None
        self.seq = None
        self.last_seq = None
        self.window = None
        self.pending = False
        self.pending_label = None
        self.selected_index = None
        self.riichi_arming = False
        self.drawn_index = None
        self.frozen = False
        self.result = None
        self.result_at = None
        self.match_end = None
        self.win_seat = None
        self.game_started = True
        self.screen = SCREEN_TABLE
        self.phase = "对局中"
        self.check_hand_count()
        self.tenpai_seq = None   # 新场次 ⇒ 服务端也归零（§5.16）；此处跟随
        self.clear_tenpai()      # F-TP-12：game_start 无条件清空上一局的 waits 与动画状态

        # 保留 room_joined 缓存的昵称 / is_bot，清空对局数据
        for view in self.seats:
            view.hand_count = 0
            view.discards = []
            view.melds = []
            view.disconnected = False
        log.info(
            "开局：seat=%s dealer=%s round=%s/%s hand=%d 张",
            self.my_seat, self.dealer, self.round_index + 1, self.round_count, len(self.hand),
        )

    def _apply_draw(self, frame: dict) -> None:
        seat = frame.get("seat")
        tile = frame.get("tile")
        self._note_seq(frame)
        if seat != self.my_seat:
            # PROTOCOL §5.5：draw 只发给摸牌者本人；他家 draw 属服务端缺陷，绝不消费。
            log.warning("收到他家 draw（协议异常），已忽略")
            return
        if not is_valid_tile(tile):
            log.warning("draw.tile 非法：%r，已忽略", tile)
            return
        self.hand.append(tile)
        self.hand = sort_tiles(self.hand)
        # §4.5：标记同牌面中最左的一张为 drawn
        self.drawn_index = self.hand.index(tile) if tile in self.hand else None
        self.check_hand_count()

    def _apply_discard_event(self, frame: dict) -> None:
        seat = frame.get("seat")
        tile = frame.get("tile")
        self._note_seq(frame)
        if not isinstance(seat, int):
            log.warning("discard_event.seat 非法，已忽略")
            return
        if not is_valid_tile(tile):
            log.warning("discard_event.tile 非法：%r，已忽略", tile)
            return
        self.seat_view(seat).discards.append(tile)
        # B 批（FEEL_SPEC §3.2 F-CL-01）：被鸣牌 = **最近一次被打出的那张**，定位需要它。
        # 这是客户端从公开信息派生的视觉事实，不新增协议字段。
        self.last_discard_seat = seat
        if seat == self.my_seat:
            if not remove_leftmost(self.hand, tile):
                log.warning("本家 discard_event 的牌不在手牌中：%s", tile)
            self.drawn_index = None
            self.check_hand_count()
            # §5.1 兜底规则：本家打牌已生效 → 立即清除 PENDING 与选中
            self.clear_pending()

    def _apply_action_request(self, frame: dict) -> None:
        seq = frame.get("seq")
        if not isinstance(seq, int):
            log.warning("action_request.seq 非法，已忽略")
            return
        if self.last_seq is not None and seq < self.last_seq:
            log.warning("收到过期 action_request(seq=%s < %s)，已忽略", seq, self.last_seq)
            return
        window = frame.get("window")
        if window not in (WINDOW_DISCARD, WINDOW_CLAIM, WINDOW_CHANKAN):
            log.warning("action_request.window 非法：%r，已忽略", window)
            return
        options = frame.get("options")
        if not isinstance(options, list):
            log.warning("action_request.options 非法，按空处理")
            options = []
        clean_options: list[dict] = []
        for opt in options:
            if not isinstance(opt, dict):
                continue
            action = opt.get("action")
            tiles = opt.get("tiles")
            if not isinstance(action, str):
                continue
            clean_options.append({"action": action, "tiles": list(tiles) if isinstance(tiles, list) else []})

        now_mono = time.monotonic()
        now_epoch = time.time() * 1000.0
        deadline = frame.get("deadline_ms")
        if not isinstance(deadline, (int, float)):
            deadline = now_epoch
        window_ms = int(clamp(float(deadline) - now_epoch, 1000.0, 60000.0))

        chankan_tile = frame.get("tile")
        if window == WINDOW_CHANKAN and not is_valid_tile(chankan_tile):
            log.warning("chankan 窗口缺少合法 tile：%r", chankan_tile)
            chankan_tile = None

        # v1.21：立直宣言牌候选。服务端恒发普通牌面码（5m 而不是 0m），
        # 这里仍归一化一次，免得老/新服务端的赤 5 写法差异让标记漏掉手上那张赤 5。
        riichi_tiles: list[str] = []
        raw_riichi_tiles = frame.get("riichi_tiles")
        if isinstance(raw_riichi_tiles, list):
            for code in raw_riichi_tiles:
                if not is_valid_tile(code):
                    log.warning("riichi_tiles 含非法牌码：%r（已忽略）", code)
                    continue
                face = tile_face(code)
                if face not in riichi_tiles:
                    riichi_tiles.append(face)

        self.last_seq = seq
        self.seq = seq
        self.window = ActionWindow(
            seq=seq,
            deadline_ms=int(deadline),
            window=window,
            options=clean_options,
            recv_monotonic=now_mono,
            recv_epoch_ms=now_epoch,
            window_ms=window_ms,
            tile=chankan_tile if window == WINDOW_CHANKAN else None,
            riichi_tiles=riichi_tiles,
        )
        self.pending = False
        self.pending_label = None
        self.selected_index = None
        self.riichi_arming = False
        self.recv_ms[window] = now_mono
        log.debug("窗口开启 seq=%s window=%s options=%s", seq, window, [o["action"] for o in clean_options])

    def _apply_action_event(self, frame: dict) -> None:
        seat = frame.get("seat")
        action = frame.get("action")
        tiles = frame.get("tiles")
        from_seat = frame.get("from_seat")
        self._note_seq(frame)
        if not isinstance(tiles, list):
            tiles = []
        if action in ("pong", "chow", "kong"):
            # §4.7/§5.8：被鸣牌恒为 from_seat 座位最近一次打出的那张 → 从 from_seat 的牌河移除最后一张
            if isinstance(from_seat, int):
                river = self.seat_view(from_seat).discards
                if river:
                    river.pop()
                else:
                    log.warning("action_event 的 from_seat 牌河为空：%s", from_seat)
            else:
                log.warning("action_event 缺少 from_seat（协议异常）")
        elif action == "win":
            self.frozen = True
            if isinstance(seat, int):
                self.win_seat = seat
        elif action == "riichi":
            # 日麻：公开事件，供立直棒/横置动画。权威状态随后由 state_sync.seats[].riichi 覆盖。
            if isinstance(seat, int) and 0 <= seat <= 3:
                self.seat_view(seat).riichi = True
        # F-TP-11 ①⑤：本家和牌 / 本家副露生效 ⇒ 立刻清空听牌（牌形已变，旧 waits 是脏数据）
        if action == "win" or action in ("pong", "chow", "kong", "ankan", "kakan"):
            self.clear_tenpai()
        # §5.1 兜底规则：任一 action_event → 本窗口已结算 → 立即清除 PENDING
        self.clear_pending()

    def _apply_state_sync(self, frame: dict) -> None:
        self._note_seq(frame)
        hand = frame.get("hand")
        if isinstance(hand, list):
            self.hand = [t for t in hand if is_valid_tile(t)]
        melds = frame.get("melds")
        if isinstance(melds, list):
            self.melds = [m for m in (parse_meld(x) for x in melds) if m is not None]
        seats = frame.get("seats")
        if isinstance(seats, list):
            for raw in seats:
                if not isinstance(raw, dict):
                    continue
                seat = raw.get("seat")
                if not isinstance(seat, int) or not 0 <= seat <= 3:
                    continue
                if "hand" in raw:
                    # §8.2 第 2 条：即便服务端缺陷下发，也不渲染、不写入本地状态，只记 WARN。
                    log.warning("state_sync.seats[%s] 含 hand 字段（协议异常），已脱敏忽略", seat)
                    self.protocol_warning = True
                view = self.seats[seat]
                view.name = raw.get("name", view.name) if isinstance(raw.get("name"), str) else view.name
                view.is_bot = bool(raw.get("is_bot", view.is_bot))
                count = raw.get("hand_count")
                if isinstance(count, int) and count >= 0:
                    view.hand_count = count
                discards = raw.get("discards")
                if isinstance(discards, list):
                    view.discards = [t for t in discards if is_valid_tile(t)]
                view_melds = raw.get("melds")
                if isinstance(view_melds, list):
                    view.melds = [m for m in (parse_meld(x) for x in view_melds) if m is not None]
                view.disconnected = bool(raw.get("disconnected", False))
                view.riichi = bool(raw.get("riichi", view.riichi))
        remaining = frame.get("remaining")
        if isinstance(remaining, int):
            self.remaining = remaining
        self._absorb_riichi_fields(frame, reset=False)
        current = frame.get("current_seat")
        if isinstance(current, int):
            self.current_seat = current
        self.check_hand_count()

    def _absorb_riichi_fields(self, frame: dict, *, reset: bool) -> None:
        """日麻字段（持点 / 宝牌指示牌 / 荒庄听牌）。

        classic 帧不带这些键，因此**不写**任何值——`points` 保持 None，HUD 与结算页自动回到
        番数制文案，不会出现「0 点」这种假信息。
        """
        points = frame.get("points")
        if isinstance(points, list) and len(points) == 4 and all(isinstance(v, int) for v in points):
            self.points = list(points)
        elif reset:
            self.points = None
        dora = frame.get("dora_indicators")
        if isinstance(dora, list):
            self.dora_indicators = [t for t in dora if is_valid_tile(t)]
        elif reset:
            self.dora_indicators = []
        tenpai = frame.get("tenpai")
        if isinstance(tenpai, list) and len(tenpai) == 4:
            self.tenpai_flags = [bool(v) for v in tenpai]
        elif reset:
            self.tenpai_flags = []

    # ------------------------------------------------------------ 听牌（v1.7 §5.16）

    def clear_tenpai(self) -> None:
        """F-TP-11：清空听牌缓存（八条清零时机的统一落点）。

        `tenpai_seq` **不清零**——它是「每座位一条、跨局不重置」的服务端计数器
        （§5.16），客户端跟着服务端走，本地清零会导致后续帧被误判成过期而丢弃。
        """
        self.tenpai_waits = []
        self.is_tenpai = False
        self.tenpai_no_yaku = False

    def _apply_tenpai(self, frame: dict) -> None:
        """`tenpai` 帧：**仅本家**、独立 `tenpai_seq`、不掺 `room.seq`。

        - F-TP-11 ⑧：`is_tenpai == false` ⇒ 下发即清空（服务端 MUST 发清空信号）；
        - §7 R6：只接受 `tenpai_seq` **严格递增**的帧，乱序/重放一律丢弃（不回退显示）。
        """
        seq = frame.get("tenpai_seq")
        if not isinstance(seq, int):
            log.warning("tenpai 帧缺少 tenpai_seq，已丢弃")
            return
        if self.tenpai_seq is not None and seq <= self.tenpai_seq:
            log.warning("收到过期 tenpai 帧（tenpai_seq=%s <= %s），已丢弃", seq, self.tenpai_seq)
            return
        self.tenpai_seq = seq
        waits_raw = frame.get("waits")
        clean: list[dict] = []
        if isinstance(waits_raw, list):
            for item in waits_raw:
                if not isinstance(item, dict):
                    continue
                tile = item.get("tile")
                remaining = item.get("remaining")
                if not is_valid_tile(tile):
                    log.warning("tenpai.waits 含非法牌面，已过滤：%r", tile)
                    continue
                if not isinstance(remaining, int) or not 0 <= remaining <= 4:
                    # §2.3：`> 4` 视为服务端缺陷（多半误填了 wall_remaining_），照画不崩 + WARN。
                    log.warning("tenpai.waits[%s].remaining=%r 越界（应 0..4），照画不崩", tile, remaining)
                    if not isinstance(remaining, int):
                        remaining = 0
                # `yaku` 只有日麻帧才有；缺省 True ⇒ classic 与老服务端的渲染路径完全不变。
                has_yaku = item.get("yaku", True)
                clean.append({"tile": tile, "remaining": remaining,
                              "yaku": has_yaku if isinstance(has_yaku, bool) else True})
        else:
            log.warning("tenpai.waits 非法，按空处理")
        flag = frame.get("is_tenpai")
        self.tenpai_waits = clean
        self.is_tenpai = bool(flag) if isinstance(flag, bool) else bool(clean)
        no_yaku = frame.get("no_yaku")
        # 帧级标记只做「提示口径」，不改变 is_tenpai：形听仍然要收流局罚符、仍然可以立直。
        self.tenpai_no_yaku = bool(no_yaku) if isinstance(no_yaku, bool) else False
        if not self.is_tenpai:
            self.tenpai_waits = []  # 双向一致（false ⇒ []）
            self.tenpai_no_yaku = False

    def _apply_game_end(self, frame: dict) -> None:
        # §5.10 / H5：game_end 不携带 seq，不参与 seq 比较。v2.0：game_end = 局终页（非终态），
        # `match_will_continue == true` 时等待下一局 game_start，`false` 时等待 match_end。
        self.result = frame
        self.result_at = time.monotonic()
        self.window = None
        self.pending = False
        self.pending_label = None
        self.selected_index = None
        self.riichi_arming = False
        self.drawn_index = None
        self.frozen = True
        self.screen = SCREEN_RESULT
        self.phase = "结算"
        self.clear_tenpai()      # F-TP-11 ②③：局终（含荒庄）一律清空
        win = frame.get("win")
        if isinstance(win, dict) and isinstance(win.get("seat"), int):
            self.win_seat = win["seat"]
        else:
            self.win_seat = None
        # v2.0：连庄次数由局终帧累计（HUD「连庄 N」与局终页标题共用；跨局保留）。
        renchan = frame.get("renchan_count")
        if isinstance(renchan, int) and renchan >= 0:
            self.renchan_count = renchan

    def _apply_match_end(self, frame: dict) -> None:
        # v2.0（§9.6）：match_end = 场终页（每场恰好 1 次），此后不再有对局帧。
        self.match_end = frame
        self.window = None
        self.pending = False
        self.pending_label = None
        self.selected_index = None
        self.riichi_arming = False
        self.drawn_index = None
        self.frozen = True
        self.screen = SCREEN_RESULT
        self.phase = "结算"
        self.clear_tenpai()      # F-TP-11 ⑦：场终清空
        log.info(
            "场终：hands_played=%s renchan=%s",
            frame.get("hands_played"), frame.get("renchan_count"),
        )

    # ------------------------------------------------------------ seq / 窗口推进

    def _note_seq(self, frame: dict) -> None:
        """§5.1/§10.3：比较**携带 seq 的帧**；出现更新的 seq 即视为旧窗口已结束。

        只认「严格更大」的 `seq`：乱序到达的陈旧帧（`seq` 小于已见最大值）不得关窗，
        否则 `state_sync` 与 `action_request` 的到达顺序不保证（H9）会误关窗口。
        """
        seq = frame.get("seq")
        if not isinstance(seq, int):
            return
        if self.last_seq is None or seq > self.last_seq:
            self.last_seq = seq
        self.seq = self.last_seq
        if self.window is not None and self.last_seq > self.window.seq:
            self.window = None
            self.clear_pending()

    def clear_pending(self) -> None:
        self.pending = False
        self.pending_label = None
        self.pending_index = None
        self.selected_index = None
        self.riichi_arming = False
        self.drawn_index = None

    def mark_pending(self, label: str, index: int | None = None) -> None:
        self.pending = True
        self.pending_label = label
        self.pending_index = index
        self.selected_index = None
        self.riichi_arming = False

    def hide_window(self) -> None:
        self.window = None
        self.clear_pending()
