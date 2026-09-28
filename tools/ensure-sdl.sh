#!/usr/bin/env bash
# Cook vendor/sdl3/lib/<platform>/*.a on demand, the way
# tools/ensure-prebuilt.sh cooks prebuilt/<platform>/ (🎯T190 / 🎯T195).
#
# Usage:
#   tools/ensure-sdl.sh <platform>
#
# platform: macos-arm64 | ios-arm64 | ios-arm64-simulator
#
# Phase 1 of the LFS exit (97f244c) deleted every vendor/sdl3/lib/**/*.a and
# put nothing in their place: prebuilt/ gained a cook (ensure-prebuilt.sh),
# the vendor SDL set did not. A consumer with zero archives could therefore
# build iOS and Android but died on the desktop link with seven
# "no such file or directory". This script closes that gap — it is the
# ensure-prebuilt.sh of vendor/sdl3.
#
# It fast-exits when the platform's whole archive set is already present, so
# it is cheap to hang off every link rule. Otherwise it runs
# tools/rebuild-sdl-prebuilts.sh for that platform ONLY (the script used to
# cook all four platforms plus the xcframeworks on every invocation).
#
# Make invokes this once per missing archive, and under -j those invocations
# are concurrent, so the cook is serialised behind a mkdir mutex: the first
# caller cooks, the rest block, then find the archives in place and exit.
#
# GE_ENSURE_SDL_LOCK_TIMEOUT overrides the wait (seconds, default 1800).
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <platform>" >&2
  echo "  platform: macos-arm64 | ios-arm64 | ios-arm64-simulator" >&2
  exit 1
fi
PLATFORM="$1"

case "$PLATFORM" in
  macos-arm64|ios-arm64|ios-arm64-simulator) ;;
  android-arm64)
    # cmake/android-arm64.cmake builds SDL3/SDL3_image/SDL3_ttf from the
    # submodule with add_subdirectory, so Android links no vendor archive.
    echo "ge: android-arm64 builds SDL from source — nothing to cook"
    exit 0
    ;;
  *)
    echo "error: unknown platform '$PLATFORM'" >&2
    exit 1
    ;;
esac

GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

LIB_DIR="vendor/sdl3/lib/$PLATFORM"
# The seven archives every ge link line on this platform names. Keep in sync
# with ge/SDL_LIBS in Module.mk and with LIBRARY_SEARCH_PATHS in
# tools/ios-build/build_project.rb.
ARCHIVES="libSDL3.a libSDL3_image.a libSDL3_ttf.a libfreetype.a libharfbuzz.a libplutosvg.a libplutovg.a"

missing() {
  local a
  for a in $ARCHIVES; do
    if [[ ! -f "$LIB_DIR/$a" ]]; then
      echo "$a"
    fi
  done
}

if [[ -z "$(missing)" ]]; then
  exit 0
fi

mkdir -p "$GE_ROOT/build"
LOCK="$GE_ROOT/build/.ensure-sdl-$PLATFORM.lock"
LOCK_PID_FILE="$LOCK/pid"
LOCK_TIMEOUT="${GE_ENSURE_SDL_LOCK_TIMEOUT:-1800}"

# 🎯T201: a cook killed mid-flight (SIGKILL, OOM, ctrl-C) leaves $LOCK behind
# with nothing to remove it — every later invocation then waited out the
# full $LOCK_TIMEOUT (30 minutes) before failing. The holder writes its own
# pid into $LOCK/pid right after acquiring; a waiter that finds the lock
# held by a pid that is no longer running knows the lock is stale (the
# holder cannot possibly still be cooking) and reclaims it immediately
# instead of waiting. A lock directory with no pid file yet is a holder
# mid-way through acquiring — not stale — and is still subject to the
# ordinary timeout.
waited=0
until mkdir "$LOCK" 2>/dev/null; do
  if [[ -f "$LOCK_PID_FILE" ]]; then
    holder_pid="$(cat "$LOCK_PID_FILE" 2>/dev/null || true)"
    if [[ -n "$holder_pid" ]] && ! kill -0 "$holder_pid" 2>/dev/null; then
      echo "ge: $LOCK held by dead pid $holder_pid — reclaiming stale lock" >&2
      rm -rf "$LOCK"
      continue
    fi
  fi
  if [[ "$waited" -ge "$LOCK_TIMEOUT" ]]; then
    echo "error: timed out after ${LOCK_TIMEOUT}s waiting for $LOCK" >&2
    echo "       A previous cook may have been killed; remove that directory." >&2
    exit 1
  fi
  if [[ "$waited" -eq 0 ]]; then
    echo "ge: waiting for a concurrent vendor/sdl3/$PLATFORM cook…"
  fi
  sleep 2
  waited=$((waited + 2))
done
echo "$$" > "$LOCK_PID_FILE"
trap 'rm -rf "$LOCK" 2>/dev/null || true' EXIT

# Re-check: whoever held the lock may have cooked exactly what we wanted.
STILL_MISSING="$(missing)"
if [[ -z "$STILL_MISSING" ]]; then
  exit 0
fi

# Cooking compiles vendor sources, so the SDL submodule must be initialised.
# A consumer that checks ge out non-recursively has ge but none of its vendor
# trees — and since archives are never published under the source-only
# doctrine, there is nothing to fetch and nothing to cook from. Say so
# plainly instead of dying inside cmake on the first missing source. This is
# the same fail-loud shape as tools/ensure-prebuilt.sh.
SDL_SRC="vendor/github.com/libsdl-org/SDL"
if [[ ! -e "$SDL_SRC/.git" ]]; then
  {
    echo "error: cannot cook $LIB_DIR — ge's SDL submodule is not initialised."
    echo ""
    echo "Archives are no longer committed (Phase 1 LFS exit), so they have to be"
    echo "cooked from vendor sources. Pick one:"
    echo ""
    echo "  # Full checkout (developer / co-dev):"
    echo "  git -C $GE_ROOT submodule update --init --recursive"
    echo ""
    echo "  # Consumer CI: check ge out recursively too —"
    echo "  #   actions/checkout with submodules: recursive"
    echo "  # ge prebuilts are cooked from source, never downloaded."
    echo ""
    echo "See docs/vendor-prebuilds.md."
  } >&2
  exit 1
fi

echo "ge: $LIB_DIR is missing $(echo $STILL_MISSING | tr '\n' ' ')— cooking from source…"
tools/rebuild-sdl-prebuilts.sh "$PLATFORM"

STILL_MISSING="$(missing)"
if [[ -n "$STILL_MISSING" ]]; then
  echo "error: $LIB_DIR still missing after cook:" >&2
  echo "$STILL_MISSING" | sed 's/^/  /' >&2
  exit 1
fi
echo "ge: $LIB_DIR cooked"
exit 0
