#!/usr/bin/env bash
# Prebuild Android arm64 static libs → ge/prebuilt/android-arm64/.
#
# Counterpart to tools/prebuild-vendor-ios-arm64.sh. Run from the ge repo
# root. Requires the Android NDK installed (ANDROID_NDK_HOME or
# auto-detected under ~/Library/Android/sdk/ndk/). Outputs ten static libs:
#   bgfx, bx, bimg, box2d, lunasvg_ge, plutovg_ge, sqlite3_ge, lz4_ge,
#   liteparser, **ge** (engine itself).
#
# The .a files are committed to ge/prebuilt/android-arm64/ via Git LFS.
# Consumer apps' Android CMakeLists declares each as STATIC IMPORTED and
# links them into the app's libmain.so — no ge source compiles in the
# consumer build (T73.1).
#
# The consumer-side IMPORTED declarations live in a hand-maintained CMake
# snippet at ge/cmake/android-arm64.cmake — this script only produces the
# .a files. The CMake snippet is short, stable, and reviewable; auto-
# generating it would re-introduce exactly the source-list drift class
# T73 set out to retire.
#
# SDL3 + SDL3_image + SDL3_ttf are NOT prebuilt by this script — they
# still come from add_subdirectory of the SDL submodules in the consumer
# CMakeLists. That's a separate prebuild scope (future T73 phase).
#
# Refresh workflow: bump submodule SHAs or edit ge sources, re-run this
# script + tools/lift-headers.sh, commit.
#
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

# ─── locate NDK ───────────────────────────────────────────────────────────
# Order: explicit env var, then the highest-version installed NDK under
# ~/Library/Android/sdk/ndk/. Fail loudly if neither finds one.
if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
  NDK="$ANDROID_NDK_HOME"
elif [[ -d "$HOME/Library/Android/sdk/ndk" ]]; then
  NDK="$(ls -d "$HOME"/Library/Android/sdk/ndk/*/ 2>/dev/null | sort -V | tail -1)"
  NDK="${NDK%/}"
fi
if [[ -z "${NDK:-}" || ! -d "$NDK" ]]; then
  echo "error: Android NDK not found. Set ANDROID_NDK_HOME or install one under ~/Library/Android/sdk/ndk/." >&2
  exit 1
fi
echo "── using NDK: $NDK ──"

# Detect host triple under prebuilt/. Apple Silicon ships as darwin-x86_64
# (Rosetta), Linux as linux-x86_64.
NDK_HOST="$(ls "$NDK/toolchains/llvm/prebuilt" 2>/dev/null | head -1)"
if [[ -z "$NDK_HOST" ]]; then
  echo "error: no llvm host triple under $NDK/toolchains/llvm/prebuilt." >&2
  exit 1
fi
NDK_BIN="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin"
NDK_SYSROOT="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/sysroot"

# Target: arm64-v8a, minimum API 26 (matches consumer build).
TARGET_TRIPLE="aarch64-none-linux-android26"
CC="$NDK_BIN/clang"
CXX="$NDK_BIN/clang++"
AR="$NDK_BIN/llvm-ar"

OUT_DIR="prebuilt/android-arm64"
OBJ_DIR="build/prebuilt/android-arm64"
rm -rf "$OBJ_DIR"
mkdir -p "$OUT_DIR" "$OBJ_DIR"

COMMON_FLAGS=(
  --target="$TARGET_TRIPLE"
  --sysroot="$NDK_SYSROOT"
  -O2
  -fvisibility=hidden
  -fPIC
  -fno-color-diagnostics
  -DANDROID
  -D__ANDROID__
)

C_STD=(-std=c11)
CXX_STD=(-std=c++20)
CXX17_STD=(-std=c++17)

VENDOR="vendor/github.com"
BX_DIR="$VENDOR/bkaradzic/bx"
BIMG_DIR="$VENDOR/bkaradzic/bimg"
BGFX_DIR="$VENDOR/bkaradzic/bgfx"
BOX2D_DIR="$VENDOR/erincatto/box2d"
LUNASVG_DIR="$VENDOR/sammycage/lunasvg"
PLUTOVG_DIR="$LUNASVG_DIR/plutovg"
LITEPARSER_DIR="$VENDOR/sqliteai/liteparser/src"
SDL_DIR="$VENDOR/libsdl-org/SDL"
SDL_IMAGE_DIR="$VENDOR/libsdl-org/SDL_image"
SDL_TTF_DIR="$VENDOR/libsdl-org/SDL_ttf"

