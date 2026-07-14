#!/usr/bin/env bash
# Ensure prebuilt/<platform>/ matches the working tree before a mobile link.
#
# Usage:
#   tools/ensure-prebuilt.sh <platform>
#
# platform: ios-arm64 | ios-arm64-simulator | android-arm64
#
# If the platform's manifest is already fresh (tools/verify-prebuilds.py),
# exit 0 immediately. Otherwise refresh libge.a (--libge-only); if the
# tree is still stale (vendor/submodule drift), run a full prebuild for
# that platform. Fail only if verify still fails after both attempts.
#
# Wired from Module.mk into `make ge/ios`, `ge/ios-device`, `ge/android`,
# etc., so mobile packages cannot silently link a stale libge.a.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <platform>" >&2
  echo "  platform: ios-arm64 | ios-arm64-simulator | android-arm64" >&2
  exit 1
fi
PLATFORM="$1"

case "$PLATFORM" in
  ios-arm64|ios-arm64-simulator|android-arm64) ;;
  *)
    echo "error: unknown platform '$PLATFORM'" >&2
    exit 1
    ;;
esac

GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

VERIFY=(python3 "$GE_ROOT/tools/verify-prebuilds.py" --platform "$PLATFORM")

if "${VERIFY[@]}" >/dev/null 2>&1; then
  echo "ge: prebuilt/$PLATFORM is fresh"
  exit 0
fi

echo "ge: prebuilt/$PLATFORM is stale relative to sources — refreshing…"
echo "ge: (this is automatic so mobile packages never link a silent old libge.a)"

# Prefer the cheap path: ge sources/headers only.
if [[ -f "prebuilt/$PLATFORM/manifest.json" ]]; then
  if tools/prebuild.sh --libge-only "$PLATFORM"; then
    if "${VERIFY[@]}" >/dev/null 2>&1; then
      echo "ge: prebuilt/$PLATFORM refreshed (libge-only)"
      exit 0
    fi
    echo "ge: still stale after libge-only (vendor/submodule drift?) — full prebuild…"
  else
    echo "ge: libge-only failed — falling back to full prebuild…"
  fi
fi

tools/prebuild.sh "$PLATFORM"

if "${VERIFY[@]}"; then
  echo "ge: prebuilt/$PLATFORM refreshed (full)"
  exit 0
fi

echo "error: prebuilt/$PLATFORM still stale after rebuild. See diagnostics above." >&2
exit 1
