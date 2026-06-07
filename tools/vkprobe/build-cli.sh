#!/usr/bin/env bash
# Build the ge Vulkan capability probe as a standalone arm64 Android CLI and
# (optionally) run it on connected devices. The probe's decision logic
# (ge_vk_probe in ge_vkprobe.c) is the same code that becomes libgevkprobe.so
# for GeActivity to dlopen before getLibraries() (🎯T107).
#
# Usage:
#   tools/vkprobe/build-cli.sh            # build only -> build/ge-vkprobe-arm64
#   tools/vkprobe/build-cli.sh --run      # build, then run on every adb device
#
# Only vkGetInstanceProcAddr is linked (Vulkan 1.0, API 24); all 1.1+ entry
# points are resolved dynamically, so the binary runs at low minSdk and simply
# rejects devices whose loader lacks a 1.1 entry point.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

GE_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$GE_ROOT"

# Pin the same NDK major the consumer fleet links with (🎯T103).
GE_ANDROID_NDK_MAJOR="${GE_ANDROID_NDK_MAJOR:-27}"
if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
  NDK="$ANDROID_NDK_HOME"
else
  NDK="$(ls -d "$HOME"/Library/Android/sdk/ndk/"$GE_ANDROID_NDK_MAJOR".*/ 2>/dev/null | sort -V | tail -1)"
  NDK="${NDK%/}"
fi
[[ -d "${NDK:-}" ]] || { echo "error: Android NDK r$GE_ANDROID_NDK_MAJOR not found" >&2; exit 1; }
HOST="$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)"
CC="$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang"
SYSROOT="$NDK/toolchains/llvm/prebuilt/$HOST/sysroot"

OUT="build/ge-vkprobe-arm64"
mkdir -p build
"$CC" --target=aarch64-linux-android24 --sysroot="$SYSROOT" \
  -O2 -Wall -Wextra tools/vkprobe/ge_vkprobe.c -lvulkan -o "$OUT"
echo "built $OUT"

[[ "${1:-}" == "--run" ]] || exit 0

for dev in $(adb devices | awk 'NR>1 && $2=="device"{print $1}'); do
  model="$(adb -s "$dev" shell getprop ro.product.model | tr -d '\r')"
  echo "── $model ($dev) ──"
  adb -s "$dev" push "$OUT" /data/local/tmp/ge-vkprobe >/dev/null
  adb -s "$dev" shell chmod 755 /data/local/tmp/ge-vkprobe
  adb -s "$dev" shell /data/local/tmp/ge-vkprobe || true
  echo
done