# Sanity check — submodules must be initialised.
for d in "$BX_DIR" "$BIMG_DIR" "$BGFX_DIR" "$BOX2D_DIR" "$LUNASVG_DIR" "$SDL_DIR"; do
  if [[ ! -f "$d/.git" && ! -d "$d/.git" ]]; then
    echo "error: $d is not initialised. Run: git submodule update --init --recursive" >&2
    exit 1
  fi
done

DEPFLAGS=(-MMD -MP)

# SDL3 headers — consumer's add_subdirectory(SDL) generates a
# SDL_revision.h in its build tree at runtime, but for ge compilation we
# need the headers as-checked-in plus a minimal revision stub. The bare
# include path is enough for ge's call sites; SDL itself ships
# include-revision/SDL3/SDL_revision.h via its CMake.
SDL_REVISION_STUB_DIR="$OBJ_DIR/sdl3-stub/SDL3"
mkdir -p "$SDL_REVISION_STUB_DIR"
cat >"$SDL_REVISION_STUB_DIR/SDL_revision.h" <<'EOF'
/* Stub SDL_revision.h for ge prebuild. Consumer's SDL build generates a
 * real one with version metadata; ge never reads this constant. */
#define SDL_REVISION "prebuilt-stub"
EOF
SDL_STUB_INC="$OBJ_DIR/sdl3-stub"

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
  "$AR" rcs "$out" "$@"
}

# ─── bx (Android uses include/compat/linux per CMakeLists pattern) ────────
echo "── bx (amalgamated) ──"
BX_OBJ="$OBJ_DIR/bx/amalgamated.o"
compile_cxx "$BX_OBJ" "$BX_DIR/src/amalgamated.cpp" \
  -I"$BX_DIR/include" -I"$BX_DIR/include/compat/linux" -I"$BX_DIR/3rdparty" \
  -DBX_CONFIG_DEBUG=0
archive "$OUT_DIR/libbx.a" "$BX_OBJ"

# ─── bimg ─────────────────────────────────────────────────────────────────
echo "── bimg ──"
BIMG_OBJS=()
for src in "$BIMG_DIR/src/image.cpp" "$BIMG_DIR/src/image_gnf.cpp"; do
  obj="$OBJ_DIR/bimg/$(basename "${src%.cpp}").o"
  compile_cxx "$obj" "$src" \
    -I"$BIMG_DIR/include" -I"$BIMG_DIR/3rdparty" -I"$BIMG_DIR/3rdparty/astc-encoder/include" \
    -I"$BX_DIR/include" \
    -DBX_CONFIG_DEBUG=0 -DBIMG_CONFIG_DECODE_ENABLE=0
  BIMG_OBJS+=("$obj")
done
archive "$OUT_DIR/libbimg.a" "${BIMG_OBJS[@]}"

# ─── bgfx (Vulkan + GLES 3.1 backends, single-threaded; not ObjC++) ──────
# Matches the Android branch of top-level CMakeLists.txt before deletion.
echo "── bgfx (amalgamated, Vulkan+GLES, single-threaded) ──"
BGFX_OBJ="$OBJ_DIR/bgfx/amalgamated.o"
compile_cxx "$BGFX_OBJ" "$BGFX_DIR/src/amalgamated.cpp" \
  -I"$BGFX_DIR/include" -I"$BGFX_DIR/3rdparty" -I"$BGFX_DIR/3rdparty/khronos" -I"$BGFX_DIR/src" \
  -I"$BX_DIR/include" \
  -I"$BIMG_DIR/include" \
  -DBX_CONFIG_DEBUG=0 \
  -DBGFX_CONFIG_MULTITHREADED=0 \
  -DBGFX_CONFIG_RENDERER_VULKAN=1 \
  -DBGFX_CONFIG_RENDERER_OPENGLES=31
