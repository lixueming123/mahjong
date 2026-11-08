#!/usr/bin/env python3
"""Guard the frozen PROTOCOL v2.0 wire enums across C++ / Python / JS / docs.

``game/protocol/enums.json`` is the single source of truth. Python reads it directly,
while the C++ header, the JS module and the markdown tables keep checked-in typed copies
(a browser cannot read a local JSON file, and C++ has no build-time codegen here). This
test makes sure none of those copies can drift: it compares names *and* numeric values.

Run directly or via ctest (``protocol_enums_consistency``). Pure standard library, except
that node MUST be available to cover the web side.
"""

from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]  # game/
ENUMS_JSON = ROOT / "protocol" / "enums.json"
CPP_HEADER = ROOT / "server" / "protocol_enums.h"
JS_MODULE = ROOT / "web" / "js" / "protocol_enums.js"
PY_MODULE = ROOT / "protocol" / "enums.py"
PROTOCOL_MD = ROOT / "docs" / "PROTOCOL.md"
SERVE_PY = ROOT / "web" / "serve.py"

# domain -> (real value count, C++ enum class name, C++ name-table variable)
DOMAIN_LAYOUT = {
    "frame_type": (27, "FrameType", "kFrameTypeNames"),
    "action": (8, "Action", "kActionNames"),
    "window": (3, "Window", "kWindowNames"),
    "mode": (2, "WireMode", "kModeNames"),
    "length": (2, "WireLength", "kLengthNames"),
    "meld_type": (5, "WireMeldType", "kMeldTypeNames"),
    "reason": (3, "RoundEndReason", "kReasonNames"),
    "error_code": (14, "ErrorCode", "kErrorCodeNames"),
    "yaku": (47, "YakuId", "kYakuNames"),
    "limit": (6, "WireLimit", "kLimitNames"),
}

errors: list[str] = []


def fail(message: str) -> None:
    errors.append(message)


def check(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def canonical() -> dict[str, dict[str, int]]:
    definition = json.loads(ENUMS_JSON.read_text(encoding="utf-8"))
    check(definition.get("version") == "2.1", "enums.json version must be 2.1")
    domains = definition.get("domains", {})
    check(
        set(domains) == set(DOMAIN_LAYOUT),
        f"domain set mismatch: {sorted(domains)} vs {sorted(DOMAIN_LAYOUT)}",
    )
    for domain, (expected_count, _, _) in DOMAIN_LAYOUT.items():
        values = domains.get(domain)
        if not isinstance(values, dict):
            fail(f"{domain}: missing domain table")
            continue
        check(values.get("INVALID") == 0, f"{domain}: INVALID must be 0")
        check(
            len(values) == expected_count + 1,
            f"{domain}: expected {expected_count + 1} entries, got {len(values)}",
        )
        real = {name: value for name, value in values.items() if name != "INVALID"}
        check(
            sorted(real.values()) == list(range(1, expected_count + 1)),
            f"{domain}: real values must be contiguous 1..{expected_count}, got {sorted(real.values())}",
        )
        check(
            len(set(real.values())) == len(real),
            f"{domain}: duplicate numeric values",
        )
    return domains


def ordered_names(values: dict[str, int]) -> list[str]:
    return [name for name, _ in sorted(values.items(), key=lambda item: item[1])]


def camel_to_snake(member: str) -> str:
    """kLoginOk -> login_ok; kInvalidName -> invalid_name."""
    body = member[1:] if member.startswith("k") else member
    out: list[str] = []
    for index, char in enumerate(body):
        if char.isupper() and index > 0:
            out.append("_")
        elif (
            char.isdigit()
            and index > 0
            and not body[index - 1].isdigit()
            and body[index - 1] != "_"
        ):
            out.append("_")
        out.append(char.lower())
    return "".join(out)


def check_cpp(domains: dict[str, dict[str, int]]) -> None:
    text = CPP_HEADER.read_text(encoding="utf-8")
    for domain, (_, enum_name, names_var) in DOMAIN_LAYOUT.items():
        block = re.search(
            rf"enum class {enum_name}\s*:\s*uint8_t\s*\{{(.*?)\}};", text, re.DOTALL
        )
        if block is None:
            fail(f"C++: enum class {enum_name} not found")
            continue
        members = re.findall(r"k(\w+)\s*=\s*(\d+)\s*,", block.group(1))
        check(
            len(members) == len(domains[domain]),
            f"C++ {enum_name}: {len(members)} members, expected {len(domains[domain])}",
        )
        for member, raw_value in members:
            value = int(raw_value)
            expected = next(
                (name for name, number in domains[domain].items() if number == value), None
            )
            if expected is None:
                fail(f"C++ {enum_name}: unexpected value {value} ({member})")
                continue
            check(
                camel_to_snake("k" + member).lower() == expected.lower(),
                f"C++ {enum_name}: k{member} (={value}) should be named {expected}",
            )
        names_block = re.search(
            rf"{names_var}\s*=\s*\{{(.*?)\}};", text, re.DOTALL
        )
        if names_block is None:
            fail(f"C++: name table {names_var} not found")
            continue
        names = re.findall(r'"([^"]+)"', names_block.group(1))
        check(
            names == ordered_names(domains[domain]),
            f"C++ {names_var}: name order mismatch\n  got      {names}\n  expected {ordered_names(domains[domain])}",
        )


def check_python(domains: dict[str, dict[str, int]]) -> None:
    spec = importlib.util.spec_from_file_location("protocol_enums_under_test", PY_MODULE)
    if spec is None or spec.loader is None:
        fail("Python: cannot load protocol/enums.py")
        return
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    check(module.domains() == domains, "Python: domains() differs from enums.json")
    check(module.VERSION == "2.1", "Python: VERSION must be 2.1")
    check(
        module.FRAME_TYPE.DISCARD == domains["frame_type"]["discard"],
        "Python: FRAME_TYPE.DISCARD mismatch",
    )
    check(
        module.ERROR_CODE.BAD_REQUEST == domains["error_code"]["BAD_REQUEST"],
        "Python: ERROR_CODE.BAD_REQUEST mismatch",
    )
    check(
        module.value_of("reason", "win_tsumo") == domains["reason"]["win_tsumo"],
        "Python: value_of(reason, win_tsumo) mismatch",
    )
    check(
        module.YAKU.MENZEN == domains["yaku"]["menzen"],
        "Python: YAKU.MENZEN mismatch",
    )
    check(
        module.YAKU.WIN_TSUMO == domains["yaku"]["win_tsumo"],
        "Python: YAKU.WIN_TSUMO mismatch",
    )
    check(
        module.LIMIT.MANGAN == domains["limit"]["mangan"],
        "Python: LIMIT.MANGAN mismatch",
    )
    check(
        module.value_of("yaku", "seven_pairs") == domains["yaku"]["seven_pairs"],
        "Python: value_of(yaku, seven_pairs) mismatch",
    )


def check_js(domains: dict[str, dict[str, int]]) -> None:
    if shutil.which("node") is None:
        fail("JS: node is not available; the web enum table cannot be verified")
        return
    result = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            f"const m = await import('{JS_MODULE.as_uri()}');"
            "console.log(JSON.stringify(m.DOMAINS));",
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        fail(f"JS: failed to import {JS_MODULE}: {result.stderr.strip()}")
        return
    try:
        loaded = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError) as exc:
        fail(f"JS: cannot parse DOMAINS output: {exc}")
        return
    check(loaded == domains, "JS: DOMAINS differs from enums.json")


