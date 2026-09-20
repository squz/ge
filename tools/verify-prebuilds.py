#!/usr/bin/env python3
"""Reject committed prebuilt binary artefacts (Phase 1 LFS exit).

Prebuilt static libs are a **local cache** produced by `make prebuild` /
`tools/ensure-prebuilt.sh`. They must not be committed. This verifier
fails if any tracked path matches the binary globs that used to go
through Git LFS.

cook.json / manifest.json under prebuilt/ may remain as plain text; they
are not checked here for freshness (local cook + verify-cook.py handle
that at link time).

Used by:
- `scripts/hooks/pre-commit` — reject staging binaries before commit.
- `.github/workflows/verify-prebuilds.yml` — CI tip-tree gate.

Copyright 2026 Marcelo Cantos
SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(
    os.environ.get("GE_REPO_ROOT") or Path(__file__).resolve().parent.parent
).resolve()

# Paths that must never be tracked after Phase 1 stop-publish.
FORBIDDEN_SUFFIXES = (
    ".a",
    ".so",
    ".dylib",
    ".o",
    ".lib",
    ".dll",
    ".exe",
    ".jar",
)


def tracked_files() -> list[str]:
    out = subprocess.check_output(
        ["git", "-C", str(REPO_ROOT), "ls-files", "-z"],
        text=False,
    )
    if not out:
        return []
    return [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]


def is_forbidden(path: str) -> bool:
    # Keep gradle-wrapper.jar if present (plain blob / regenerated locally).
    if path.endswith("gradle-wrapper.jar"):
        return False
    name = path.rsplit("/", 1)[-1]
    if name == "shaderc" and path.startswith("bin/"):
        return True
    lower = path.lower()
    if lower.endswith(FORBIDDEN_SUFFIXES):
        # Focus on prebuilt/vendor cook outputs + stray root libs; also
        # catch any remaining LFS-era jars (e.g. formal/tla2tools.jar).
        if (
            path.startswith("prebuilt/")
            or path.startswith("vendor/")
            or name == "libge.a"
            or lower.endswith(".jar")
            or path.startswith("bin/")
        ):
            return True
    return False


def main() -> int:
    bad = sorted(p for p in tracked_files() if is_forbidden(p))
    if bad:
        print("ERROR: committed prebuilt/binary artefacts found (Phase 1 LFS exit).", file=sys.stderr)
        print("", file=sys.stderr)
        for p in bad[:80]:
            print(f"  {p}", file=sys.stderr)
        if len(bad) > 80:
            print(f"  ... and {len(bad) - 80} more", file=sys.stderr)
        print("", file=sys.stderr)
        print("Prebuilts are local-only. Cook with `make prebuild`; do not git add archives.", file=sys.stderr)
        print("See docs/vendor-prebuilds.md.", file=sys.stderr)
        return 1

    print("prebuilts ok: no committed prebuilt/**/*.a (or related binary patterns)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
