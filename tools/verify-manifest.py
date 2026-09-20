#!/usr/bin/env python3
"""Verify prebuilt/<platform>/manifest.json still matches the working tree.

This is the **staleness oracle**: it answers "were these archives cooked
from the sources that are on disk right now?" It does NOT care whether
archives are committed — that is `tools/verify-prebuilds.py`'s job, and
after the Phase 1 LFS exit the two questions are genuinely separate.

  tools/verify-manifest.py           # every prebuilt/*/manifest.json
  tools/verify-manifest.py --platform ios-arm64
  tools/verify-manifest.py --platform android-arm64-debug

Exit 0 when the recorded script hashes, submodule SHAs and input hashes
all match. Exit 1 with a per-file diagnostic otherwise, pointing at
`make prebuild`.

Split out of the pre-Phase-1 `verify-prebuilds.py`, which conflated this
check with "are binaries tracked in git?". Phase 1 rewrote that file for
the tracked-binaries question and dropped `--platform`, which silently
turned `ensure-prebuilt.sh`'s freshness gate into a no-op: it kept
passing `--platform` to a script that no longer parses argv, so every
tree looked fresh. Archive integrity (cook.json <-> .a on disk) remains
`tools/verify-cook.py`.

Copyright 2026 Marcelo Cantos
SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(
    os.environ.get("GE_REPO_ROOT") or Path(__file__).resolve().parent.parent
).resolve()
SUPPORTED_MANIFEST_VERSIONS = {1, 2}

# Android NDK ABI floor (T100). Keep in sync with GE_ANDROID_NDK_MAJOR in
# tools/prebuild.sh. A newer cook NDK bakes in libc++ exception-ABI
# symbols the consumer's older runtime cannot resolve at link time.
ANDROID_NDK_MAJOR_PIN = "27"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load_submodule_status() -> dict[str, str]:
    """{submodule_path: committed_sha}. Reads the superproject only —
    submodules need not be initialised."""
    out = subprocess.check_output(
        ["git", "submodule", "status"], cwd=REPO_ROOT, text=True
    )
    shas: dict[str, str] = {}
    for line in out.splitlines():
        m = re.match(r"^[ \-+U]([0-9a-f]+)\s+(\S+)", line)
        if m:
            shas[m.group(2)] = m.group(1)
    return shas


def verify_manifest(manifest_path: Path) -> list[str]:
    errors: list[str] = []
    with open(manifest_path) as f:
        manifest = json.load(f)

    if manifest.get("version") not in SUPPORTED_MANIFEST_VERSIONS:
        return [
            f"unsupported manifest version {manifest.get('version')}"
            f" (verifier accepts {sorted(SUPPORTED_MANIFEST_VERSIONS)})"
        ]

    # 1) Controlling shell + python scripts.
    for rel, expected in manifest.get("scripts", {}).items():
        p = REPO_ROOT / rel
        if not p.exists():
            errors.append(f"missing script: {rel}")
        elif sha256_file(p) != expected:
            errors.append(f"script changed: {rel}")

    # 2) Submodule SHAs, two-way (T78): the manifest must reference every
    # submodule in the tree, and every submodule it references must still
    # exist. Without the converse, swapping a submodule and forgetting to
    # refresh the manifest would pass silently.
    actual_shas = load_submodule_status()
    manifest_shas = manifest.get("submodule_shas", {})
    for rel, expected in manifest_shas.items():
        actual = actual_shas.get(rel)
        if actual is None:
            errors.append(f"submodule not configured: {rel}")
        elif actual != expected:
            errors.append(
                f"submodule SHA changed: {rel}\n"
                f"    expected {expected[:12]}\n"
                f"    actual   {actual[:12]}"
            )
    for rel in actual_shas:
        if rel not in manifest_shas:
            errors.append(f"submodule not in manifest: {rel}")

    # 3) Source inputs.
    for rel, expected in manifest.get("inputs", {}).items():
        p = REPO_ROOT / rel
        if not p.exists():
            errors.append(f"missing input: {rel}")
        elif sha256_file(p) != expected:
            errors.append(f"input changed: {rel}")

    # 4) Android NDK ABI pin (T100).
    if manifest_path.parent.name.startswith("android-arm64"):
        ndk_path = manifest.get("toolchain", {}).get("ndk_path", "")
        ndk_major = Path(ndk_path).name.split(".", 1)[0] if ndk_path else ""
        if ndk_major != ANDROID_NDK_MAJOR_PIN:
            errors.append(
                f"android prebuilt built with NDK r{ndk_major or '?'}, "
                f"expected r{ANDROID_NDK_MAJOR_PIN} (T100). Rebuild with: "
                f"GE_ANDROID_NDK_MAJOR={ANDROID_NDK_MAJOR_PIN} "
                f"tools/prebuild.sh android-arm64"
            )

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "--platform",
        default=None,
        help="Scope to one platform dir (e.g. ios-arm64, android-arm64-debug). "
             "Default: every prebuilt/*/manifest.json.",
    )
    args = parser.parse_args()

    if args.platform:
        manifests = [REPO_ROOT / f"prebuilt/{args.platform}/manifest.json"]
    else:
        manifests = sorted((REPO_ROOT / "prebuilt").glob("*/manifest.json"))

    if not manifests:
        print(
            "error: no prebuilt/*/manifest.json found. Run `make prebuild`.",
            file=sys.stderr,
        )
        return 1

    all_errors: list[tuple[str, str]] = []
    checked: list[str] = []
    for manifest_path in manifests:
        platform = manifest_path.parent.name
        if not manifest_path.exists():
            all_errors.append((platform, f"manifest missing: {manifest_path}"))
            continue
        checked.append(platform)
        for e in verify_manifest(manifest_path):
            all_errors.append((platform, e))

    if all_errors:
        print("ERROR: prebuilt manifest is stale vs the working tree.", file=sys.stderr)
        print("", file=sys.stderr)
        for platform, e in all_errors:
            print(f"  [{platform}] {e}", file=sys.stderr)
        print("", file=sys.stderr)
        print("Re-cook: make prebuild   (or tools/prebuild.sh <platform>)", file=sys.stderr)
        return 1

    print(f"manifest ok: {', '.join(checked)} match the working tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