def check_docs(domains: dict[str, dict[str, int]]) -> None:
    text = PROTOCOL_MD.read_text(encoding="utf-8")
    check("v2.1" in text, "docs: PROTOCOL.md is not marked v2.1")
    check(
        '"type"（string）' not in text and '"type"`（string）' not in text,
        "docs: §1.3 still says type is a string",
    )
    for domain, (_, _, _) in DOMAIN_LAYOUT.items():
        block = re.search(
            rf"<!-- wire-enum: {domain} -->(.*?)<!-- /wire-enum -->", text, re.DOTALL
        )
        if block is None:
            fail(f"docs: missing <!-- wire-enum: {domain} --> table")
            continue
        rows = re.findall(r"^\|\s*(\d+)\s*\|\s*`([^`]+)`", block.group(1), re.MULTILINE)
        expected = [(number, name) for number, name in
                    sorted(((v, k) for k, v in domains[domain].items()))]
        check(
            [(int(number), name) for number, name in rows] == expected,
            f"docs: {domain} table mismatch\n  got      {rows}\n  expected {expected}",
        )


def check_serve() -> None:
    text = SERVE_PY.read_text(encoding="utf-8")
    check(
        '{"type":"error"' not in text and "'\"type\":\"error\"'" not in text,
        "serve.py still hardcodes a string-typed error frame",
    )
    check("FRAME_TYPE.ERROR" in text, "serve.py does not use FRAME_TYPE.ERROR")
    check("ERROR_CODE.INTERNAL" in text, "serve.py does not use ERROR_CODE.INTERNAL")


def main() -> int:
    domains = canonical()
    check_cpp(domains)
    check_python(domains)
    check_js(domains)
    check_docs(domains)
    check_serve()
    if errors:
        print(f"protocol enum consistency FAILED ({len(errors)} issue(s)):", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    total = sum(count for count, _, _ in DOMAIN_LAYOUT.values())
    print(f"protocol enum consistency OK: {len(DOMAIN_LAYOUT)} domains, {total} frozen wire values, "
          "C++/Python/JS/docs/serve.py agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
