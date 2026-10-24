"""结算播报语音的**实测时长表**（唯一副本，随代码走）。

为什么把数字写进代码，而不是运行时读 `assets/vo/vo_manifest.json`？

- **时间轴必须与「有没有 wav」解耦**（RULINGS §3 / 本批验收第 2 项）：`assets/vo/`
  整个目录改名后，播报的**每一帧**必须与有语音时完全一致，只是没声。若时长来自
  manifest，目录一改名时间轴就会跟着变 —— 这条验收项就不可能成立。
- 因此：**时长表 = 代码里的常量**，manifest 只作为**回归比对物**（`verify_manifest()`）。

真值来源：`docs/SETTLEMENT_BROADCAST_AUDIO.md` §8.3「资产验收表」（57 条逐条实测，
2026-10-23 生成，裁静音后）。重跑 TTS / 改文案后，用下面的命令重新对账::

    python3 -c "from client import vo_durations as v; print(v.verify_manifest())"

本模块是**纯数据 + 纯标准库**：不 import pygame、不碰 IO（除 `verify_manifest` 显式调用）。
"""

from __future__ import annotations

import json
import logging
import os

log = logging.getLogger("mahjong.client.vo_durations")

#: 57 条语音的实测时长（ms）。键 = 逻辑 id = `assets/vo/<id>.wav` 的主干。
#: 与 `client/assets/vo/vo_lines.tsv` 一一对应（id 集合必须相等，见 `verify_lines()`）。
VOICE_MS: dict[str, int] = {
    # ---- 开场：和牌方式（3）
    "vo_mode_tsumo": 488,
    "vo_mode_ron": 562,
    "vo_mode_draw": 573,
    # ---- 段 1：番种整句（16；箭刻 3 条）
    "vo_yaku_menzen": 1328,
    "vo_yaku_tanyao": 1165,
    "vo_yaku_pinfu": 1037,
    "vo_yaku_iipeikou": 1162,
    "vo_yaku_rinshan": 1531,
    "vo_yaku_yakuhai_1": 966,
    "vo_yaku_yakuhai_2": 1038,
    "vo_yaku_yakuhai_3": 1038,
    "vo_yaku_sanshoku_doujun": 1638,
    "vo_yaku_ittsuu": 1520,
    "vo_yaku_toitoi": 1290,
    "vo_yaku_sanankou": 1360,
    "vo_yaku_honitsu": 1255,
    "vo_yaku_shousangen": 1388,
    "vo_yaku_seven_pairs": 1306,
    "vo_yaku_pure_one_suit": 1236,
    # ---- 段 1 摘要（5，预留：逐条删句方案下无消费方，仍随库生产）
    "vo_yaku_sum_2": 771,
    "vo_yaku_sum_3": 780,
    "vo_yaku_sum_4": 750,
    "vo_yaku_sum_5": 684,
    "vo_yaku_sum_6": 735,
    # ---- 段 3：加成（2）
    "vo_tsumo_no_add": 1230,
    "vo_dealer_bonus": 1234,
    # ---- 段 2：合计番数（7；`vo_han_0` = 「无番」）
    "vo_han_0": 515,
    "vo_han_1": 950,
    "vo_han_2": 1037,
    "vo_han_3": 1043,
    "vo_han_4": 991,
    "vo_han_5": 968,
    "vo_han_6": 988,
    # ---- 段 4：封顶（1）
    "vo_cap": 734,
    # ---- 段 5：倍率（7）
    "vo_mult_1": 410,
    "vo_mult_2": 487,
    "vo_mult_4": 478,
    "vo_mult_8": 418,
    "vo_mult_16": 721,
    "vo_mult_32": 966,
    "vo_mult_64": 849,
    # ---- 段 6：支付·荣和（7）
    "vo_pay_ron_1": 1535,
    "vo_pay_ron_2": 1616,
    "vo_pay_ron_4": 1605,
    "vo_pay_ron_8": 1582,
    "vo_pay_ron_16": 1814,
    "vo_pay_ron_32": 2055,
    "vo_pay_ron_64": 1960,
    # ---- 段 6：支付·自摸（7）
    "vo_pay_tsumo_2": 1534,
    "vo_pay_tsumo_4": 1529,
    "vo_pay_tsumo_8": 1500,
    "vo_pay_tsumo_16": 1767,
    "vo_pay_tsumo_32": 2010,
    "vo_pay_tsumo_64": 1882,
    "vo_pay_tsumo_128": 2192,
    # ---- 段 6：荒庄（2）
    "vo_draw_no_score": 1149,
    "vo_draw_renchan": 1055,
}

