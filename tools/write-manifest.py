#!/usr/bin/env python3
"""Write prebuilt/ios-arm64/manifest.json from depfiles emitted by the
prebuild script.

Called at the end of `tools/prebuild-vendor-ios-arm64.sh`. Aggregates
the `.d` files clang wrote next to each `.o`, filters paths to the ge
repo root (drops system headers + paths inside submodules), hashes each
input, and writes a JSON manifest committed alongside the prebuilt .a
files.

The manifest's purpose is to let `tools/verify-prebuilds.py` (on dev
laptops via pre-commit hook + in CI on Linux runners) detect when the
prebuilts are stale relative to the source tree, without requiring the
verifier to actually re-compile.

Submodule contents are captured by `submodule_shas` (the commit pointer
recorded in the superproject's tree) rather than file hashes — that's
enough to detect bumps, and avoids needing submodule init on CI.

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
DEPFILE_GLOB = REPO_ROOT / "build/prebuilt/ios-arm64"
MANIFEST_PATH = REPO_ROOT / "prebuilt/ios-arm64/manifest.json"

SCRIPTS_TO_HASH = [
    "tools/prebuild-vendor-ios-arm64.sh",
    "tools/lift-headers.sh",
    "tools/write-manifest.py",
    "tools/verify-prebuilds.py",
]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_depfile(path: Path) -> list[str]:
    """Parse a Make-format dependency file. Returns dependency paths.

    Clang's `-MP` adds phony "header.h:" rules (one per included header,
    with no dependencies) so make doesn't choke if a header is later
    deleted. Skip any line that has its own `:` — that's a rule head,
    not a dependency. Only collect tokens between the FIRST rule's `:`
    and the next rule head."""
    deps: list[str] = []
    text = path.read_text()
    # Collapse line continuations so each logical Make line is one Python line.
    logical = text.replace("\\\n", " ")
    # First line is the real rule: "build/.../foo.o: src/foo.cpp <header>...".
    # Subsequent lines (from -MP) are "<header>:" with nothing after.
    seen_first_rule = False
    for line in logical.split("\n"):
        if ":" in line:
            if not seen_first_rule:
                seen_first_rule = True
                _, _, after = line.partition(":")
                for tok in after.split():
                    if tok and tok != "\\":
                        deps.append(tok)
            # All other ":" lines are -MP phony rules — skip.
        else:
            # Pure continuation-only line (rare) or empty.
            for tok in line.split():
                if tok and tok != "\\":
                    deps.append(tok)
    return deps


def aggregate_depfile_paths() -> set[Path]:
    """Walk all .d files under build/prebuilt/ios-arm64/, parse, return
    the union of dependency paths as absolute paths."""
    paths: set[Path] = set()
    for d in DEPFILE_GLOB.rglob("*.d"):
        for tok in parse_depfile(d):
            paths.add(Path(tok).resolve())
    return paths


def is_submodule_path(rel: str, submodule_paths: list[str]) -> bool:
    for sm in submodule_paths:
        if rel == sm or rel.startswith(sm + "/"):
            return True
    return False


def get_submodule_info() -> tuple[dict[str, str], list[str]]:
    """Returns ({submodule_path: committed_sha}, [submodule_paths])."""
    out = subprocess.check_output(
        ["git", "submodule", "status"], cwd=REPO_ROOT, text=True
    )
    shas: dict[str, str] = {}
    paths: list[str] = []
    for line in out.splitlines():
        # Format: " <sha> <path> (<describe>)" — leading char varies (-/+/U/space).
        m = re.match(r"^[ \-+U]([0-9a-f]+)\s+(\S+)", line)
        if not m:
            continue
        sha, path = m.group(1), m.group(2)
        shas[path] = sha
        paths.append(path)
    return shas, paths


def get_toolchain() -> dict[str, str]:
    """Capture xcode/SDK/clang versions for diagnostic clarity. Not used
    as a fail-stop in the verifier; just recorded."""
    def run(cmd: list[str]) -> str:
        try:
            return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
        except Exception:
            return "<unavailable>"

    clang_full = run(["xcrun", "--sdk", "iphoneos", "clang", "--version"])
    # Pull just the first line ("Apple clang version XYZ").
    clang_line = clang_full.split("\n", 1)[0] if clang_full else "<unavailable>"
    return {
        "clang": clang_line,
        "iphoneos_sdk_path": run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"]),
        "iphoneos_sdk_version": run(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"]),
    }


def main() -> int:
    if not DEPFILE_GLOB.exists():
        print(
            f"error: {DEPFILE_GLOB} doesn't exist. Run "
            "`make ge/prebuild-vendor-ios-arm64` first.",
            file=sys.stderr,
        )
        return 1

    submodule_shas, submodule_paths = get_submodule_info()

    # Aggregate depfile paths, filter to repo-internal + non-submodule.
    inputs: dict[str, str] = {}
    skipped_system = 0
    skipped_submodule = 0
    for abs_p in sorted(aggregate_depfile_paths()):
        try:
            rel = abs_p.relative_to(REPO_ROOT).as_posix()
        except ValueError:
            # Outside the repo (toolchain headers, system frameworks). Skip.
            skipped_system += 1
            continue
        if is_submodule_path(rel, submodule_paths):
            # Tracked by submodule SHA instead.
            skipped_submodule += 1
            continue
        if not abs_p.exists():
            print(f"warning: depfile referenced missing file: {rel}", file=sys.stderr)
            continue
        inputs[rel] = sha256_file(abs_p)

    # Hash the controlling scripts.
    scripts: dict[str, str] = {}
    for script_rel in SCRIPTS_TO_HASH:
        script_path = REPO_ROOT / script_rel
        if script_path.exists():
            scripts[script_rel] = sha256_file(script_path)

    # Hash the prebuilt .a files themselves — so the manifest also detects
    # someone editing a .a directly without rerunning the prebuild script
    # (or losing/replacing a .a). LFS-tracked content is read as the
    # actual binary, not the pointer, since this script runs on the
    # developer's laptop where smudge has materialised the files.
    prebuilts: dict[str, str] = {}
    prebuilt_dir = REPO_ROOT / "prebuilt/ios-arm64"
    for a in sorted(prebuilt_dir.glob("*.a")):
        prebuilts[a.relative_to(REPO_ROOT).as_posix()] = sha256_file(a)

    manifest = {
        "version": 1,
        "toolchain": get_toolchain(),
        "scripts": dict(sorted(scripts.items())),
        "submodule_shas": dict(sorted(submodule_shas.items())),
        "prebuilts": dict(sorted(prebuilts.items())),
        "inputs": dict(sorted(inputs.items())),
    }

    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(MANIFEST_PATH, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"wrote {MANIFEST_PATH.relative_to(REPO_ROOT)}")
    print(
        f"  toolchain: {manifest['toolchain']['clang']}, "
        f"SDK {manifest['toolchain']['iphoneos_sdk_version']}"
    )
    print(f"  scripts:        {len(scripts):4d}")
    print(f"  submodules:     {len(submodule_shas):4d}")
    print(f"  prebuilts:      {len(prebuilts):4d}")
    print(f"  inputs:         {len(inputs):4d}")
    print(f"  filtered:       {skipped_system} system, {skipped_submodule} submodule")
    return 0


if __name__ == "__main__":
    sys.exit(main())
