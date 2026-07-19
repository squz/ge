#!/usr/bin/env bash
# Ensure prebuilt/<platform>/ (or prebuilt/<platform>-debug/) matches the
# working tree before a mobile link.
#
# Usage:
#   tools/ensure-prebuilt.sh [--debug] <platform>
#
# platform: ios-arm64 | ios-arm64-simulator | android-arm64
# --debug / GE_PREBUILD_DEBUG=1 → prebuilt/<platform>-debug/
#
# If the platform's manifest is already fresh (tools/verify-prebuilds.py),
# exit 0. Otherwise run a *full* prebuild of every archive in the tree.
# Partial (--libge-only) cooks are gone: they left libge ahead of vendor
# libs and caused the 2026-07 Android sqldeep SIGSEGV.
#
# Wired from Module.mk into `make ge/ios`, `ge/ios-device`, `ge/android`,
# etc.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

DEBUG=0
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) DEBUG=1; shift ;;
    -*)
      echo "error: unknown option: $1" >&2
      exit 1
      ;;
    *) ARGS+=("$1"); shift ;;
  esac
done

if [[ ${#ARGS[@]} -ne 1 ]]; then
  echo "usage: $0 [--debug] <platform>" >&2
  echo "  platform: ios-arm64 | ios-arm64-simulator | android-arm64" >&2
  exit 1
fi
PLATFORM="${ARGS[0]}"
if [[ "${GE_PREBUILD_DEBUG:-}" == "1" ]]; then
  DEBUG=1
fi

case "$PLATFORM" in
  ios-arm64|ios-arm64-simulator|android-arm64) ;;
  *)
    echo "error: unknown platform '$PLATFORM'" >&2
    exit 1
    ;;
esac

if [[ "$DEBUG" -eq 1 ]]; then
  PREBUILT_KEY="${PLATFORM}-debug"
  PREBUILD_FLAGS=(--debug)
else
  PREBUILT_KEY="$PLATFORM"
  PREBUILD_FLAGS=()
fi

GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

VERIFY=(python3 "$GE_ROOT/tools/verify-prebuilds.py" --platform "$PREBUILT_KEY")

if "${VERIFY[@]}" >/dev/null 2>&1 \
   && python3 "$GE_ROOT/tools/verify-cook.py" "prebuilt/$PREBUILT_KEY" >/dev/null 2>&1; then
  echo "ge: prebuilt/$PREBUILT_KEY is fresh (manifest + cook)"
  exit 0
fi
if "${VERIFY[@]}" >/dev/null 2>&1; then
  echo "ge: prebuilt/$PREBUILT_KEY cook.json does not match archives — full recook"
fi

echo "ge: prebuilt/$PREBUILT_KEY is stale — full prebuild of every archive…"
tools/prebuild.sh "${PREBUILD_FLAGS[@]}" "$PLATFORM"

if ! "${VERIFY[@]}"; then
  echo "error: prebuilt/$PREBUILT_KEY still stale after full rebuild." >&2
  exit 1
fi
if ! python3 "$GE_ROOT/tools/verify-cook.py" "prebuilt/$PREBUILT_KEY"; then
  echo "error: prebuilt/$PREBUILT_KEY cook check failed after full rebuild." >&2
  exit 1
fi
echo "ge: prebuilt/$PREBUILT_KEY refreshed (full cook)"
exit 0
