#!/usr/bin/env python3
"""Regression tests for tools/verify-prebuilds.py (🎯T78).

Each test builds a minimal fake "ge" repo in a TemporaryDirectory, drops
a single manifest into prebuilt/<platform>/manifest.json, and runs the
verifier against it via subprocess with GE_REPO_ROOT pointing at the
temp dir. We check exit code + stderr for the expected diagnostic.

Run: `python3 tools/test_verify_prebuilds.py`
     `make python-test`           (wired into bullseye)
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


VERIFIER = Path(__file__).resolve().parent / "verify-prebuilds.py"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FakeRepo:
    """Minimal repo for verifier tests: a tmpdir with .gitmodules, fake
    submodule worktrees, fake prebuilt .a files, and a manifest that
    references them."""

    def __init__(self, root: Path):
        self.root = root
        self.submodules: dict[str, str] = {}  # path -> sha
        self.scripts: dict[str, bytes] = {}
        self.prebuilts: dict[str, bytes] = {}  # rel path -> bytes
        self.inputs: dict[str, bytes] = {}     # rel path -> bytes

    def add_submodule(self, path: str, sha: str = "a" * 40) -> "FakeRepo":
        self.submodules[path] = sha
        return self

    def add_script(self, path: str, content: bytes = b"#!/bin/sh\n") -> "FakeRepo":
        self.scripts[path] = content
        return self

    def add_prebuilt(self, path: str, content: bytes = b"\x00prebuilt") -> "FakeRepo":
        self.prebuilts[path] = content
        return self

    def add_input(self, path: str, content: bytes = b"// stub\n") -> "FakeRepo":
        self.inputs[path] = content
        return self

    def materialise(self) -> None:
        """Write everything to disk. Must be called before run()."""
        # .gitmodules + git init so `git submodule status` returns the
        # right shape.
        gitmodules = ""
        for sm_path in self.submodules:
            gitmodules += f'[submodule "{sm_path}"]\n\tpath = {sm_path}\n\turl = https://example.invalid\n'
        (self.root / ".gitmodules").write_text(gitmodules)
        subprocess.check_output(["git", "init", "-q"], cwd=self.root)
        # Tell git about each submodule path. We don't actually init real
        # submodules; we fake the commit-pointer index entries so
        # `git submodule status` reports the SHA we asked for.
        for sm_path, sha in self.submodules.items():
            sm_dir = self.root / sm_path
            sm_dir.mkdir(parents=True, exist_ok=True)
            # Add a 160000 (gitlink) index entry pointing at the SHA.
            subprocess.check_output(
                ["git", "update-index", "--add", "--cacheinfo", f"160000,{sha},{sm_path}"],
                cwd=self.root,
            )
        # Files.
        for rel, content in self.scripts.items():
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(content)
        for rel, content in self.prebuilts.items():
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(content)
        for rel, content in self.inputs.items():
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(content)

    def write_manifest(
        self,
        platform: str = "ios-arm64",
        *,
        scripts: dict[str, str] | None = None,
        submodule_shas: dict[str, str] | None = None,
        prebuilts: dict[str, str] | None = None,
        inputs: dict[str, str] | None = None,
        toolchain: dict[str, str] | None = None,
        version: int = 2,
    ) -> Path:
        """Write a manifest under prebuilt/<platform>/manifest.json.
        Each kwarg defaults to a manifest that matches what materialise()
        put on disk. Pass an override to introduce a mismatch."""
        if scripts is None:
            scripts = {rel: sha256(c) for rel, c in self.scripts.items()}
        if submodule_shas is None:
            submodule_shas = dict(self.submodules)
        if prebuilts is None:
            prebuilts = {rel: sha256(c) for rel, c in self.prebuilts.items()}
        if inputs is None:
            inputs = {rel: sha256(c) for rel, c in self.inputs.items()}
        if toolchain is None:
            toolchain = {"clang": "fake-clang"}

        manifest = {
            "version":         version,
            "platform":        platform,
            "toolchain":       toolchain,
            "scripts":         scripts,
            "submodule_shas":  submodule_shas,
            "prebuilts":       prebuilts,
            "inputs":          inputs,
        }
        manifest_path = self.root / f"prebuilt/{platform}/manifest.json"
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        return manifest_path

    def run(self, *args: str) -> subprocess.CompletedProcess:
        env = {**os.environ, "GE_REPO_ROOT": str(self.root)}
        return subprocess.run(
            [sys.executable, str(VERIFIER), *args],
            env=env,
            capture_output=True,
            text=True,
        )


class VerifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="ge-verify-test-")
        self.repo = FakeRepo(Path(self.tmp))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ── happy path ─────────────────────────────────────────────────

    def test_happy_path_passes(self) -> None:
        self.repo.add_submodule("vendor/foo")
        self.repo.add_script("tools/build.sh")
        self.repo.add_prebuilt("prebuilt/ios-arm64/libge.a")
        self.repo.add_input("src/main.cpp")
        self.repo.materialise()
        self.repo.write_manifest()

        r = self.repo.run()
        self.assertEqual(r.returncode, 0, f"stderr={r.stderr!r}")
        self.assertIn("prebuilts ok", r.stdout)

    # ── scripts ────────────────────────────────────────────────────

    def test_script_hash_mismatch(self) -> None:
        self.repo.add_script("tools/build.sh", b"version 1")
        self.repo.materialise()
        # Manifest claims a different hash.
        self.repo.write_manifest(scripts={"tools/build.sh": sha256(b"version 2")})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("script changed: tools/build.sh", r.stderr)

    def test_missing_script(self) -> None:
        self.repo.materialise()
        self.repo.write_manifest(scripts={"tools/gone.sh": sha256(b"")})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("missing script: tools/gone.sh", r.stderr)

    # ── submodules ─────────────────────────────────────────────────

    def test_submodule_sha_changed(self) -> None:
        self.repo.add_submodule("vendor/foo", sha="b" * 40)
        self.repo.materialise()
        self.repo.write_manifest(submodule_shas={"vendor/foo": "a" * 40})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("submodule SHA changed: vendor/foo", r.stderr)

    def test_manifest_references_ghost_submodule(self) -> None:
        """A submodule listed in the manifest but no longer in
        .gitmodules — the T38 case described in 🎯T78's context."""
        self.repo.materialise()
        self.repo.write_manifest(submodule_shas={"vendor/ghost": "a" * 40})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("submodule not configured: vendor/ghost", r.stderr)

    def test_submodule_added_but_not_in_manifest(self) -> None:
        """The converse case: a submodule in .gitmodules that the
        manifest doesn't list. Means write-manifest wasn't re-run after
        the submodule was added. Closed gap (🎯T78)."""
        self.repo.add_submodule("vendor/new-thing")
        self.repo.materialise()
        self.repo.write_manifest(submodule_shas={})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("submodule not in manifest: vendor/new-thing", r.stderr)

    # ── prebuilts ──────────────────────────────────────────────────

    def test_prebuilt_hash_mismatch(self) -> None:
        self.repo.add_prebuilt("prebuilt/ios-arm64/libge.a", b"v1")
        self.repo.materialise()
        self.repo.write_manifest(prebuilts={"prebuilt/ios-arm64/libge.a": sha256(b"v2")})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("prebuilt changed: prebuilt/ios-arm64/libge.a", r.stderr)

    def test_missing_prebuilt(self) -> None:
        """Manifest references a .a file that was deleted from disk."""
        self.repo.materialise()
        self.repo.write_manifest(
            prebuilts={"prebuilt/ios-arm64/libgone.a": sha256(b"")},
        )

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("missing prebuilt: prebuilt/ios-arm64/libgone.a", r.stderr)

    def test_prebuilt_added_but_not_in_manifest(self) -> None:
        """The converse: a .a appears in prebuilt/<platform>/ but the
        manifest doesn't list it. Closed gap (🎯T78)."""
        self.repo.add_prebuilt("prebuilt/ios-arm64/libnew.a", b"xyz")
        self.repo.materialise()
        self.repo.write_manifest(prebuilts={})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("prebuilt not in manifest: prebuilt/ios-arm64/libnew.a", r.stderr)

    # ── inputs ─────────────────────────────────────────────────────

    def test_input_hash_mismatch(self) -> None:
        self.repo.add_input("src/main.cpp", b"// v1")
        self.repo.materialise()
        self.repo.write_manifest(inputs={"src/main.cpp": sha256(b"// v2")})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("input changed: src/main.cpp", r.stderr)

    def test_missing_input(self) -> None:
        self.repo.materialise()
        self.repo.write_manifest(inputs={"src/gone.cpp": sha256(b"")})

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("missing input: src/gone.cpp", r.stderr)

    # ── Android NDK ABI pin (🎯T100) ───────────────────────────────

    def test_android_ndk_pin_ok(self) -> None:
        """android-arm64 prebuilt built with the pinned NDK major passes."""
        self.repo.add_prebuilt("prebuilt/android-arm64/libge.a")
        self.repo.materialise()
        self.repo.write_manifest(
            platform="android-arm64",
            toolchain={
                "clang": "fake-clang",
                "ndk_path": "/opt/Android/sdk/ndk/27.0.12077973",
            },
        )

        r = self.repo.run()
        self.assertEqual(r.returncode, 0, f"stderr={r.stderr!r}")

    def test_android_ndk_pin_too_new_fails(self) -> None:
        """A prebuilt built with a newer NDK than the pin is rejected —
        the exact 🎯T100 skew (NDK 29 prebuilt, NDK 27 consumer)."""
        self.repo.add_prebuilt("prebuilt/android-arm64/libge.a")
        self.repo.materialise()
        self.repo.write_manifest(
            platform="android-arm64",
            toolchain={
                "clang": "fake-clang",
                "ndk_path": "/opt/Android/sdk/ndk/29.0.14206865",
            },
        )

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("built with NDK r29", r.stderr)
        self.assertIn("T100", r.stderr)

    # ── manifest format ────────────────────────────────────────────

    def test_unsupported_version(self) -> None:
        self.repo.materialise()
        self.repo.write_manifest(version=99)

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unsupported manifest version 99", r.stderr)

    def test_no_manifests_at_all(self) -> None:
        """`prebuilt/` empty — verifier should report that explicitly,
        not silently pass."""
        (self.repo.root / "prebuilt").mkdir()
        # No git init, no submodules, no scripts — bare repo root.
        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no prebuilt/*/manifest.json found", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
