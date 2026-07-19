#!/usr/bin/env bash
# Prebuild static libs for one of ge's supported platforms.
#
# Usage:
#   tools/prebuild.sh [--debug] <platform>
#
# Where <platform> is one of:
#   ios-arm64             — iOS arm64 device (iphoneos SDK, Metal backend)
#   ios-arm64-simulator   — iOS arm64 simulator (iphonesimulator SDK)
#   android-arm64         — Android arm64 (NDK clang, Vulkan + GLES backends)
#
# Always cooks *every* archive in the tree together (vendor + libge). There is
# no partial refresh: a mixed cook (new libge + old libliteparser) caused the
# 2026-07 Android sqldeep SIGSEGV. Output goes to prebuilt/<platform>/ or
# prebuilt/<platform>-debug/ plus manifest.json + cook.json (hashes of every
# .a). Consumers refuse to link if cook.json does not match the archives.
#
# --debug builds *every* archive in that tree with DWARF + the same -O2 as
# release (plus -fno-omit-frame-pointer). O0 is intentionally NOT used: it
# can hide optimised-only bugs (sqldeep SEGV was one). For a pure -O0 cook
# set GE_PREBUILD_OPT=0. Release and debug trees are separate so optimised
# no-DWARF archives are not clobbered. Android Debug packages select the
# -debug tree via cmake/android-arm64.cmake.
#
# Per-platform differences are confined to the case statement at the top
# (toolchain, flags, output dir, ge source list). The compile
# body below is identical across platforms — same vendor lib calls, same
# manifest writer.
#
# Refresh workflow: bump a vendor submodule SHA or edit a ge source, re-run
# this script for each affected platform + tools/lift-headers.sh, commit.
# See docs/vendor-prebuilds.md.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

# Keep static archives stable across rebuilds when object contents don't change
# (Apple libtool honours ZERO_AR_DATE; llvm-ar is deterministic by default but
# the env var is harmless there).
export ZERO_AR_DATE=1

DEBUG=0
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    # Historical flag: partial cooks are forbidden (mixed libge/vendor = SIGSEGV).
    # Accept and ignore so old scripts still run a full coherent cook.
    --libge-only)
      echo "ge: --libge-only is removed; always cooking full tree (vendor + libge)." >&2
      shift
      ;;
    --debug) DEBUG=1; shift ;;
    -h|--help)
      echo "usage: $0 [--debug] <platform>" >&2
      echo "  platform: ios-arm64 | ios-arm64-simulator | android-arm64 | web-wasm" >&2
      echo "  Always rebuilds every archive in the tree (no partial cooks)." >&2
      echo "  --debug:  write prebuilt/<platform>-debug/ with -g -O2 (symbols+opt)" >&2
      echo "            GE_PREBUILD_OPT=0|1|2|3|s|g|fast  (default 2)" >&2
      exit 0
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      exit 1
      ;;
    *) ARGS+=("$1"); shift ;;
  esac
done

