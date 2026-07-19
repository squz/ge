#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""Structural proof that the shipped iOS orientation path covers the SDL glass.

SDL_uikitviewcontroller overrides supportedInterfaceOrientations; a
UIViewController-only swizzle never runs on the real glass. iPadOS 26 also
requires TN3192 freeze *after* the interface adopts the requested mask
(freezing too early freezes portrait and letterboxes).

This script asserts the source contracts. Runtime metrics (UIScreen.bounds
landscape, window fill, Sokol W>H) are recorded under goal scratch when
exercising M5 with chassis portrait.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    mm = (ROOT / "tools/player_orientation_ios.mm").read_text()
    sh = (ROOT / "src/SessionHost.mm").read_text()
    checks = [
        ("swizzle SDL_uikitviewcontroller by name", "SDL_uikitviewcontroller" in mm),
        ("separate g_supportedOrientation mask", "g_supportedOrientation" in mm),
        ("separate g_freezeOrientation TN3192", "g_freezeOrientation" in mm),
        ("request geometry while freeze can be off", "requestGeometryUpdateWithPreferences" in mm),
        ("log freeze ON after match", "freeze ON" in mm),
        ("SessionHost arms before DirectRenderHost",
         "playerForceOrientation(config.orientation)" in sh
         and sh.find("playerForceOrientation(config.orientation)")
         < sh.find("DirectRenderHost host(config)")),
        ("SDL_HINT_ORIENTATIONS armed early", "SDL_HINT_ORIENTATIONS" in sh),
    ]
    ok = True
    for name, passed in checks:
        print(f"{'PASS' if passed else 'FAIL'}: {name}")
        ok = ok and passed
    print("OVERALL", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
