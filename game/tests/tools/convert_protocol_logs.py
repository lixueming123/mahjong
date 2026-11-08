#!/usr/bin/env python3
"""PROTOCOL v2.0 golden 日志一次性机械转换脚本。

线协议控制字段由字符串硬切换为整数。golden 帧日志是历史录制证据，不能重录、不能删除，
因此就地按「字段上下文」递归转换。映射唯一来源是 game/protocol/enums.json（不手写第二份表）。

只转换控制字段的「值」，不碰对象键、不碰展示型字段（win_type / yaku[].id / tags / limit /
牌码）与元数据（ts / dir / event / binary / seed / sha256 / port / server_bin / server_mtime /
hand / error_count / reached_table / before_login_ok 等）。server_sha256 保留原值，不重录。

用法（可在任意 cwd 运行）:
    python3 game/tests/tools/convert_protocol_logs.py            # 默认：就地写回
    python3 game/tests/tools/convert_protocol_logs.py --check    # 只校验，不写文件

--check 校验三点：转换后再转换字节不变、所有控制字段均为 1..N 整数、无残留旧字符串。
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys

SCRIPT = pathlib.Path(__file__).resolve()
# SCRIPT: .../game/tests/tools/convert_protocol_logs.py
# parents[0]=tools  [1]=tests  [2]=game  [3]=仓库根
ENUMS_PATH = SCRIPT.parents[2] / "protocol" / "enums.json"
ARTIFACTS = SCRIPT.parents[1] / "artifacts"

EXPECTED_DOMAINS = {
    "frame_type", "action", "window", "mode",
    "length", "meld_type", "reason", "error_code",
}

# 字段上下文：控制字段「键」-> 所属域。type 特殊：按是否位于 melds 内二选一，见 walk()。
KEY_DOMAIN = {
    "action": "action",
    "window": "window",
    "mode": "mode",
    "length": "length",
    "reason": "reason",
    "code": "error_code",
}


class ConvertError(Exception):
    """转换期任何异常都 fail loud，绝不静默透传。"""


class Converter:
    def __init__(self, enums: dict):
        domains = enums.get("domains")
        if not isinstance(domains, dict):
            raise ConvertError("enums.json 缺少 domains 对象")
        missing = EXPECTED_DOMAINS - set(domains)
        extra = set(domains) - EXPECTED_DOMAINS
        if missing or extra:
            raise ConvertError(f"enums.json 域名集合不符，缺={sorted(missing)} 多={sorted(extra)}")

        # 每个域: 旧线字符串 -> 整数。INVALID(0) 不是真实线值，排除在表外。
        self.tables: dict[str, dict[str, int]] = {}
        self.maxv: dict[str, int] = {}
        for dom, mapping in domains.items():
            if not isinstance(mapping, dict):
                raise ConvertError(f"域 {dom} 不是对象")
            table = {}
            for name, value in mapping.items():
                if name == "INVALID":
                    continue
                if isinstance(value, bool) or not isinstance(value, int) or value < 1:
                    raise ConvertError(f"域 {dom} 的值 {name}={value!r} 非法（应为 >=1 整数）")
                table[name] = value
            if not table:
                raise ConvertError(f"域 {dom} 无真实线值")
            # 连续性自检 1..N，与 protocol_enums_consistency_test.py 的约束一致
            values = sorted(table.values())
            if values != list(range(1, len(values) + 1)):
                raise ConvertError(f"域 {dom} 的线值不是连续 1..N: {values}")
            self.tables[dom] = table
            self.maxv[dom] = values[-1]

        self.stats: collections.Counter = collections.Counter()

    def cv(self, value, domain: str, where: str):
        """单个控制值：字符串->整数（计数），整数原样（幂等），其它/未知 fail loud。"""
        if isinstance(value, bool):
            raise ConvertError(f"{where}: 控制字段 {domain} 出现 bool {value!r}")
        if isinstance(value, int):
            return value
        if isinstance(value, str):
            table = self.tables[domain]
            if value not in table:
                raise ConvertError(
                    f"{where}: 未知的 {domain} 字符串 {value!r}；已知值={sorted(table)}")
            self.stats[domain] += 1
            return table[value]
        raise ConvertError(
            f"{where}: 控制字段 {domain} 的值类型异常 {value!r} ({type(value).__name__})")

    def validate_int(self, value, domain: str, where: str):
        """--check 用：控制值必须是 1..N 的整数，字符串即视为残留旧值。"""
        if isinstance(value, str):
            raise ConvertError(f"{where}: 残留旧字符串 {value!r}（控制字段 {domain} 未转换）")
        if isinstance(value, bool) or not isinstance(value, int):
            raise ConvertError(f"{where}: 控制字段 {domain} 非整数 {value!r}")
        if not (1 <= value <= self.maxv[domain]):
            raise ConvertError(
                f"{where}: 控制字段 {domain} 越界 {value!r}（应为 1..{self.maxv[domain]}）")

    def walk(self, node, type_domain: str, where: str, validate: bool = False):
        """按字段上下文递归。

        type_domain 决定当前层遇到的 `type` 键用哪个域：
          - 默认 frame_type（顶层 frame.type / outbound_frames[].type / 裸帧 .json 等）
          - 进入任意 `melds` 列表后切到 meld_type（frame.melds[].type / seats[].melds[].type）
        其余控制键由 KEY_DOMAIN 固定；recent[] / classic.recent[] / riichi.recent[] 里的
        reason / mode 因键名命中而自动转换，无需特判。
        对象键、非控制键的值一律原样遍历，绝不改动。
        """
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "type":
                    domain = type_domain
                else:
                    domain = KEY_DOMAIN.get(key)
                child_where = f"{where}.{key}"

                if domain is not None:
                    if validate:
                        self.validate_int(value, domain, child_where)
                    else:
                        node[key] = self.cv(value, domain, child_where)
                elif key == "inbound_frames" and isinstance(value, list):
                    # client_zero_error_handshake.json: 裸字符串列表，按 frame_type 转
                    if validate:
                        for i, item in enumerate(value):
                            self.validate_int(item, "frame_type", f"{child_where}[{i}]")
                    else:
                        node[key] = [
                            self.cv(item, "frame_type", f"{child_where}[{i}]")
                            for i, item in enumerate(value)
                        ]
                else:
                    child_type_domain = "meld_type" if key == "melds" else "frame_type"
                    self.walk(value, child_type_domain, child_where, validate)
        elif isinstance(node, list):
            for i, item in enumerate(node):
                self.walk(item, type_domain, f"{where}[{i}]", validate)
        # str / int / float / bool / None: 叶子，不动


def is_candidate(path: pathlib.Path) -> bool:
    """候选 = .json 或名字含 .jsonl（覆盖 .jsonl.0/.run1/.run2），排除 .log 文本日志。"""
    name = path.name
    if name.endswith(".log"):
        return False
    return name.endswith(".json") or ".jsonl" in name


def list_candidates() -> list[pathlib.Path]:
    if not ARTIFACTS.is_dir():
        raise ConvertError(f"artifacts 目录不存在: {ARTIFACTS}")
    return sorted(p for p in ARTIFACTS.rglob("*") if p.is_file() and is_candidate(p))


def render_json(raw: str, data) -> str:
    """.json 序列化：2 空格缩进 + 保留原有尾换行。"""
    body = json.dumps(data, ensure_ascii=False, indent=2)
    if raw.endswith("\n"):
        body += "\n"
    return body


def render_jsonl(parts: list[str], objs: list) -> str:
    """.jsonl 序列化：逐行紧凑 dumps，空行/尾换行原样保留。"""
    out = []
    for part, obj in zip(parts, objs):
        if obj is None:
            out.append(part)
        else:
            out.append(json.dumps(obj, ensure_ascii=False))
    return "\n".join(out)


def parse_document(path: pathlib.Path, raw: str):
    """返回 (is_json, parts, objs)。
    .json -> parts=None, objs=[整棵对象]
    .jsonl* -> parts=按 '\\n' 切分, objs[i]=解析结果或 None(空行)
    """
    if path.name.endswith(".json"):
        return True, None, [json.loads(raw)]
    parts = raw.split("\n")
    objs = [json.loads(p) if p.strip() else None for p in parts]
    return False, parts, objs


def render_document(is_json: bool, raw: str, parts, objs) -> str:
    if is_json:
        return render_json(raw, objs[0])
    return render_jsonl(parts, objs)


def assert_format_roundtrip(path: pathlib.Path, raw: str, is_json: bool, parts, objs):
    """安全护栏：无转换时序列化必须与原文逐字节一致，防止改动格式化而破坏证据。"""
    baseline = render_document(is_json, raw, parts, objs)
    if baseline != raw:
        raise ConvertError(
            f"{path}: 序列化无法与原文逐字节一致，拒绝改写（格式不受支持）")


def convert_document(conv: Converter, objs, validate: bool, tag: str):
    for i, obj in enumerate(objs):
        if obj is not None:
            conv.walk(obj, "frame_type", tag, validate=validate)


def process(path: pathlib.Path, check: bool, conv: Converter):
    """处理单文件（纯内存，不写盘）。返回 (lines, new_text_or_none)。

    new_text 非 None 表示需要写回的内容；由 main() 在「所有文件都转换成功」后统一写盘，
    保证任一文件 fail loud 时不会留下半转换状态。check 模式恒返回 None。
    """
    raw = path.read_text(encoding="utf-8")
    is_json, parts, objs = parse_document(path, raw)
    assert_format_roundtrip(path, raw, is_json, parts, objs)

    lines = sum(1 for o in objs if o is not None)

    # 1) 在解析出的对象上做一次转换（check 模式下应为纯 no-op）
    convert_document(conv, objs, validate=False, tag=path.name)
    rendered = render_document(is_json, raw, parts, objs)

    if check:
        # 2) 转换后再转换，字节不变（幂等）
        _, parts2, objs2 = parse_document(path, rendered)
        convert_document(conv, objs2, validate=False, tag=path.name)
        rendered2 = render_document(is_json, rendered, parts2, objs2)
        if rendered2 != rendered:
            raise ConvertError(f"{path}: 转换后再转换字节发生变化（非幂等）")

        # 3) 文件必须已完全转换：任何残留旧字符串都会让 rendered != raw
        if rendered != raw:
            raise ConvertError(
                f"{path}: 仍存在可转换的旧字符串（未完全转换 / 零残留检查失败）")

        # 4) 从原文重新解析，逐个控制字段确认「均为 1..N 整数、无残留旧字符串」
        _, _, fresh = parse_document(path, raw)
        convert_document(conv, fresh, validate=True, tag=path.name)

        if conv.stats:
            raise ConvertError(
                f"{path}: --check 中发生了 {sum(conv.stats.values())} 次转换，说明有残留")
        return lines, None

    # 默认模式：有变化则返回新内容，交给 main() 统一写回
    if rendered != raw:
        return lines, rendered
    return lines, None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="只校验幂等 + 零残留 + 1..N 整数，不写文件")
    args = parser.parse_args()

    try:
        if not ENUMS_PATH.is_file():
            raise ConvertError(f"找不到 enums.json: {ENUMS_PATH}")
        enums = json.loads(ENUMS_PATH.read_text(encoding="utf-8"))
        paths = list_candidates()
        if not paths:
            raise ConvertError(f"artifacts 下没有待转换文件: {ARTIFACTS}")

        # 关键：先在内存中转换全部文件，任一失败都不写盘，避免半转换状态。
        results = []
        for path in paths:
            conv = Converter(enums)          # 每文件独立 Converter，便于 --check 校验 0 转换
            lines, new_text = process(path, args.check, conv)
            results.append((path, lines, conv.stats, new_text))

        # 全部通过后，才统一写盘（--check 模式 new_text 恒为 None，天然不写）
        if not args.check:
            for path, _, _, new_text in results:
                if new_text is not None:
                    path.write_text(new_text, encoding="utf-8")

    except ConvertError as exc:
        print(f"[convert] 失败（未写入任何文件）: {exc}", file=sys.stderr)
        return 1
    except (json.JSONDecodeError, OSError) as exc:
        print(f"[convert] 失败（未写入任何文件）: {exc}", file=sys.stderr)
        return 1

    # 汇总统计
    total_files = len(results)
    total_lines = sum(r[1] for r in results)
    total_conv = collections.Counter()
    for _, _, stats, _ in results:
        total_conv.update(stats)
    written_files = sum(1 for _, _, _, w in results if w is not None)

    mode = "CHECK" if args.check else "WRITE"
    print(f"[convert] 模式={mode} 枚举真值={ENUMS_PATH}")
    print(f"[convert] 文件数={total_files} 数据行/对象数={total_lines} "
          f"写回文件数={written_files}")
    print("[convert] 各域转换次数:")
    for dom in sorted(EXPECTED_DOMAINS):
        print(f"    {dom:<12} {total_conv.get(dom, 0)}")
    print(f"    {'TOTAL':<12} {sum(total_conv.values())}")

    if args.check:
        print("[convert] --check 通过：幂等 / 零残留 / 控制字段均为 1..N 整数")
    return 0


if __name__ == "__main__":
    sys.exit(main())