if [[ ${#ARGS[@]} -ne 1 ]]; then
  echo "usage: $0 [--debug] <platform>" >&2
  echo "  platform: ios-arm64 | ios-arm64-simulator | android-arm64 | web-wasm" >&2
  exit 1
fi
PLATFORM="${ARGS[0]}"

# Env override keeps older callers working (GE_PREBUILD_DEBUG=1 tools/prebuild.sh …).
if [[ "${GE_PREBUILD_DEBUG:-}" == "1" ]]; then
  DEBUG=1
fi

GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

# Debug and release prebuilts are separate trees so a debug cook never
# overwrites the optimised archives consumers ship / CI verifies.
if [[ "$DEBUG" -eq 1 ]]; then
  PREBUILT_KEY="${PLATFORM}-debug"
else
  PREBUILT_KEY="$PLATFORM"
fi
OUT_DIR="prebuilt/$PREBUILT_KEY"
OBJ_DIR="build/prebuilt/$PREBUILT_KEY"
# Wipe stale .d files so write-manifest.py only sees deps from this run.
rm -rf "$OBJ_DIR"
mkdir -p "$OUT_DIR" "$OBJ_DIR"

# cook.json: single co-build identity for every .a in this tree. Consumers
# refuse to link unless every archive hash matches (tools/verify-cook.py —
# used by cmake/android-arm64.cmake, iOS Xcode script phase, ensure-prebuilt).
write_cook_json() {
  local flags_joined="$1"
  python3 - "$OUT_DIR/cook.json" "$PREBUILT_KEY" "$DEBUG" "$OPT_FLAG" "$flags_joined" <<'PY'
import hashlib, json, sys, time
from pathlib import Path
path, key, debug, opt, flags = sys.argv[1:6]
out = Path(path).parent
# Keys are stems (ge, liteparser) — no ".a", so CMake string(JSON) is unambiguous.
archives = {}
for a in sorted(out.glob("*.a")):
    h = hashlib.sha256()
    with open(a, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    archives[a.stem.removeprefix("lib")] = h.hexdigest()
doc = {
    "version": 1,
    "prebuilt_key": key,
    "config": "debug" if debug == "1" else "release",
    "opt": opt,
    "flags": flags,
    "flags_sha256": hashlib.sha256(flags.encode()).hexdigest(),
    "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "archives": archives,
}
with open(path, "w") as f:
    json.dump(doc, f, indent=2)
    f.write("\n")
print(f"wrote {path} ({len(archives)} archives)")
PY
}

# ─── platform-specific setup ──────────────────────────────────────────────
# Everything that differs across platforms lives here. The compile body
# below uses these variables and doesn't branch further.
#
# Optimisation level: release defaults to -O2. --debug defaults to -O2 as
# well (plus -g) so Heisenbugs that only fire under opt still reproduce in
# Android Studio. Override: GE_PREBUILD_OPT=0 for a true no-opt cook.
OPT_LEVEL="${GE_PREBUILD_OPT:-2}"
case "$OPT_LEVEL" in
  0|1|2|3|s|g) OPT_FLAG="-O${OPT_LEVEL}" ;;
  fast)        OPT_FLAG="-Ofast" ;;
  *)
    echo "error: GE_PREBUILD_OPT must be 0|1|2|3|s|g|fast (got '$OPT_LEVEL')" >&2
    exit 1
    ;;
esac
if [[ "$DEBUG" -eq 1 ]]; then
  # DWARF + frame pointers on every archive (libge + vendor) so AS/lldb can
  # step into sqldeep, liteparser, box2d, sqlite while still matching release
  # optimisation unless GE_PREBUILD_OPT overrides.
  COMMON_FLAGS=(-g "$OPT_FLAG" -fno-omit-frame-pointer -fvisibility=hidden -fno-color-diagnostics)
else
  COMMON_FLAGS=("$OPT_FLAG" -fvisibility=hidden -fno-color-diagnostics)
fi

GE_PLATFORM_DEFINE=
GE_PLATFORM_SOURCES=()
SDL_STUB_NEEDED=false   # Android needs a stub SDL_revision.h; Apple gets real one from headers/sdl3.

case "$PLATFORM" in
  ios-arm64|ios-arm64-simulator)
    if [[ "$PLATFORM" == "ios-arm64" ]]; then
      SDK=iphoneos
      COMMON_FLAGS+=(-arch arm64 -miphoneos-version-min=16.3)
    else
      SDK=iphonesimulator
      # Apple Silicon sim ABI: arm64 host but -simulator triple variant.
      COMMON_FLAGS+=(-target arm64-apple-ios16.3-simulator)
    fi
    SDK_PATH="$(xcrun --sdk "$SDK" --show-sdk-path)"
    CC="$(xcrun --sdk "$SDK" -f clang)"
    CXX="$(xcrun --sdk "$SDK" -f clang++)"
    AR="$(xcrun --sdk "$SDK" -f libtool)"
    AR_FLAGS=(-static -o)
    COMMON_FLAGS+=(-isysroot "$SDK_PATH")
    GE_PLATFORM_DEFINE=-DGE_IOS
    GE_MANIFEST_TARGET=print-direct-ios
    ;;
  android-arm64)
    # ── NDK ABI pin (🎯T100) ──────────────────────────────────────────
    # libge.a bakes in references to libc++ runtime symbols emitted by the
    # NDK that compiles it; the consumer's Gradle build links those refs
    # against ITS NDK's libc++. Build with an NDK newer than the consumer's
    # and the older runtime won't provide the newer exception-ABI symbols
    # (e.g. __cxa_init_primary_exception,
    # std::exception_ptr::__from_native_exception_pointer) — the consumer
    # link dies with "undefined symbol". Rule: build with an NDK no newer
    # than the OLDEST NDK any consumer links with. Pin to that major here;
    # bump GE_ANDROID_NDK_MAJOR in lockstep once the whole consumer fleet
    # moves up. Keep in sync with ANDROID_NDK_MAJOR_PIN in
    # tools/verify-prebuilds.py.
    GE_ANDROID_NDK_MAJOR="${GE_ANDROID_NDK_MAJOR:-27}"
    if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
      # Explicit override wins, but warn if it is newer than the pin — that
      # is precisely the skew that produced 🎯T100.
      NDK="$ANDROID_NDK_HOME"
      ovr_major="$(basename "$NDK" | cut -d. -f1)"
      if [[ "$ovr_major" =~ ^[0-9]+$ && "$ovr_major" -gt "$GE_ANDROID_NDK_MAJOR" ]]; then
        echo "warning: ANDROID_NDK_HOME points at NDK r$ovr_major, newer than the r$GE_ANDROID_NDK_MAJOR ABI pin." >&2
        echo "         Consumers on r$GE_ANDROID_NDK_MAJOR may fail to link newer libc++ symbols (🎯T100)." >&2
        echo "         Unset ANDROID_NDK_HOME to auto-select r$GE_ANDROID_NDK_MAJOR, or bump GE_ANDROID_NDK_MAJOR." >&2
      fi
    elif [[ -d "$HOME/Library/Android/sdk/ndk" ]]; then
      # Highest installed NDK whose MAJOR matches the pin, so an
      # incidentally-newer NDK on the build machine can't leak forward-
      # incompatible symbols into the shipped prebuilt.
      NDK="$(ls -d "$HOME"/Library/Android/sdk/ndk/"$GE_ANDROID_NDK_MAJOR".*/ 2>/dev/null | sort -V | tail -1)"
      NDK="${NDK%/}"
    fi
    if [[ -z "${NDK:-}" || ! -d "$NDK" ]]; then
      echo "error: Android NDK r$GE_ANDROID_NDK_MAJOR not found." >&2
      echo "  Install it under ~/Library/Android/sdk/ndk/ (e.g. r$GE_ANDROID_NDK_MAJOR.x), or set" >&2
      echo "  ANDROID_NDK_HOME to an r$GE_ANDROID_NDK_MAJOR toolchain. The prebuilt's libc++ ABI" >&2
      echo "  must be no newer than any consumer's NDK (🎯T100)." >&2
      exit 1
    fi
    # Pin write-manifest.py (called below) to the SAME NDK so the manifest
    # records exactly what built the archive — no independent re-derivation
    # that could pick a different (newer) NDK than this build used.
    export ANDROID_NDK_HOME="$NDK"
    NDK_HOST="$(ls "$NDK/toolchains/llvm/prebuilt" 2>/dev/null | head -1)"
    if [[ -z "$NDK_HOST" ]]; then
      echo "error: no llvm host triple under $NDK/toolchains/llvm/prebuilt." >&2
      exit 1
    fi
    NDK_BIN="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin"
    NDK_SYSROOT="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/sysroot"
    CC="$NDK_BIN/clang"
    CXX="$NDK_BIN/clang++"
    AR="$NDK_BIN/llvm-ar"
    AR_FLAGS=(rcs)
    COMMON_FLAGS+=(
      --target=aarch64-none-linux-android26
      --sysroot="$NDK_SYSROOT"
      -fPIC -DANDROID -D__ANDROID__
    )
    GE_PLATFORM_DEFINE=-DGE_ANDROID
    GE_MANIFEST_TARGET=print-direct-android
    SDL_STUB_NEEDED=true
    ;;
  web-wasm)
    # 🎯T157 Emscripten/wasm (WebGL2 via SOKOL_GLES3). Single wasm32 arch;
    # toolchain from PATH (brew install emscripten, or an activated emsdk).
    if ! command -v em++ >/dev/null 2>&1; then
      echo "error: em++ not found — install Emscripten (brew install emscripten" >&2
      echo "  or https://emscripten.org/docs/getting_started/downloads.html)." >&2
      exit 1
    fi
    CC=emcc
    CXX=em++
    AR=emar
    AR_FLAGS=(rcs)
    # emcc disables C++ exception catching by default; ge relies on
    # exceptions (guardCallback, resolveFont, sqlpipe). JS-based exceptions
    # (-fexceptions), NOT -fwasm-exceptions: the web run loop suspends via
    # Asyncify (SessionHost.mm ge_web_await_frame), and Asyncify cannot
    # instrument functions holding wasm-EH pads ("Aborted(invalid state)"
    # at runtime). Must match the consumer's compile+link flags
    # (cmake/web-wasm.cmake keeps them in sync).
    # -msimd128 -msse2: box2d's __EMSCRIPTEN__ branch selects its SSE2 SIMD
    # path (emmintrin.h over wasm SIMD — box2d's own CMake passes the same
    # pair for Emscripten); wasm SIMD is likewise universal in 2026.
    COMMON_FLAGS+=(-fexceptions -msimd128 -msse2)
    GE_PLATFORM_DEFINE=-DGE_WEB
    GE_MANIFEST_TARGET=print-direct-web
    SDL_STUB_NEEDED=true
    ;;
  *)
    echo "error: unsupported platform '$PLATFORM'" >&2
    echo "  supported: ios-arm64 ios-arm64-simulator android-arm64 web-wasm" >&2
    exit 1
    ;;
