#!/usr/bin/env python3
"""Verify prebuilt/<platform>/cook.json matches every archive on disk.

Hard co-cook gate: every .a must match the SHA-256 recorded when
tools/prebuild.sh last cooked the *whole* tree. Prevents linking a
fresh libge against a stale libliteparser (2026-07 Android sqldeep
SIGSEGV). Used by:

  - tools/ensure-prebuilt.sh (before declaring a tree "fresh")
  - cmake/android-arm64.cmake (configure/link time)
  - iOS Xcode "Verify prebuilt cook" script phase (every build)

Usage:
  tools/verify-cook.py <prebuilt-dir>
  tools/verify-cook.py prebuilt/ios-arm64
  tools/verify-cook.py prebuilt/android-arm64-debug

Exit 0 on match. Exit 1 with a diagnostic on any mismatch/missing file.

Copyright 2026 Marcelo Cantos
SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

# Stems without lib/ .a — keep in sync with cmake/android-arm64.cmake
# and tools/prebuild.sh's archive set.
REQUIRED_STEMS = (
    "ge",
    "box2d",
    "lunasvg_ge",
    "plutovg_ge",
    "sqlite3_ge",
    "lz4_ge",
    "liteparser",
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_cook(prebuilt_dir: Path) -> list[str]:
    """Return a list of error strings (empty = ok)."""
    errors: list[str] = []
    root = prebuilt_dir.resolve()
    if not root.is_dir():
        return [f"prebuilt dir not found: {root}"]

    cook_path = root / "cook.json"
    if not cook_path.is_file():
        return [
            f"missing {cook_path}",
            "Prebuilts must be produced by tools/prebuild.sh (writes cook.json).",
            f"Run: tools/prebuild.sh <platform>  # or --debug for *-debug trees",
        ]

    try:
        cook = json.loads(cook_path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        return [f"cannot parse {cook_path}: {e}"]

    archives = cook.get("archives")
    if not isinstance(archives, dict) or not archives:
        return [f"{cook_path}: missing or empty 'archives' map"]

    for stem in REQUIRED_STEMS:
        a_path = root / f"lib{stem}.a"
        if not a_path.is_file():
            errors.append(f"missing {a_path}")
            continue
        expected = archives.get(stem)
        if not expected:
            errors.append(f"cook.json has no entry for {stem!r} ({cook_path})")
            continue
        actual = sha256_file(a_path)
        if actual != expected:
            errors.append(
                f"lib{stem}.a does not match cook.json\n"
                f"  archive: {a_path}\n"
                f"  cook:    {cook_path}\n"
                f"  disk sha256: {actual}\n"
                f"  cook sha256: {expected}\n"
                f"Archives were mixed from different cooks."
            )

    # Extra .a on disk not in cook (or cook entry with no file) is also
    # a co-cook violation — refuse to silently ignore orphans.
    on_disk = {p.stem.removeprefix("lib") for p in root.glob("*.a")}
    for stem in sorted(on_disk - set(archives)):
        errors.append(f"lib{stem}.a on disk but not in cook.json ({cook_path})")
    for stem in sorted(set(archives) - on_disk):
        errors.append(f"cook.json entry {stem!r} has no lib{stem}.a under {root}")

    return errors


def main(argv: list[str]) -> int:
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        print(
            "usage: verify-cook.py <prebuilt-dir>\n"
            "  e.g. tools/verify-cook.py prebuilt/ios-arm64",
            file=sys.stderr,
        )
        return 2
    root = Path(argv[1])
    errors = verify_cook(root)
    if not errors:
        print(f"ge: cook verified ({root})")
        return 0
    print(f"error: prebuilt cook check failed for {root.resolve()}:", file=sys.stderr)
    for e in errors:
        print(f"  {e}", file=sys.stderr)
    print(
        "  Run a full co-cook (partial cooks are disabled):\n"
        "    tools/prebuild.sh <platform>\n"
        "    tools/prebuild.sh --debug android-arm64   # debug tree",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
