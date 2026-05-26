#!/usr/bin/env python3
"""Verify prebuilt/ios-arm64/manifest.json matches the working tree.

Exit 0 if all input hashes, prebuilt .a hashes, script hashes, and
submodule SHAs match the manifest. Exit 1 with a per-file diagnostic
otherwise — pointing the developer at `make ge/prebuild-vendor-ios-arm64`
and/or `make ge/lift-headers`.

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

import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = REPO_ROOT / "prebuilt/ios-arm64/manifest.json"


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


def main() -> int:
    if not MANIFEST_PATH.exists():
        print(
            f"error: {MANIFEST_PATH.relative_to(REPO_ROOT)} missing. "
            "Run `make ge/prebuild-vendor-ios-arm64` to generate it.",
            file=sys.stderr,
        )
        return 1

    with open(MANIFEST_PATH) as f:
        manifest = json.load(f)

    if manifest.get("version") != 1:
        print(
            f"error: manifest version {manifest.get('version')} not supported "
            "by this verifier (expected 1)",
            file=sys.stderr,
        )
        return 1

    errors: list[str] = []
    skipped_lfs = 0

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

    # 4) inputs — every source / header file the compile saw, filtered
    # to repo-root non-submodule paths.
    for rel, expected in manifest.get("inputs", {}).items():
        p = REPO_ROOT / rel
        if not p.exists():
            errors.append(f"missing input: {rel}")
            continue
        actual = sha256_file(p)
        if actual != expected:
            errors.append(f"input changed: {rel}")

    if errors:
        print("ERROR: prebuilt artefacts are stale.", file=sys.stderr)
        print("", file=sys.stderr)
        for e in errors[:40]:
            print(f"  {e}", file=sys.stderr)
        if len(errors) > 40:
            print(f"  ... and {len(errors) - 40} more", file=sys.stderr)
        print("", file=sys.stderr)
        print("Fix on a Mac with iOS SDK:", file=sys.stderr)
        print("  git submodule update --init --recursive", file=sys.stderr)
        print("  make ge/prebuild-vendor-ios-arm64", file=sys.stderr)
        print("  make ge/lift-headers     # if headers/ subtree drifted", file=sys.stderr)
        print("  git add prebuilt/ headers/ vendor/github.com/", file=sys.stderr)
        print("  git commit --amend --no-edit", file=sys.stderr)
        print("", file=sys.stderr)
        print("To bypass for an unrelated commit:  git commit --no-verify", file=sys.stderr)
        return 1

    n_scripts = len(manifest.get("scripts", {}))
    n_submods = len(manifest.get("submodule_shas", {}))
    n_prebuilt = len(manifest.get("prebuilts", {}))
    n_inputs = len(manifest.get("inputs", {}))
    print(
        f"prebuilts ok: {n_scripts} scripts, {n_submods} submodules, "
        f"{n_prebuilt} prebuilts, {n_inputs} inputs"
        + (f" ({skipped_lfs} prebuilt LFS pointers skipped)" if skipped_lfs else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
