# TiltBuggy — in-tree ge sample app.
#
# Demonstrates the minimal-Makefile integration pattern: declare app-level
# variables, include ge/Module.mk, done. Everything else (link/compile rules,
# default target, shader compilation, run, clean, depgraph, compile_commands)
# is inherited.

ge := ../..

APP_NAME    := tiltbuggy
APP_DISPLAY_NAME := TiltBuggy
APP_ID      := com.squz.tiltbuggy
APP_SRC     := src/main.cpp src/Scene.cpp src/Renderer.cpp
APP_SHADERS := build/shaders/simple.h

-include $(ge)/Module.mk
$(ge)/Module.mk:
	git submodule update --init --recursive

# ── Visual regression baseline update ───────────────────────────────
#
# Re-runs the two visual-regression cells in update mode: each cell captures
# a screenshot at its known-stable point and calls `spyder baseline_update`
# to write the new PNG to ~/.spyder/visualdiff/ge-tiltbuggy/<cell-id>/...
#
# This target is developer-only — not part of `make check`. Run it after a
# deliberate rendering change to bless the new look as the correct baseline.
#
# Prerequisites:
#   - spyder installed and `spyder serve` running (or spyder auto-starts)
#   - iOS Simulator (tablet form factor) booted for the ios-sim-tablet-dist cell
#   - Android emulator running for the android-emu-tablet-dist cell
#
# Usage:
#   make update-baselines
#   make update-baselines VR_PIXEL_TOLERANCE=12   # loosen tolerance for a specific cell
.PHONY: update-baselines
update-baselines:
	VR_UPDATE_MODE=1 $(ge)/tools/matrix-cell.sh ios-sim-tablet-dist
	VR_UPDATE_MODE=1 GE_ANDROID_TABLET_AVD=$(GE_ANDROID_TABLET_AVD) \
	    $(ge)/tools/matrix-cell.sh android-emu-tablet-dist