esac

echo "── platform: $PLATFORM ──"
echo "   prebuilt: $OUT_DIR"
echo "   config: $([ "$DEBUG" -eq 1 ] && echo debug || echo release)  ${OPT_FLAG}$([ "$DEBUG" -eq 1 ] && echo ' -g' || true)"
echo "   mode: full (all archives; partial cooks disabled)"
echo "   CC:  $CC"
echo "   AR:  $AR"

PREBUILD_JOBS="${GE_PREBUILD_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
if ! [[ "$PREBUILD_JOBS" =~ ^[0-9]+$ ]] || [[ "$PREBUILD_JOBS" -lt 1 ]]; then
  PREBUILD_JOBS=1
fi
echo "   jobs: $PREBUILD_JOBS"

# Android needs a stub SDL_revision.h (the real one is generated by SDL's
# CMake build at consumer-side configure time). On Apple the real header
# lives in headers/sdl3/include and gets picked up by GE_INCLUDES below.
SDL_STUB_INC=
if $SDL_STUB_NEEDED; then
  SDL_STUB_INC="$OBJ_DIR/sdl3-stub"
  mkdir -p "$SDL_STUB_INC/SDL3"
  cat >"$SDL_STUB_INC/SDL3/SDL_revision.h" <<'EOF'
/* Stub SDL_revision.h for ge prebuild. Consumer's SDL build generates a
 * real one with version metadata; ge never reads this constant. */
