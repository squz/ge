#!/usr/bin/env python3
"""Unit tests for tools/verify-cook.py.

Run: `python3 tools/test_verify_cook.py`

Copyright 2026 Marcelo Cantos
SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

VERIFIER = Path(__file__).resolve().parent / "verify-cook.py"
REQUIRED_STEMS = (
    "ge",
    "box2d",
    "lunasvg_ge",
    "plutovg_ge",
    "sqlite3_ge",
    "lz4_ge",
    "liteparser",
)


def _write_tree(root: Path, corrupt: str | None = None) -> None:
    archives = {}
    for stem in REQUIRED_STEMS:
        path = root / f"lib{stem}.a"
        good = f"payload-for-{stem}".encode()
        path.write_bytes(b"CORRUPT-" + good if corrupt == stem else good)
        archives[stem] = hashlib.sha256(good).hexdigest()
    (root / "cook.json").write_text(
        json.dumps({"version": 1, "archives": archives}, indent=2) + "\n"
    )


def run_verify(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VERIFIER), str(root)],
        capture_output=True,
        text=True,
    )


class VerifyCookTests(unittest.TestCase):
    def test_ok(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_tree(root)
            r = run_verify(root)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_missing_cook(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "libge.a").write_bytes(b"x")
            r = run_verify(root)
            self.assertEqual(r.returncode, 1)
            self.assertIn("cook.json", r.stderr)

    def test_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_tree(root, corrupt="liteparser")
            r = run_verify(root)
            self.assertEqual(r.returncode, 1)
            self.assertIn("liteparser", r.stderr)
            self.assertIn("does not match", r.stderr)

    def test_orphan_archive(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_tree(root)
            (root / "libextra.a").write_bytes(b"extra")
            r = run_verify(root)
            self.assertEqual(r.returncode, 1)
            self.assertIn("libextra.a", r.stderr)


if __name__ == "__main__":
    unittest.main()
