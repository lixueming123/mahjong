"""Python view of the frozen wire enums (PROTOCOL v2.0).

The single source of truth is ``enums.json`` next to this file: Python reads it at
import time, so this side can never drift. C++ (``server/protocol_enums.h``) and JS
(``web/js/protocol_enums.js``) keep checked-in typed copies, guarded by
``tests/protocol_enums_consistency_test.py``.

Import from anywhere under ``game/`` with::

    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from protocol.enums import FRAME_TYPE, ACTION, ERROR_CODE
"""

from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace

_DEFINITION = json.loads((Path(__file__).with_name("enums.json")).read_text(encoding="utf-8"))

VERSION = _DEFINITION["version"]
DOMAINS = _DEFINITION["domains"]


def domains() -> dict[str, dict[str, int]]:
    """Return a fresh copy of the canonical name -> value tables."""
    return {domain: dict(values) for domain, values in DOMAINS.items()}


def _constants(domain: str) -> SimpleNamespace:
    # Upper-case attribute names (FRAME_TYPE.LOGIN_OK) are used by both clients.
    return SimpleNamespace(**{name.upper(): value for name, value in DOMAINS[domain].items()})


FRAME_TYPE = _constants("frame_type")
ACTION = _constants("action")
WINDOW = _constants("window")
MODE = _constants("mode")
LENGTH = _constants("length")
MELD_TYPE = _constants("meld_type")
REASON = _constants("reason")
ERROR_CODE = _constants("error_code")

MAXS = {domain: max(values.values()) for domain, values in DOMAINS.items()}


def name_of(domain: str, value: int) -> str:
    """Reverse lookup for logs and diagnostics; unknown values map to ``INVALID``."""
    for name, number in DOMAINS[domain].items():
        if number == value:
            return name
    return "INVALID"


def value_of(domain: str, name: str) -> int:
    """Forward lookup for DB/UI boundary translation; unknown names map to 0."""
    return DOMAINS[domain].get(name, 0)


__all__ = [
    "VERSION",
    "DOMAINS",
    "MAXS",
    "FRAME_TYPE",
    "ACTION",
    "WINDOW",
    "MODE",
    "LENGTH",
    "MELD_TYPE",
    "REASON",
    "ERROR_CODE",
    "domains",
    "name_of",
    "value_of",
]