#define SDL_REVISION "prebuilt-stub"
EOF
fi

C_STD=(-std=c11)
if [[ "$PLATFORM" == "web-wasm" ]]; then
  # Strict c11 hides POSIX names (CLOCK_MONOTONIC in box2d's timer.c) in
  # Emscripten's musl headers; bionic/Darwin expose them regardless.
  C_STD=(-std=gnu11)
fi
CXX_STD=(-std=c++20)
CXX17_STD=(-std=c++17)
DEPFLAGS=(-MMD -MP)

VENDOR="vendor/github.com"
BOX2D_DIR="$VENDOR/erincatto/box2d"
LUNASVG_DIR="$VENDOR/sammycage/lunasvg"
PLUTOVG_DIR="$LUNASVG_DIR/plutovg"
# deepparser (marcelocantos) — sqlpipe ≥0.29 needs sqldeep AST extensions.
LITEPARSER_DIR="$VENDOR/marcelocantos/deepparser/src"

# Sanity check — the vendored submodules this prebuild compiles must be
# initialised. (sokol_gfx is a vendored single header, not a submodule.)
for d in "$BOX2D_DIR" "$LUNASVG_DIR"; do
  if [[ ! -f "$d/.git" && ! -d "$d/.git" ]]; then
    echo "error: $d is not initialised. Run: git submodule update --init --recursive" >&2
    exit 1
  fi
done

# ─── build helpers ────────────────────────────────────────────────────────

