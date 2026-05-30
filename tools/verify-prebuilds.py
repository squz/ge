#!/usr/bin/env python3
"""Verify prebuilt/<platform>/manifest.json files match the working tree.

By default checks every manifest under prebuilt/*/manifest.json. Pass
`--platform ios-arm64` (or `android-arm64`) to scope the check to one
platform.

Exit 0 if all input hashes, prebuilt .a hashes, script hashes, and
submodule SHAs match the manifest. Exit 1 with a per-file diagnostic
otherwise — pointing the developer at `make prebuild` (or a single
platform, e.g. `make prebuild-ios-arm64`) and/or `make ge/lift-headers`.

Used by:
- `scripts/hooks/pre-commit` — local fast check before push.
- `.github/workflows/verify-prebuilds.yml` — authoritative CI gate on
  PRs that touch source / vendor / scripts / submodule pointers /
  prebuilt artefacts.

Runs on Linux + macOS. No xcrun / clang needed — just hashes files.
On CI, runs against a checkout WITHOUT submodule init (cheaper) and
WITHOUT LFS materialised for the prebuilts/ tree (the .a hash comes
from the committed LFS pointer's payload, materialised via `git lfs
pull --include='prebuilt/**' --exclude=''` if a deep check is wanted;
by default the verifier checks the working-tree file as-is).

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


REPO_ROOT = Path(__file__).resolve().parent.parent
SUPPORTED_MANIFEST_VERSIONS = {1, 2}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def is_lfs_pointer(path: Path) -> bool:
    """A file that is still an LFS pointer (not smudged) starts with the
    distinctive `version https://git-lfs.github.com/spec/v1` marker and
    is small (~130 bytes). Detect so we can skip hashing prebuilt .a
    files when CI didn't pull LFS — that's expected for the cheap path."""
    try:
        if path.stat().st_size > 4096:
            return False
        with open(path, "rb") as f:
            head = f.read(64)
        return head.startswith(b"version https://git-lfs.github.com/spec/")
    except OSError:
        return False


def load_submodule_status() -> dict[str, str]:
    """Returns {submodule_path: committed_sha}. Reads `git submodule
    status` which only needs the superproject's git data — submodules
    don't need to be initialised."""
    out = subprocess.check_output(
        ["git", "submodule", "status"], cwd=REPO_ROOT, text=True
    )
    shas: dict[str, str] = {}
    for line in out.splitlines():
        m = re.match(r"^[ \-+U]([0-9a-f]+)\s+(\S+)", line)
        if m:
            shas[m.group(2)] = m.group(1)
    return shas