archive "$OUT_DIR/libbgfx.a" "$BGFX_OBJ"

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

# ─── libge (engine itself, Android variant) ───────────────────────────────
# Source list mirrors top-level CMakeLists.txt's Android branch:
#   - Common GE_CORE_SOURCES (cross-platform parts).
#   - Android-specific platform glue (FontLoader_android, SdlContext_android,
#     Immersive_android, CutoutInsets_android, Attitude_android, iap_android,
#     log_android, RefreshRateBoost_android).
#   - GE_DIRECT_RENDER_SOURCES (BgfxContext.mm, SessionHost.mm,
#     DirectRenderHost.mm — .mm compiled as plain C++ on Android).
#   - tools/player_orientation_stub.cpp (no iOS swizzle on Android).
echo "── ge (Android arm64) ──"

# Header search paths match the iOS xcodeproj-gem path
# (tools/ios-build/build_project.rb#header_search_paths) post-T71 header
# lifting — so the same #include landscape works on both platforms.
GE_INCLUDES=(
  -I include
  -I vendor/include
  -I "$SDL_STUB_INC"
  -I headers/spdlog/include
  -I headers/asio/include
  -I headers/sdl3/include
  -I vendor/sdl3/include
  -I headers/liteparser/include
  -I headers/lunasvg/include
  -I headers/plutovg/include
  -I headers/box2d/include
  -I headers/bx/include
  -I headers/bx/include/compat/linux
  -I headers/bimg/include
  -I headers/bgfx/include
)

GE_DEFINES=(
  -DASIO_STANDALONE
  -DGE_ANDROID
  -DGE_DIRECT
  -DGE_DIRECT_ONLY
  -DSQLITE_ENABLE_SESSION
  -DSQLITE_ENABLE_PREUPDATE_HOOK
  -DSQLITE_ENABLE_DESERIALIZE
  -DBX_CONFIG_DEBUG=0
  -DBIMG_CONFIG_DECODE_ENABLE=0
  -DBGFX_CONFIG_MULTITHREADED=0
  -DBGFX_CONFIG_RENDERER_VULKAN=1
  -DBGFX_CONFIG_RENDERER_OPENGLES=31
  -DLUNASVG_BUILD_STATIC
  -DPLUTOVG_BUILD_STATIC
)

GE_SOURCES=(
  src/Context.cpp
  src/Resource.cpp
  src/FileIO.cpp
  src/Signal.cpp
  src/sprite.cpp
  src/svg.cpp
  src/png.cpp
  src/text.cpp
  src/button.cpp
  src/sdl_input.cpp
  src/iap.cpp
  src/log.cpp
  src/audio.cpp
  src/FontLoader_android.cpp
  src/SdlContext_android.cpp
  src/Immersive_android.cpp
  src/CutoutInsets_android.cpp
  src/Attitude_android.cpp
  src/iap_android.cpp
  src/log_android.cpp
  src/render/RefreshRateBoost_android.cpp
  src/BgfxContext.mm
  src/SessionHost.mm
  src/render/DirectRenderHost.mm
  tools/player_orientation_stub.cpp
  vendor/src/sqlpipe.cpp
)

GE_OBJS=()
for src in "${GE_SOURCES[@]}"; do
  ext="${src##*.}"
  rel="${src%.*}"
  obj="$OBJ_DIR/ge/${rel//\//_}.o"
  mkdir -p "$(dirname "$obj")"
  case "$ext" in
    cpp|mm)
      # .mm on Android is plain C++ (no ObjC runtime). clang treats unknown
      # extensions as C++; force it via -x for the .mm case.
      lang_flag=()
      [[ "$ext" == "mm" ]] && lang_flag=(-x c++)
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
python3 tools/write-manifest.py --platform android-arm64

# ─── summary ──────────────────────────────────────────────────────────────
echo ""
echo "── prebuilt Android arm64 vendor libs ──"
ls -lh "$OUT_DIR"/*.a | awk '{printf "  %-30s %s\n", $NF, $5}'

echo ""
echo "Total: $(du -sh "$OUT_DIR" | cut -f1)"
