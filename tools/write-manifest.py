#!/usr/bin/env python3
"""Write prebuilt/<platform>/manifest.json from depfiles emitted by the
prebuild script.

Called at the end of `tools/prebuild.sh <platform>`. Aggregates the `.d`
files clang wrote next to each `.o`, filters paths to the ge repo root
(drops system headers + paths inside submodules), hashes each input,
and writes a JSON manifest committed alongside the prebuilt .a files.

Usage:
    python3 tools/write-manifest.py --platform ios-arm64
    python3 tools/write-manifest.py --platform ios-arm64-simulator
    python3 tools/write-manifest.py --platform android-arm64
    python3 tools/write-manifest.py --platform ios-arm64 --merge-existing-inputs

Default platform is `ios-arm64` for backward compatibility with the
original T71 caller.

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

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent

SCRIPTS_TO_HASH_COMMON = [
    "tools/lift-headers.sh",
    "tools/write-manifest.py",
    "tools/verify-prebuilds.py",
    "tools/prebuild.sh",
]
SUPPORTED_PLATFORMS = ("ios-arm64", "ios-arm64-simulator", "android-arm64", "web-wasm")


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


def aggregate_depfile_paths(depfile_root: Path) -> set[Path]:
    """Walk all .d files under build/prebuilt/<platform>/, parse, return
    the union of dependency paths as absolute paths."""
    paths: set[Path] = set()
    for d in depfile_root.rglob("*.d"):
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


def get_toolchain_ios(platform: str) -> dict[str, str]:
    """iOS xcode/SDK/clang versions for diagnostic clarity. Not a
    fail-stop in the verifier; just recorded."""
    def run(cmd: list[str]) -> str:
        try:
            return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
        except Exception:
            return "<unavailable>"

    sdk = "iphonesimulator" if platform == "ios-arm64-simulator" else "iphoneos"
    clang_full = run(["xcrun", "--sdk", sdk, "clang", "--version"])
    clang_line = clang_full.split("\n", 1)[0] if clang_full else "<unavailable>"
    return {
        "clang": clang_line,
        "sdk": sdk,
        "sdk_path": run(["xcrun", "--sdk", sdk, "--show-sdk-path"]),
        "sdk_version": run(["xcrun", "--sdk", sdk, "--show-sdk-version"]),
    }


def get_toolchain_android() -> dict[str, str]:
    """Android NDK clang version + NDK path. Reads $ANDROID_NDK_HOME or
    auto-detects under ~/Library/Android/sdk/ndk/."""
    def run(cmd: list[str]) -> str:
        try:
            return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
        except Exception:
            return "<unavailable>"

    ndk = os.environ.get("ANDROID_NDK_HOME", "")
    if not ndk:
        ndk_dir = Path.home() / "Library/Android/sdk/ndk"
        if ndk_dir.is_dir():
            cands = sorted(ndk_dir.iterdir())
            if cands:
                ndk = str(cands[-1])
    if not ndk or not Path(ndk).is_dir():
        return {"clang": "<unavailable>", "ndk_path": "<unavailable>"}

    # Find the host triple (darwin-x86_64 / linux-x86_64).
    host_dir = Path(ndk) / "toolchains/llvm/prebuilt"
    if host_dir.is_dir():
        hosts = list(host_dir.iterdir())
        if hosts:
            clang = hosts[0] / "bin/clang"
            full = run([str(clang), "--version"])
            clang_line = full.split("\n", 1)[0] if full else "<unavailable>"
            return {"clang": clang_line, "ndk_path": ndk}
    return {"clang": "<unavailable>", "ndk_path": ndk}


def get_toolchain_web() -> dict[str, str]:
    """Emscripten version (🎯T157). emcc from PATH, same resolution as
    tools/prebuild.sh's web-wasm case."""
    try:
        full = subprocess.check_output(
            ["emcc", "--version"], text=True, stderr=subprocess.DEVNULL
        ).strip()
        emcc_line = full.split("\n", 1)[0] if full else "<unavailable>"
    except Exception:
        emcc_line = "<unavailable>"
    return {"emcc": emcc_line}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "--platform",
        default="ios-arm64",
        choices=SUPPORTED_PLATFORMS,
        help="Platform directory under prebuilt/ to manifest (default: ios-arm64).",
    )
    parser.add_argument(
        "--merge-existing-inputs",
        action="store_true",
        help=(
            "Preserve input hashes from an existing manifest when this run did "
            "not emit depfiles for them. Used by libge-only prebuilds so "
            "unchanged vendor archive inputs remain verified without rebuilding "
            "vendor libraries."
        ),
    )
    args = parser.parse_args()
    platform = args.platform

    depfile_root = REPO_ROOT / f"build/prebuilt/{platform}"
    prebuilt_dir = REPO_ROOT / f"prebuilt/{platform}"
    manifest_path = prebuilt_dir / "manifest.json"

    if not depfile_root.exists():
        print(
            f"error: {depfile_root} doesn't exist. Run "
            f"`make prebuild-{platform}` first.",
            file=sys.stderr,
        )
        return 1

    submodule_shas, submodule_paths = get_submodule_info()

    # Aggregate depfile paths, filter to repo-internal + non-submodule
    # + non-generated (anything under build/ is a build-time intermediate
    # that won't exist on a fresh clone, so it'd fail verify-prebuilds).
    inputs: dict[str, str] = {}
    skipped_system = 0
    skipped_submodule = 0
    skipped_generated = 0
    for abs_p in sorted(aggregate_depfile_paths(depfile_root)):
        try:
            rel = abs_p.relative_to(REPO_ROOT).as_posix()
        except ValueError:
            skipped_system += 1
            continue
        if is_submodule_path(rel, submodule_paths):
            skipped_submodule += 1
            continue
        if rel.startswith("build/"):
            skipped_generated += 1
            continue
        if not abs_p.exists():
            print(f"warning: depfile referenced missing file: {rel}", file=sys.stderr)
            continue
        inputs[rel] = sha256_file(abs_p)

    if args.merge_existing_inputs and manifest_path.exists():
        with open(manifest_path) as f:
            old_manifest = json.load(f)
        for rel, expected in old_manifest.get("inputs", {}).items():
            inputs.setdefault(rel, expected)

    # Hash the controlling scripts. After tools/prebuild.sh consolidation
    # there is a single unified script driving every platform — a change
    # to it invalidates every platform's manifest, which is the right
    # behaviour given the case-statement inside.
    scripts: dict[str, str] = {}
    for script_rel in SCRIPTS_TO_HASH_COMMON:
        script_path = REPO_ROOT / script_rel
        if script_path.exists():
            scripts[script_rel] = sha256_file(script_path)

    # Hash the prebuilt .a files themselves.
    prebuilts: dict[str, str] = {}
    for a in sorted(prebuilt_dir.glob("*.a")):
        prebuilts[a.relative_to(REPO_ROOT).as_posix()] = sha256_file(a)

    if platform.startswith("ios-"):
        toolchain = get_toolchain_ios(platform)
    elif platform == "web-wasm":
        toolchain = get_toolchain_web()
    else:
        toolchain = get_toolchain_android()

    manifest = {
        "version": 2,
        "platform": platform,
        "toolchain": toolchain,
        "scripts": dict(sorted(scripts.items())),
        "submodule_shas": dict(sorted(submodule_shas.items())),
        "prebuilts": dict(sorted(prebuilts.items())),
        "inputs": dict(sorted(inputs.items())),
    }

    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"wrote {manifest_path.relative_to(REPO_ROOT)}")
    print(f"  toolchain: {toolchain.get('clang') or toolchain.get('emcc') or '<unavailable>'}")
    print(f"  scripts:        {len(scripts):4d}")
    print(f"  submodules:     {len(submodule_shas):4d}")
    print(f"  prebuilts:      {len(prebuilts):4d}")
    print(f"  inputs:         {len(inputs):4d}")
    print(f"  filtered:       {skipped_system} system, {skipped_submodule} submodule, {skipped_generated} generated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