compile_c() {
  local out="$1" src="$2"; shift 2
  mkdir -p "$(dirname "$out")"
  "$CC" "${COMMON_FLAGS[@]}" "${C_STD[@]}" "${DEPFLAGS[@]}" -MF "$out.d" \
    "$@" -c "$src" -o "$out"
}

compile_cxx() {
  local out="$1" src="$2"; shift 2
  mkdir -p "$(dirname "$out")"
  "$CXX" "${COMMON_FLAGS[@]}" "${CXX_STD[@]}" "${DEPFLAGS[@]}" -MF "$out.d" \
    "$@" -c "$src" -o "$out"
}

compile_cxx17() {
  local out="$1" src="$2"; shift 2
  mkdir -p "$(dirname "$out")"
  "$CXX" "${COMMON_FLAGS[@]}" "${CXX17_STD[@]}" "${DEPFLAGS[@]}" -MF "$out.d" \
    "$@" -c "$src" -o "$out"
}

archive() {
  local out="$1"; shift
  rm -f "$out"
  "$AR" "${AR_FLAGS[@]}" "$out" "$@"
}

JOB_PIDS=()
JOB_FAILED=0

run_job() {
  while [[ ${#JOB_PIDS[@]} -ge "$PREBUILD_JOBS" ]]; do
    wait "${JOB_PIDS[0]}" || JOB_FAILED=1
    JOB_PIDS=("${JOB_PIDS[@]:1}")
  done
  "$@" &
  JOB_PIDS+=("$!")
}

wait_jobs() {
  local pid
  for pid in "${JOB_PIDS[@]}"; do
    wait "$pid" || JOB_FAILED=1
  done
  JOB_PIDS=()
  if [[ "$JOB_FAILED" -ne 0 ]]; then
    echo "error: one or more compile jobs failed" >&2
    exit 1
  fi
}

# T38: bgfx / bx / bimg vendor builds retired. ge now uses sokol_gfx
# (vendored single-header at vendor/github.com/floooh/sokol/sokol_gfx.h),
# included via SOKOL_IMPL by src/SokolContext.mm on Apple and
# src/SokolContext_android.cpp on Android — no separate static library
# to produce or archive.

# ─── box2d ──────────────────────────────────────────────────────────────
echo "── box2d ──"
BOX2D_OBJS=()
for src in "$BOX2D_DIR"/src/*.c; do
  obj="$OBJ_DIR/box2d/$(basename "${src%.c}").o"
  run_job compile_c "$obj" "$src" \
    -I"$BOX2D_DIR/include" -I"$BOX2D_DIR/src" \
    -Wno-everything
  BOX2D_OBJS+=("$obj")
done
wait_jobs
archive "$OUT_DIR/libbox2d.a" "${BOX2D_OBJS[@]}"

# ─── lunasvg ────────────────────────────────────────────────────────────
echo "── lunasvg ──"
LUNASVG_OBJS=()
for src in "$LUNASVG_DIR"/source/*.cpp; do
  obj="$OBJ_DIR/lunasvg/$(basename "${src%.cpp}").o"
  run_job compile_cxx17 "$obj" "$src" \
    -I"$LUNASVG_DIR/include" -I"$LUNASVG_DIR/source" -I"$PLUTOVG_DIR/include" \
    -DLUNASVG_BUILD -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC \
    -Wno-everything
  LUNASVG_OBJS+=("$obj")
done
wait_jobs
archive "$OUT_DIR/liblunasvg_ge.a" "${LUNASVG_OBJS[@]}"

# ─── plutovg ────────────────────────────────────────────────────────────
echo "── plutovg ──"
PLUTOVG_OBJS=()
for src in "$PLUTOVG_DIR"/source/*.c; do
  obj="$OBJ_DIR/plutovg/$(basename "${src%.c}").o"
  run_job compile_c "$obj" "$src" \
    -I"$PLUTOVG_DIR/include" -I"$PLUTOVG_DIR/source" \
    -DPLUTOVG_BUILD -DPLUTOVG_BUILD_STATIC \
    -Wno-everything
  PLUTOVG_OBJS+=("$obj")
done
wait_jobs
archive "$OUT_DIR/libplutovg_ge.a" "${PLUTOVG_OBJS[@]}"

# ─── liteparser ─────────────────────────────────────────────────────────
echo "── liteparser ──"
LITEPARSER_OBJS=()
for src in "$LITEPARSER_DIR/arena.c" "$LITEPARSER_DIR/liteparser.c" \
           "$LITEPARSER_DIR/lp_tokenize.c" "$LITEPARSER_DIR/lp_unparse.c" \
           "$LITEPARSER_DIR/parse.c"; do
  obj="$OBJ_DIR/liteparser/$(basename "${src%.c}").o"
  run_job compile_c "$obj" "$src" -I"$LITEPARSER_DIR" -Wno-everything
  LITEPARSER_OBJS+=("$obj")
done
wait_jobs
archive "$OUT_DIR/libliteparser.a" "${LITEPARSER_OBJS[@]}"

# ─── sqlite3 ────────────────────────────────────────────────────────────
echo "── sqlite3 ──"
# 🎯T157 web: no-op sqlite's xSync. fsync maps to the suspending
# __wasi_fd_sync import under Asyncify, and sqlite fsyncs beneath C++
# try scopes (invoke trampolines) — an unsuspendable position. Durability
# on web comes from the explicit FS.syncfs in tools/web-template/pre.js.
SQLITE_PLATFORM_DEFINES=()
if [[ "$PLATFORM" == "web-wasm" ]]; then
  SQLITE_PLATFORM_DEFINES=(-DSQLITE_NO_SYNC)
fi
SQLITE_OBJ="$OBJ_DIR/sqlite3/sqlite3.o"
run_job compile_c "$SQLITE_OBJ" "vendor/src/sqlite3.c" \
  "${SQLITE_PLATFORM_DEFINES[@]}" \
  -I vendor/include \
  -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_DESERIALIZE \
  -Wno-everything
wait_jobs
archive "$OUT_DIR/libsqlite3_ge.a" "$SQLITE_OBJ"

# ─── lz4 ────────────────────────────────────────────────────────────────
echo "── lz4 ──"
LZ4_OBJ="$OBJ_DIR/lz4/lz4.o"
run_job compile_c "$LZ4_OBJ" "vendor/src/lz4.c" -I vendor/include -Wno-everything
wait_jobs
archive "$OUT_DIR/liblz4_ge.a" "$LZ4_OBJ"

# ─── libge (engine itself) ────────────────────────────────────────────────
echo "── ge ──"

# Header search paths match the iOS xcodeproj-gem path
# (tools/ios-build/build_project.rb#header_search_paths) post-T71 header
# lifting — so the same #include landscape works on both platforms.
GE_INCLUDES=(
  -I include
  -I vendor/include
  -I vendor/github.com/floooh/sokol
  -I tools/sokol-dispatch/generated   # 🎯T107 dispatch shim header (Android libge)
  -I tools/vkprobe                    # 🎯T107 ge_vkprobe.h (backend selection)
  -I headers/spdlog/include
  -I headers/asio/include
  -I headers/sdl3/include
  -I vendor/sdl3/include
  -I headers/liteparser/include
  -I headers/lunasvg/include
  -I headers/plutovg/include
  -I headers/box2d/include
)

# T38/🎯T97/drawSolid: generate ge_sprite.h + ge_debug.h + ge_solid.h via
# sokol-shdc so sprite.cpp / debug.cpp / solid.cpp can #include them.
GE_SHADER_OUT_DIR="$OBJ_DIR/ge-shaders"
mkdir -p "$GE_SHADER_OUT_DIR"
SOKOL_SHDC="vendor/github.com/floooh/sokol-tools-bin/bin/osx_arm64/sokol-shdc"
# metal_sim (🎯T84): the iOS Simulator reports SG_BACKEND_METAL_SIMULATOR,
# so the headers need a metal_sim slot or sg_make_shader aborts there.
# Keep this lang list in sync with Module.mk's ge/SOKOL_SHDC_LANGS.
# 🎯T107: spirv_vk emits the SOKOL_VULKAN variant of ge's internal shaders
# (ge_sprite.h / ge_debug.h / ge_solid.h). Without it, *_shader_desc returns
# NULL on a Vulkan-selected device → sg_make_shader(NULL) aborts.
GE_SHDC_LANGS="metal_macos:metal_ios:metal_sim:glsl300es:spirv_vk"
for sh in ge_sprite ge_debug ge_solid; do
  "$SOKOL_SHDC" -i "src/render/shaders/$sh.glsl" \
                -o "$GE_SHADER_OUT_DIR/$sh.h" \
                -l "$GE_SHDC_LANGS" -f sokol
done
GE_INCLUDES+=(-I "$GE_SHADER_OUT_DIR")
if [[ -n "$SDL_STUB_INC" ]]; then
  GE_INCLUDES+=(-I "$SDL_STUB_INC")
fi

GE_DEFINES=(
  -DASIO_STANDALONE
  "$GE_PLATFORM_DEFINE"
  -DGE_DIRECT_ONLY
  -DSQLITE_ENABLE_SESSION
  -DSQLITE_ENABLE_PREUPDATE_HOOK
  -DSQLITE_ENABLE_DESERIALIZE
  -DLUNASVG_BUILD_STATIC
  -DPLUTOVG_BUILD_STATIC
)

compile_ge_source() {
  local src="$1" obj="$2"
  local ext="${src##*.}"
  mkdir -p "$(dirname "$obj")"
  case "$ext" in
    cpp|mm)
      # On Apple .mm is ObjC++; on Android and web (no ObjC) treat .mm as
      # plain C++.
      local lang_flag=()
      if [[ "$ext" == "mm" && ( "$PLATFORM" == "android-arm64" || "$PLATFORM" == "web-wasm" ) ]]; then
        lang_flag=(-x c++)
      fi
      # 🎯T157 web: the run-loop TU must contain no JS-exception invoke
      # trampolines — Asyncify cannot suspend beneath a JS frame, and the
      # ge_web_await_frame suspend lives here. Last flag wins in clang;
      # guardCallback / renderBatch degrade via __cpp_exceptions.
      if [[ "$PLATFORM" == "web-wasm" && "$src" == "src/SessionHost.mm" ]]; then
        # SPDLOG_NO_EXCEPTIONS: spdlog's inline headers otherwise contain
        # throw/try, which -fno-exceptions rejects.
        lang_flag+=(-fno-exceptions -DSPDLOG_NO_EXCEPTIONS)
      fi
      "$CXX" "${COMMON_FLAGS[@]}" "${CXX_STD[@]}" "${DEPFLAGS[@]}" -MF "$obj.d" \
        "${lang_flag[@]}" "${GE_INCLUDES[@]}" "${GE_DEFINES[@]}" \
        -c "$src" -o "$obj"
      ;;
    c)
      "$CC" "${COMMON_FLAGS[@]}" "${C_STD[@]}" "${DEPFLAGS[@]}" -MF "$obj.d" \
        "${GE_INCLUDES[@]}" "${GE_DEFINES[@]}" \
        -c "$src" -o "$obj"
      ;;
    *)
      echo "error: unhandled source extension: $src" >&2
      return 1
      ;;
  esac
}

# ge source list — extracted from the canonical manifest at
# tools/ge-sources.mk so this script and Module.mk stay in sync.
# GE_MANIFEST_TARGET was selected per-platform above.
mapfile -t GE_SOURCES < <(make -s -f tools/ge-sources.mk "$GE_MANIFEST_TARGET")
if [[ ${#GE_SOURCES[@]} -eq 0 ]]; then
  echo "error: tools/ge-sources.mk produced no sources for $GE_MANIFEST_TARGET" >&2
  exit 1
fi

GE_OBJS=()
for src in "${GE_SOURCES[@]}"; do
  rel="${src%.*}"
  obj="$OBJ_DIR/ge/${rel//\//_}.o"
  run_job compile_ge_source "$src" "$obj"
  GE_OBJS+=("$obj")
done
wait_jobs
archive "$OUT_DIR/libge.a" "${GE_OBJS[@]}"

# ─── manifest + cook identity ─────────────────────────────────────────────
echo ""
echo "── manifest ──"
python3 tools/write-manifest.py --platform "$PREBUILT_KEY"
write_cook_json "${COMMON_FLAGS[*]}"

# ─── summary ──────────────────────────────────────────────────────────────
echo ""
echo "── prebuilt $PREBUILT_KEY libs ──"
ls -lh "$OUT_DIR"/*.a | awk '{printf "  %-30s %s\n", $NF, $5}'
echo ""
echo "Total: $(du -sh "$OUT_DIR" | cut -f1)"