def verify_manifest(manifest_path: Path) -> tuple[list[str], int, dict[str, int]]:
    """Verify a single platform's manifest. Returns (errors, skipped_lfs,
    counts)."""
    errors: list[str] = []
    skipped_lfs = 0

    with open(manifest_path) as f:
        manifest = json.load(f)

    if manifest.get("version") not in SUPPORTED_MANIFEST_VERSIONS:
        errors.append(
            f"unsupported manifest version {manifest.get('version')}"
            f" (verifier accepts {sorted(SUPPORTED_MANIFEST_VERSIONS)})"
        )
        return errors, skipped_lfs, {}

    # 1) scripts — controlling shell + python scripts.
    for rel, expected in manifest.get("scripts", {}).items():
        p = REPO_ROOT / rel
        if not p.exists():
            errors.append(f"missing script: {rel}")
            continue
        actual = sha256_file(p)
        if actual != expected:
            errors.append(f"script changed: {rel}")

    # 2) submodule SHAs — from `git submodule status`, no init needed.
    actual_shas = load_submodule_status()
    for rel, expected in manifest.get("submodule_shas", {}).items():
        actual = actual_shas.get(rel)
        if actual is None:
            errors.append(f"submodule not configured: {rel}")
        elif actual != expected:
            errors.append(
                f"submodule SHA changed: {rel}\n"
                f"    expected {expected[:12]}\n"
                f"    actual   {actual[:12]}"
            )

    # 3) prebuilt .a files — skip if still LFS pointers (CI cheap path).
    for rel, expected in manifest.get("prebuilts", {}).items():
        p = REPO_ROOT / rel
        if not p.exists():
            errors.append(f"missing prebuilt: {rel}")
            continue
        if is_lfs_pointer(p):
            skipped_lfs += 1
            continue
        actual = sha256_file(p)
        if actual != expected:
            errors.append(f"prebuilt changed: {rel}")

    # 4) inputs.
    for rel, expected in manifest.get("inputs", {}).items():
        p = REPO_ROOT / rel
        if not p.exists():
            errors.append(f"missing input: {rel}")
            continue
        actual = sha256_file(p)
        if actual != expected:
            errors.append(f"input changed: {rel}")

    counts = {
        "scripts":   len(manifest.get("scripts", {})),
        "submods":   len(manifest.get("submodule_shas", {})),
        "prebuilts": len(manifest.get("prebuilts", {})),
        "inputs":    len(manifest.get("inputs", {})),
    }
    return errors, skipped_lfs, counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "--platform",
        default=None,
        help="Scope check to one platform directory (e.g. ios-arm64, "
             "android-arm64). Default: every prebuilt/*/manifest.json.",
    )
    args = parser.parse_args()

    if args.platform:
        manifests = [REPO_ROOT / f"prebuilt/{args.platform}/manifest.json"]
    else:
        manifests = sorted((REPO_ROOT / "prebuilt").glob("*/manifest.json"))

    if not manifests:
        print(
            "error: no prebuilt/*/manifest.json found. "
            "Run `make prebuild` to generate them.",
            file=sys.stderr,
        )
        return 1

    all_errors: list[tuple[str, str]] = []
    total_skipped_lfs = 0
    total_counts = {"scripts": 0, "submods": 0, "prebuilts": 0, "inputs": 0}
    platforms_checked: list[str] = []

    for manifest_path in manifests:
        platform = manifest_path.parent.name
        if not manifest_path.exists():
            all_errors.append((platform, f"manifest missing: {manifest_path.relative_to(REPO_ROOT)}"))
            continue
        errors, skipped_lfs, counts = verify_manifest(manifest_path)
        platforms_checked.append(platform)
        total_skipped_lfs += skipped_lfs
        for k, v in counts.items():
            total_counts[k] += v
        for e in errors:
            all_errors.append((platform, e))

    if all_errors:
        print("ERROR: prebuilt artefacts are stale.", file=sys.stderr)
        print("", file=sys.stderr)
        for platform, e in all_errors[:40]:
            print(f"  [{platform}] {e}", file=sys.stderr)
        if len(all_errors) > 40:
            print(f"  ... and {len(all_errors) - 40} more", file=sys.stderr)
        print("", file=sys.stderr)
        print("Fix on a Mac with iOS SDK and Android NDK:", file=sys.stderr)
        print("  git submodule update --init --recursive", file=sys.stderr)
        print("  make prebuild  # fans out to all platforms in parallel", file=sys.stderr)
        print("  make ge/lift-headers  # if headers/ subtree drifted", file=sys.stderr)
        print("  git add prebuilt/ headers/ vendor/github.com/", file=sys.stderr)
        print("  git commit --amend --no-edit", file=sys.stderr)
        print("", file=sys.stderr)
        print("To bypass for an unrelated commit:  git commit --no-verify", file=sys.stderr)
        return 1

    plats = ", ".join(platforms_checked)
    print(
        f"prebuilts ok ({plats}): "
        f"{total_counts['scripts']} scripts, "
        f"{total_counts['submods']} submodules, "
        f"{total_counts['prebuilts']} prebuilts, "
        f"{total_counts['inputs']} inputs"
        + (f" ({total_skipped_lfs} prebuilt LFS pointers skipped)" if total_skipped_lfs else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
