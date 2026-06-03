#!/usr/bin/env bash
# Prebuild static libs for one of ge's supported platforms.
#
# Usage:
#   tools/prebuild.sh <platform>
#
# Where <platform> is one of:
#   ios-arm64             — iOS arm64 device (iphoneos SDK, Metal backend)
#   ios-arm64-simulator   — iOS arm64 simulator (iphonesimulator SDK)
#   android-arm64         — Android arm64 (NDK clang, Vulkan + GLES backends)
#
# Outputs ten static libs (bgfx, bx, bimg, box2d, lunasvg_ge, plutovg_ge,
# sqlite3_ge, lz4_ge, liteparser, ge) into prebuilt/<platform>/, plus a
# manifest.json for staleness detection.
#
# Per-platform differences are confined to the case statement at the top
# (toolchain, flags, output dir, bgfx defines, ge source list). The compile
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

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <platform>"           >&2
  echo "  platform: ios-arm64 | ios-arm64-simulator | android-arm64" >&2
  exit 1
fi
PLATFORM="$1"

GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

OUT_DIR="prebuilt/$PLATFORM"
OBJ_DIR="build/prebuilt/$PLATFORM"
# Wipe stale .d files so write-manifest.py only sees deps from this run.
rm -rf "$OBJ_DIR"
mkdir -p "$OUT_DIR" "$OBJ_DIR"

# ─── platform-specific setup ──────────────────────────────────────────────
# Everything that differs across platforms lives here. The compile body
# below uses these variables and doesn't branch further.
COMMON_FLAGS=(-O2 -fvisibility=hidden -fno-color-diagnostics)
BGFX_AS_OBJC=false
BGFX_DEFINES=()
BX_COMPAT_DIR=
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
    BGFX_AS_OBJC=true   # Metal backend needs ObjC++ for the amalgamated unit.
    BGFX_DEFINES=(-DBGFX_CONFIG_RENDERER_METAL=1)
    BX_COMPAT_DIR=ios
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
    BGFX_DEFINES=(
      -DBGFX_CONFIG_MULTITHREADED=0
      -DBGFX_CONFIG_RENDERER_VULKAN=1
      -DBGFX_CONFIG_RENDERER_OPENGLES=31
    )
    BX_COMPAT_DIR=linux
    GE_PLATFORM_DEFINE=-DGE_ANDROID
    GE_MANIFEST_TARGET=print-direct-android
    SDL_STUB_NEEDED=true
    ;;
  *)
    echo "error: unsupported platform '$PLATFORM'" >&2
    echo "  supported: ios-arm64 ios-arm64-simulator android-arm64" >&2
    exit 1
    ;;
esac

echo "── platform: $PLATFORM ──"
echo "   CC:  $CC"
echo "   AR:  $AR"

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
CXX_STD=(-std=c++20)
CXX17_STD=(-std=c++17)
DEPFLAGS=(-MMD -MP)

VENDOR="vendor/github.com"
BX_DIR="$VENDOR/bkaradzic/bx"
BIMG_DIR="$VENDOR/bkaradzic/bimg"
BGFX_DIR="$VENDOR/bkaradzic/bgfx"
BOX2D_DIR="$VENDOR/erincatto/box2d"
LUNASVG_DIR="$VENDOR/sammycage/lunasvg"
PLUTOVG_DIR="$LUNASVG_DIR/plutovg"
LITEPARSER_DIR="$VENDOR/sqliteai/liteparser/src"

# Sanity check — submodules must be initialised. T38: bx/bimg/bgfx
# no longer required (ge uses sokol_gfx, vendored as a single header).
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

# T38: bgfx / bx / bimg vendor builds retired. ge now uses sokol_gfx
# (vendored single-header at vendor/github.com/floooh/sokol/sokol_gfx.h),
# included via SOKOL_IMPL by src/SokolContext.mm on Apple and
# src/SokolContext_android.cpp on Android — no separate static library
# to produce or archive.

