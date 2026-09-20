#!/usr/bin/env bash
# Regression oracle for 🎯T191 / 🎯T186: ge resolves Android toolchain paths by
# glob, never by parsing `ls`.
#
# The host that produced T191 exports CLICOLOR_FORCE=1, so macOS `ls` emits
# ANSI escapes even into a pipe and every `$(ls …)` capture came back as an
# unusable path. Rather than depend on this machine's environment staying that
# way — which would make the oracle decay the day someone edits a dotfile —
# the test puts an `ls` on PATH that ALWAYS colourises, and asserts the
# resolved paths are clean anyway.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

ESC=$'\033'
fails=0

fail() { echo "  ✗ $*" >&2; fails=$((fails + 1)); }
pass() { echo "  ✓ $*"; }

# ── A hostile `ls` ───────────────────────────────────────────────────────
# Wraps every line of the real ls in SGR escapes, which is what macOS ls does
# under CLICOLOR_FORCE=1 and what GNU ls does under --color=always.
shim="$(mktemp -d)"
cat > "$shim/ls" <<'SHIM'
#!/bin/bash
/bin/ls "$@" | while IFS= read -r line; do printf '\033[1m\033[36m%s\033[39;49m\033[0m\n' "$line"; done
SHIM
chmod +x "$shim/ls"
export PATH="$shim:$PATH"
export CLICOLOR_FORCE=1 CLICOLOR=1

echo "── 🎯T191 Android NDK path resolution ──"

# ── 0. The oracle has teeth ──────────────────────────────────────────────
# Plant the original defect and confirm the shim reproduces it. Without this
# the rest of the file could pass against an `ls` that never colourises, which
# is exactly the hollow gate this target is about.
planted="$(ls -d "$HOME"/Library/Android/sdk/ndk/*/ 2>/dev/null | tail -1 || true)"
if [[ -z "$planted" ]]; then
  echo "  ⚠ no NDK installed under ~/Library/Android/sdk/ndk — cannot plant" >&2
elif [[ "$planted" != *"$ESC"* ]]; then
  fail "planted control: \$(ls …) came back clean, so this test proves nothing"
else
  pass "planted control: \$(ls …) is contaminated, as it must be"
fi

# ── 1. The resolvers ─────────────────────────────────────────────────────
# shellcheck source=tools/android-ndk.sh
source "$GE_ROOT/tools/android-ndk.sh"

major="${GE_ANDROID_NDK_MAJOR:-27}"
if ndk="$(ge_android_ndk_dir "$major")"; then
  if [[ "$ndk" == *"$ESC"* ]]; then
    fail "ge_android_ndk_dir returned an escape-contaminated path: $(printf '%q' "$ndk")"
  elif [[ ! -d "$ndk" ]]; then
    fail "ge_android_ndk_dir returned a non-directory: $(printf '%q' "$ndk")"
  else
    pass "ge_android_ndk_dir -> $ndk"
  fi

  if host="$(ge_android_ndk_host "$ndk")"; then
    if [[ "$host" == *"$ESC"* ]]; then
      fail "ge_android_ndk_host returned an escape-contaminated triple: $(printf '%q' "$host")"
    elif [[ ! -x "$ndk/toolchains/llvm/prebuilt/$host/bin/clang" ]]; then
      fail "no runnable clang under host triple $host"
    else
      pass "ge_android_ndk_host -> $host (clang is runnable)"
    fi
  else
    fail "ge_android_ndk_host found no usable llvm toolchain in $ndk"
  fi
else
  echo "  ⚠ no r$major NDK installed — resolver assertions skipped" >&2
fi

# ── 1b. The empty and half-installed cases, under real bash 3.2 ──────────
# Two holes this closes, both raised against the first cut of this file:
#
#   * `#!/usr/bin/env bash` picks Homebrew's bash 5 on a dev Mac, so nothing
#     above ever ran on the 3.2.57 that macOS actually ships — the exact
#     invisibility bash.md warns about. These cases re-exec under /bin/bash.
#   * The resolvers were only ever exercised on a host WITH an NDK. The
#     no-NDK path is the one that leaves the candidate array empty, and an
#     empty array under `set -u` is bash 3.2's classic trap.
#
# Asserting /bin/bash really is 3.x first, because a version check that
# silently passes on bash 5 would make this whole section vacuous.
bash32_version="$(/bin/bash -c 'echo "$BASH_VERSION"')"
case "$bash32_version" in
  3.*) pass "/bin/bash is $bash32_version (3.2 trap is actually exercised)" ;;
  *)   fail "/bin/bash is $bash32_version, not 3.x — the bash 3.2 cases below prove nothing" ;;
esac

fixture="$(mktemp -d)"
trap 'rm -rf "$shim" "$fixture"' EXIT
mkdir -p "$fixture/empty-home"
# A half-installed NDK: the directory and the host triple exist, the clang
# does not. ge must call that "no usable toolchain", not hand back a path
# that dies at exec time.
mkdir -p "$fixture/half-home/Library/Android/sdk/ndk/27.9.99/toolchains/llvm/prebuilt/darwin-x86_64/bin"

if /bin/bash -euo pipefail -c '
  set -euo pipefail
  source "$1/tools/android-ndk.sh"

  HOME="$2/empty-home"
  if ge_android_ndk_dir 27 > "$2/out" 2>&1; then
    echo "FAIL: ge_android_ndk_dir succeeded with no NDK installed"; exit 1
  fi
  if [ -s "$2/out" ]; then
    echo "FAIL: ge_android_ndk_dir printed something with no NDK installed"; exit 1
  fi

  HOME="$2/half-home"
  ndk="$(ge_android_ndk_dir 27)"
  [ "$ndk" = "$2/half-home/Library/Android/sdk/ndk/27.9.99" ] || {
    echo "FAIL: half-installed NDK resolved to [$ndk]"; exit 1
  }
  if ge_android_ndk_host "$ndk" > "$2/out" 2>&1; then
    echo "FAIL: ge_android_ndk_host accepted a triple with no runnable clang"; exit 1
  fi
  exit 0
' _ "$GE_ROOT" "$fixture"; then
  pass "bash 3.2: empty and half-installed NDK trees resolve cleanly under set -u"
else
  fail "bash 3.2: resolvers misbehave on an empty or half-installed NDK tree"
fi

# ── 2. No Android script builds a path out of `ls` again ─────────────────
# Runs everywhere, NDK or not: this is the part that catches a reintroduction.
for f in tools/prebuild.sh tools/vkprobe/build-cli.sh tools/rebuild-sdl-prebuilts.sh \
         tools/android-ndk.sh tools/ship/release.sh scripts/t156-oracle; do
  # Strip whole-line comments first — this very rule is described in prose in
  # tools/android-ndk.sh, and a doc mention is not a defect.
  if sed 's/^[[:space:]]*#.*//' "$f" | grep -nE '\$\((ls|/bin/ls)[[:space:]]'; then
    fail "$f builds a value from \`ls\` output (🎯T186)"
  else
    pass "$f: no \$(ls …) capture"
  fi
done

if [[ $fails -ne 0 ]]; then
  echo "✗ $fails check(s) failed" >&2
  exit 1
fi
echo "✓ Android NDK path resolution is colour-proof"
