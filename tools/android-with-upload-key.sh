#!/usr/bin/env bash
# Run a command with the Play upload key materialized for Gradle.
#
# Laptop: if ANDROID_KEYSTORE_PATH is unset, export a temp PKCS12 from
# the keychain (ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT required) and wipe it
# on exit.
# CI: ANDROID_KEYSTORE_PATH already points at a runner temp file; leave
# it alone.
#
# Usage (from the consuming game root):
#   ./ge/tools/android-with-upload-key.sh [--require] ./gradlew :app:bundleRelease
#
# --require fails if neither a path nor a keychain item can be used.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REQUIRE=0
if [[ "${1:-}" == "--require" ]]; then
  REQUIRE=1
  shift
fi
if [[ $# -eq 0 ]]; then
  echo "usage: $0 [--require] <command> [args…]" >&2
  exit 2
fi

KS_TMP=""
cleanup() {
  if [[ -n "$KS_TMP" ]]; then
    if rm -P "$KS_TMP" 2>/dev/null; then
      :
    else
      rm -f "$KS_TMP"
    fi
  fi
}
trap cleanup EXIT INT TERM

if [[ -z "${ANDROID_KEYSTORE_PATH:-}" ]]; then
  if [[ -n "${ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT:-}" ]]; then
    eval "$("$HERE/android-upload-keychain.sh" export)"
    KS_TMP="$ANDROID_KEYSTORE_PATH"
  elif [[ "$REQUIRE" -eq 1 ]]; then
    echo "error: no upload key. Set ANDROID_KEYSTORE_KEYCHAIN_ACCOUNT" >&2
    echo "  (laptop keychain) or ANDROID_KEYSTORE_PATH (CI temp file)." >&2
    exit 1
  fi
fi

if [[ ! -d android ]]; then
  echo "error: android/ not found — run 'make ge/android-init APP_ID=... APP_NAME=...' first" >&2
  exit 1
fi

cd android
# Do not exec — the EXIT trap must wipe the temp PKCS12.
"$@"