#: 合法倍率（`vo_mult_*` 的 id 集合 = 协议 `multiplier` 的 8 值闭集去掉 128）
MULT_VALUES = (1, 2, 4, 8, 16, 32, 64)
#: 荣和支付的 8 值闭集 / 自摸支付的 8 值闭集（PROTOCOL §5.10）
PAY_RON_VALUES = (1, 2, 4, 8, 16, 32, 64)
PAY_TSUMO_VALUES = (2, 4, 8, 16, 32, 64, 128)


def duration_of(cue: str | None) -> int:
    """一条语音的时长（ms）。未知 id ⇒ 0（**不抛错**：缺句是合法降级，不是异常）。"""
    if not cue:
        return 0
    return VOICE_MS.get(cue, 0)


def voice_assets_dir() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "vo")


def existing_cues(cues_dir: str | None = None) -> set[str]:
    """磁盘上真实存在的 `<id>.wav` 主干集合（不存在 ⇒ 空集，不报错）。"""
    base = cues_dir or voice_assets_dir()
    found: set[str] = set()
    try:
        names = os.listdir(base)
    except OSError:
        return found
    for name in names:
        if name.endswith(".wav"):
            found.add(name[:-4])
    return found


def verify_manifest(cues_dir: str | None = None) -> dict:
    """把本表的数字与 `vo_manifest.json` 的实测值对账（回归用，不进主循环）。

    返回 `{"checked": n, "missing": [...], "drift": [(id, 表内, manifest), ...],
    "manifest": 路径或 None}`。manifest 不存在 ⇒ `checked=0`，不算失败。
    """
    base = cues_dir or voice_assets_dir()
    path = os.path.join(base, "vo_manifest.json")
    result: dict = {"checked": 0, "missing": [], "drift": [], "manifest": None}
    if not os.path.isfile(path):
        return result
    result["manifest"] = path
    try:
        with open(path, encoding="utf-8") as handle:
            payload = json.load(handle)
    except (OSError, ValueError) as exc:
        log.warning("vo_manifest.json 解析失败：%s", exc)
        return result
    entries = None
    if isinstance(payload, dict):
        # 真实 manifest 的形状是 `{"version":1, ..., "items": {id: {"ms": 487.9, ...}}}`
        for key in ("items", "entries", "lines"):
            if isinstance(payload.get(key), (dict, list)):
                entries = payload[key]
                break
        if entries is None:
            entries = payload
    if not isinstance(entries, (dict, list)):
        return result
    pairs: list[tuple[str, object]] = []
    if isinstance(entries, dict):
        for key, value in entries.items():
            ms = value.get("ms") if isinstance(value, dict) else value
            pairs.append((str(key), ms))
    else:
        for item in entries:
            if isinstance(item, dict) and isinstance(item.get("id"), str):
                pairs.append((item["id"], item.get("ms")))
    for cue, ms in pairs:
        if not isinstance(ms, (int, float)):
            continue
        result["checked"] += 1
        table = VOICE_MS.get(cue)
        if table is None:
            result["missing"].append(cue)
        elif abs(float(table) - float(ms)) > 0.51:
            # 表内取整毫秒；manifest 保留一位小数 ⇒ 容差 0.51 ms（否则 487.9 vs 488 会误判）
            result["drift"].append((cue, table, ms))
    return result


def verify_lines(cues_dir: str | None = None) -> list[str]:
    """`vo_lines.tsv` 的 id 集合与本表比对；返回问题列表（空 = 一致）。"""
    base = cues_dir or voice_assets_dir()
    path = os.path.join(base, "vo_lines.tsv")
    problems: list[str] = []
    try:
        with open(path, encoding="utf-8") as handle:
            ids = {line.split("\t", 1)[0].strip() for line in handle if line.strip()}
    except OSError:
        return problems          # 没有 tsv 不算问题（资产未落地是正常态）
    for cue in sorted(ids - set(VOICE_MS)):
        problems.append(f"tsv 有、时长表没有：{cue}")
    for cue in sorted(set(VOICE_MS) - ids):
        problems.append(f"时长表有、tsv 没有：{cue}")
    return problems
