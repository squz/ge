# Android NDK toolchain discovery, shared by ge's Android build scripts.
# Source this file; it defines functions and runs nothing.
#
# 🎯T191 / 🎯T186 — every path here is built by glob expansion, never by
# parsing `ls` output. macOS `ls` honours CLICOLOR_FORCE=1 even when stdout is
# a pipe, and that variable is exported by a common dotfile idiom, so
# `$(ls -d …)` hands back a path wrapped in ANSI escapes:
#
#   $'\E[1m\E[36m/Users/…/ndk/27.3.13750724/\E[39;49m\E[0m'
#
# `[[ -d ]]` then fails on a directory that plainly exists and the cook dies
# claiming the NDK is not installed. Quoting does not help — the escapes are in
# the bytes `ls` emits. A glob is expanded by the shell itself, so no external
# program's idea of formatting can get into a path ge is about to exec.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

# ge_android_ndk_dir <major>
#
# Print the highest-versioned NDK installed under the standard macOS SDK
# location whose major version is <major> (no trailing slash). Returns 1 with
# no output when none is installed.
ge_android_ndk_dir() {
  local major="$1"
  local d
  local candidates=()

  for d in "$HOME"/Library/Android/sdk/ndk/"$major".*/; do
    # A non-matching glob stays literal in bash, so the -d test is also what
    # distinguishes "nothing installed" from a real hit.
    [[ -d "$d" ]] || continue
    candidates+=("${d%/}")
  done

  [[ ${#candidates[@]} -gt 0 ]] || return 1
  printf '%s\n' "${candidates[@]}" | sort -V | tail -1
}

# ge_android_ndk_host <ndk-dir>
#
# Print the name of the LLVM prebuilt host-triple directory (e.g.
# darwin-x86_64) inside <ndk-dir>. Only a triple that actually carries a
# runnable clang qualifies, so a half-installed NDK is reported as missing a
# toolchain rather than producing a path that fails at exec time. Returns 1
# with no output when there is none.
ge_android_ndk_host() {
  local ndk="$1"
  local d

  for d in "$ndk"/toolchains/llvm/prebuilt/*/; do
    [[ -x "$d/bin/clang" ]] || continue
    d="${d%/}"
    printf '%s\n' "${d##*/}"
    return 0
  done

  return 1
}

# ge_android_ndk_explain_unusable <label> <path>
#
# Diagnostic for a path that was resolved but does not exist, which is what a
# colour-contaminated capture looks like from the inside. Printing it through
# %q makes the escape bytes visible instead of letting the terminal eat them.
ge_android_ndk_explain_unusable() {
  local label="$1" path="$2"
  echo "error: $label resolved to a path that does not exist:" >&2
  printf '  %q\n' "$path" >&2
  echo "  If the path above contains escape bytes (\\E[...), something in this" >&2
  echo "  environment is colourising output that ge is reading as a path." >&2
  echo "  Report it against 🎯T186; ge must not be parsing that output at all." >&2
}
