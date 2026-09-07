#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""Agent-facing docs must not contradict the compile graph.

Two failure modes this catches, both observed on master@2f4b0a7:

1. `kProtocolVersion` was 9 in `include/ge/Protocol.h`, 7 in `STABILITY.md`
   and 6 in `AGENTS.md` — an agent that loads the docs first reads a wrong
   number before every protocol decision.
2. `AGENTS.md`/`STABILITY.md` documented `tools/player.cpp`, `tools/ios/`,
   `tools/android/`, the bgfx vendor tree and the `$(ge/PLAYER)` Make
   variable. None of them exist; the `player` target deliberately exits 1
   ("stream player lives in the spyder repo").

`include/ge/Protocol.h` and the Makefiles are the sources of truth; the
docs derive from them.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DOCS = ("AGENTS.md", "STABILITY.md", "agents-guide.md")

# Paths the docs used to advertise that no longer exist. A doc may only
# mention one of these to say it is gone (lines matching EXEMPT below).
RETIRED_PATHS = (
    "tools/player.cpp",
    "tools/player_capture_apple.mm",
    "tools/ios/",
    "tools/android/",
    "vendor/github.com/bkaradzic/",
)

# Make variable for the player binary; removed when the player moved to spyder.
RETIRED_MAKE_VAR = "ge/PLAYER"

# A mention is fine when the sentence is explicitly about the removal.
EXEMPT = re.compile(r"\b(removed|retired|no longer|does not exist|lives in the spyder)\b", re.I)


def protocol_version() -> int:
    header = (ROOT / "include/ge/Protocol.h").read_text()
    m = re.search(r"constexpr\s+uint16_t\s+kProtocolVersion\s*=\s*(\d+)\s*;", header)
    if not m:
        sys.exit("error: kProtocolVersion not found in include/ge/Protocol.h")
    return int(m.group(1))


def main() -> int:
    version = protocol_version()
    # Any `kProtocolVersion` followed by a number in a doc must state that number.
    stated = re.compile(r"kProtocolVersion`?\s*(?:=|\|)\s*(\d+)")
    failures: list[str] = []

    for name in DOCS:
        path = ROOT / name
        if not path.exists():
            failures.append(f"{name}: documented file is missing")
            continue
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            for m in stated.finditer(line):
                if int(m.group(1)) != version:
                    failures.append(
                        f"{name}:{lineno}: kProtocolVersion = {m.group(1)}, "
                        f"but include/ge/Protocol.h says {version}"
                    )
            if EXEMPT.search(line):
                continue
            for retired in RETIRED_PATHS:
                if retired in line:
                    failures.append(f"{name}:{lineno}: references retired path {retired}")
            if RETIRED_MAKE_VAR in line:
                failures.append(
                    f"{name}:{lineno}: references retired Make variable $({RETIRED_MAKE_VAR})"
                )

    # Guard against the retired paths quietly coming back without the docs
    # being the thing at fault.
    for retired in RETIRED_PATHS:
        if (ROOT / retired.rstrip("/")).exists():
            failures.append(
                f"{retired} exists again — update RETIRED_PATHS in {Path(__file__).name}"
            )

    if failures:
        print("✗ docs disagree with the compile graph:")
        for f in failures:
            print(f"    {f}")
        return 1
    print(f"✓ docs agree with the compile graph (kProtocolVersion = {version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
