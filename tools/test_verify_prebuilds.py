#!/usr/bin/env python3
"""Regression tests for tools/verify-prebuilds.py (🎯T181.4).

verify-prebuilds.py answers exactly one question after the Phase 1 LFS
exit: **is anyone committing binaries to git?** It does not look at
freshness (tools/verify-manifest.py) or at archive integrity
(tools/verify-cook.py), and it deliberately takes no arguments — an
earlier version accepted `--platform`, and when the script was
repurposed without updating its callers the flag was silently ignored,
turning ensure-prebuilt.sh's freshness gate into a no-op.

Each test builds a real git repo in a TemporaryDirectory (the verifier
shells out to `git ls-files`), stages some files, and runs the verifier
with GE_REPO_ROOT pointing at it.

Run: `python3 tools/test_verify_prebuilds.py`
     `make python-test`           (wired into bullseye)
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


VERIFIER = Path(__file__).resolve().parent / "verify-prebuilds.py"


class GitRepo:
    """A real git repo — the verifier reads the index via `git ls-files`,
    so an on-disk-only fake would report nothing and every test would
    pass vacuously."""

    def __init__(self, root: Path):
        self.root = root
        self._git("init", "-q")
        self._git("config", "user.email", "test@example.com")
        self._git("config", "user.name", "test")

    def _git(self, *args: str) -> None:
        subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=True, capture_output=True,
        )

    def track(self, rel: str, data: bytes = b"stub") -> None:
        """Create a file and stage it, so `git ls-files` reports it."""
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        # -f: the repo under test may carry a .gitignore that would
        # otherwise stop us staging the very path we want to check.
        self._git("add", "-f", rel)

    def write_untracked(self, rel: str, data: bytes = b"stub") -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def run(self) -> subprocess.CompletedProcess:
        env = dict(os.environ, GE_REPO_ROOT=str(self.root))
        return subprocess.run(
            [sys.executable, str(VERIFIER)],
            cwd=self.root, env=env, capture_output=True, text=True,
        )


class VerifyPrebuiltsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.repo = GitRepo(self.tmp)

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ── happy path ─────────────────────────────────────────────────

    def test_clean_repo_passes(self) -> None:
        self.repo.track("src/main.cpp", b"int main(){}")
        self.repo.track("prebuilt/ios-arm64/cook.json", b"{}")
        self.repo.track("prebuilt/ios-arm64/manifest.json", b"{}")

        r = self.repo.run()
        self.assertEqual(r.returncode, 0, f"stderr={r.stderr!r}")
        self.assertIn("prebuilts ok", r.stdout)

    def test_untracked_archive_is_fine(self) -> None:
        """The whole point of Phase 1: archives may exist on disk as a
        local cook cache, as long as git does not carry them."""
        self.repo.write_untracked("prebuilt/ios-arm64/libge.a", b"\x00ar")

        r = self.repo.run()
        self.assertEqual(r.returncode, 0, f"stderr={r.stderr!r}")

    # ── rejected binaries ──────────────────────────────────────────

    def test_tracked_prebuilt_archive_fails(self) -> None:
        self.repo.track("prebuilt/ios-arm64/libge.a", b"\x00ar")

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("prebuilt/ios-arm64/libge.a", r.stderr)

    def test_tracked_debug_tree_archive_fails(self) -> None:
        self.repo.track("prebuilt/android-arm64-debug/libbox2d.a", b"\x00ar")

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("prebuilt/android-arm64-debug/libbox2d.a", r.stderr)

    def test_tracked_vendor_archive_fails(self) -> None:
        self.repo.track("vendor/ffmpeg/lib/android-arm64/libavcodec.a", b"\x00ar")

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("libavcodec.a", r.stderr)

    def test_tracked_jar_fails(self) -> None:
        self.repo.track("formal/tla2tools.jar", b"PK\x03\x04")

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("formal/tla2tools.jar", r.stderr)

    def test_tracked_shaderc_fails(self) -> None:
        self.repo.track("bin/darwin-arm64/shaderc", b"\x7fELF")

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("shaderc", r.stderr)

    # ── the one exemption ──────────────────────────────────────────

    def test_gradle_wrapper_jar_is_exempt(self) -> None:
        """tools/init-android.sh copies gradle/ wholesale into the
        scaffolded project, so this 54 KB jar is a build input consumers
        need — not a cook output. Deleting it left `make ge/android-init`
        producing projects whose ./gradlew could not start."""
        self.repo.track(
            "tools/android-template/gradle/wrapper/gradle-wrapper.jar",
            b"PK\x03\x04",
        )

        r = self.repo.run()
        self.assertEqual(r.returncode, 0, f"stderr={r.stderr!r}")

    # ── argument surface ───────────────────────────────────────────

    def test_reports_every_offender_not_just_the_first(self) -> None:
        self.repo.track("prebuilt/ios-arm64/libge.a", b"\x00ar")
        self.repo.track("prebuilt/ios-arm64/libbox2d.a", b"\x00ar")

        r = self.repo.run()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("libge.a", r.stderr)
        self.assertIn("libbox2d.a", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