# ─── box2d ────────────────────────────────────────────────────────────────
echo "── box2d ──"
BOX2D_OBJS=()
for src in "$BOX2D_DIR"/src/*.c; do
  obj="$OBJ_DIR/box2d/$(basename "${src%.c}").o"
  compile_c "$obj" "$src" \
    -I"$BOX2D_DIR/include" -I"$BOX2D_DIR/src" \
    -Wno-everything
  BOX2D_OBJS+=("$obj")
done
archive "$OUT_DIR/libbox2d.a" "${BOX2D_OBJS[@]}"

# ─── lunasvg ──────────────────────────────────────────────────────────────
echo "── lunasvg ──"
LUNASVG_OBJS=()
for src in "$LUNASVG_DIR"/source/*.cpp; do
  obj="$OBJ_DIR/lunasvg/$(basename "${src%.cpp}").o"
  compile_cxx17 "$obj" "$src" \
    -I"$LUNASVG_DIR/include" -I"$LUNASVG_DIR/source" -I"$PLUTOVG_DIR/include" \
    -DLUNASVG_BUILD -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC \
    -Wno-everything
  LUNASVG_OBJS+=("$obj")
done
archive "$OUT_DIR/liblunasvg_ge.a" "${LUNASVG_OBJS[@]}"

# ─── plutovg ──────────────────────────────────────────────────────────────
echo "── plutovg ──"
PLUTOVG_OBJS=()
for src in "$PLUTOVG_DIR"/source/*.c; do
  obj="$OBJ_DIR/plutovg/$(basename "${src%.c}").o"
  compile_c "$obj" "$src" \
    -I"$PLUTOVG_DIR/include" -I"$PLUTOVG_DIR/source" \
    -DPLUTOVG_BUILD -DPLUTOVG_BUILD_STATIC \
    -Wno-everything
  PLUTOVG_OBJS+=("$obj")
done
archive "$OUT_DIR/libplutovg_ge.a" "${PLUTOVG_OBJS[@]}"

# ─── liteparser ───────────────────────────────────────────────────────────
echo "── liteparser ──"
LITEPARSER_OBJS=()
for src in "$LITEPARSER_DIR/arena.c" "$LITEPARSER_DIR/liteparser.c" \
           "$LITEPARSER_DIR/lp_tokenize.c" "$LITEPARSER_DIR/lp_unparse.c" \
           "$LITEPARSER_DIR/parse.c"; do
  obj="$OBJ_DIR/liteparser/$(basename "${src%.c}").o"
  compile_c "$obj" "$src" -I"$LITEPARSER_DIR" -Wno-everything
  LITEPARSER_OBJS+=("$obj")
done
archive "$OUT_DIR/libliteparser.a" "${LITEPARSER_OBJS[@]}"

# ─── sqlite3 ──────────────────────────────────────────────────────────────
echo "── sqlite3 ──"
SQLITE_OBJ="$OBJ_DIR/sqlite3/sqlite3.o"
compile_c "$SQLITE_OBJ" "vendor/src/sqlite3.c" \
  -I vendor/include \
  -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_DESERIALIZE \
  -Wno-everything
archive "$OUT_DIR/libsqlite3_ge.a" "$SQLITE_OBJ"

# ─── lz4 ──────────────────────────────────────────────────────────────────
echo "── lz4 ──"
LZ4_OBJ="$OBJ_DIR/lz4/lz4.o"
compile_c "$LZ4_OBJ" "vendor/src/lz4.c" -I vendor/include -Wno-everything
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
  -I headers/spdlog/include
  -I headers/asio/include
  -I headers/sdl3/include
  -I vendor/sdl3/include
  -I headers/liteparser/include
  -I headers/lunasvg/include
  -I headers/plutovg/include
  -I headers/box2d/include
)

# T38/🎯T97: generate ge_sprite.h + ge_debug.h via sokol-shdc so src/sprite.cpp
# and src/debug.cpp can #include them.
GE_SHADER_OUT_DIR="$OBJ_DIR/ge-shaders"
mkdir -p "$GE_SHADER_OUT_DIR"
SOKOL_SHDC="vendor/github.com/floooh/sokol-tools-bin/bin/osx_arm64/sokol-shdc"
# metal_sim (🎯T84): the iOS Simulator reports SG_BACKEND_METAL_SIMULATOR,
# so the headers need a metal_sim slot or sg_make_shader aborts there.
# Keep this lang list in sync with Module.mk's ge/SOKOL_SHDC_LANGS.
GE_SHDC_LANGS="metal_macos:metal_ios:metal_sim:glsl300es"
for sh in ge_sprite ge_debug; do
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
  ext="${src##*.}"
  rel="${src%.*}"
  obj="$OBJ_DIR/ge/${rel//\//_}.o"
  mkdir -p "$(dirname "$obj")"
  case "$ext" in
    cpp|mm)
      # On Apple .mm is ObjC++; on Android (no ObjC) treat .mm as plain C++.
      lang_flag=()
      if [[ "$ext" == "mm" && "$PLATFORM" == "android-arm64" ]]; then
        lang_flag=(-x c++)
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
      exit 1
      ;;
  esac
  GE_OBJS+=("$obj")
done
archive "$OUT_DIR/libge.a" "${GE_OBJS[@]}"

# ─── manifest ─────────────────────────────────────────────────────────────
echo ""
echo "── manifest ──"
python3 tools/write-manifest.py --platform "$PLATFORM"

# ─── summary ──────────────────────────────────────────────────────────────
echo ""
echo "── prebuilt $PLATFORM vendor libs ──"
ls -lh "$OUT_DIR"/*.a | awk '{printf "  %-30s %s\n", $NF, $5}'
echo ""
echo "Total: $(du -sh "$OUT_DIR" | cut -f1)"
