#!/usr/bin/env bash
# Rebuild vendor/sdl3 static libs (and iOS xcframeworks) from the SDL submodule.
# Run from ge repo root after: git -C vendor/github.com/libsdl-org/SDL checkout release-X.Y.Z
#
# Usage:
#   tools/rebuild-sdl-prebuilts.sh [platform ...]
#
#   platform: macos-arm64 | ios-arm64 | ios-arm64-simulator | android-arm64
#             | xcframeworks | all        (default: all)
#
# 🎯T190 / 🎯T195: the platform argument exists so tools/ensure-sdl.sh can cook
# exactly the leg a link needs. A macOS-only consumer should not also pay for
# iOS device, iOS simulator, Android and the xcframeworks.
#
# Also rebuilds SDL_image (release-3.4.4) + SDL_ttf (vendored) for Apple
# platforms. Android gets SDL core only (consumers build the rest from the
# submodule via cmake add_subdirectory).
set -euo pipefail

usage() {
  echo "usage: $0 [platform ...]" >&2
  echo "  platform: macos-arm64 | ios-arm64 | ios-arm64-simulator | android-arm64" >&2
  echo "            | xcframeworks | all   (default: all)" >&2
}

WANT_MACOS=0
WANT_IOS=0
WANT_IOSSIM=0
WANT_ANDROID=0
WANT_XCF=0

# bash 3.2 (macOS) expands an empty array under `set -u` as unbound, so drive
# the loop off "$@" and seed it with the default instead of building an array.
if [[ $# -eq 0 ]]; then
  set -- all
fi

for arg in "$@"; do
  case "$arg" in
    all)
      WANT_MACOS=1; WANT_IOS=1; WANT_IOSSIM=1; WANT_ANDROID=1; WANT_XCF=1 ;;
    macos-arm64)         WANT_MACOS=1 ;;
    ios-arm64)           WANT_IOS=1 ;;
    ios-arm64-simulator) WANT_IOSSIM=1 ;;
    android-arm64)       WANT_ANDROID=1 ;;
    # An xcframework lipos both iOS slices, so it implies both.
    xcframeworks)        WANT_IOS=1; WANT_IOSSIM=1; WANT_XCF=1 ;;
    -h|--help)           usage; exit 0 ;;
    *)
      echo "error: unknown platform '$arg'" >&2
      usage
      exit 1
      ;;
  esac
done


GE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$GE_ROOT"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
VENDOR="$GE_ROOT/vendor"
SDL_SRC="$VENDOR/github.com/libsdl-org/SDL"
BUILD="$GE_ROOT/build/rebuild-sdl"
SDL_IMAGE_TAG="${SDL_IMAGE_TAG:-release-3.4.4}"
SDL_TTF_TAG="${SDL_TTF_TAG:-release-3.2.2}"
MIN_IOS="${GE_IOS_DEPLOYMENT_TARGET:-16.3}"

if [[ ! -e "$SDL_SRC/.git" && ! -f "$SDL_SRC/.git" ]]; then
  echo "error: SDL submodule missing" >&2
  exit 1
fi

echo "SDL submodule: $(git -C "$SDL_SRC" describe --tags --always)"

mkdir -p "$BUILD"

build_sdl_apple() {
  local plat="$1"   # macos | ios | iossim
  local dest_key="$2"
  local bdir="$BUILD/sdl-$dest_key"
  local dest="$VENDOR/sdl3/lib/$dest_key"
  mkdir -p "$dest"

  local cmake_args=(
    -S "$SDL_SRC" -B "$bdir"
    -DCMAKE_BUILD_TYPE=Release
    -DSDL_SHARED=OFF
    -DSDL_STATIC=ON
    -DSDL_TEST=OFF
    -DSDL_TESTS=OFF
    -DSDL_EXAMPLES=OFF
  )

  case "$plat" in
    macos)
      cmake_args+=(-DCMAKE_OSX_ARCHITECTURES=arm64)
      ;;
    ios)
      cmake_args+=(
        -DCMAKE_SYSTEM_NAME=iOS
        -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS"
        -DCMAKE_OSX_SYSROOT="$(xcrun --sdk iphoneos --show-sdk-path)"
      )
      ;;
    iossim)
      cmake_args+=(
        -DCMAKE_SYSTEM_NAME=iOS
        -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS"
        -DCMAKE_OSX_SYSROOT="$(xcrun --sdk iphonesimulator --show-sdk-path)"
        -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO
      )
      ;;
  esac

  echo "── SDL3 $dest_key ──"
  cmake "${cmake_args[@]}"
  cmake --build "$bdir" -j"$JOBS"

  # Find libSDL3.a (layout varies by generator)
  local lib
  lib="$(find "$bdir" -name 'libSDL3.a' | head -1)"
  [[ -n "$lib" ]] || { echo "error: libSDL3.a not found under $bdir" >&2; exit 1; }
  cp "$lib" "$dest/libSDL3.a"
  echo "  → $dest/libSDL3.a"
}

# Install prefix for SDL so image/ttf can find SDL3Config.cmake
install_sdl_macos_prefix() {
  local bdir="$BUILD/sdl-macos-arm64"
  local prefix="$BUILD/sdl3-prefix"
  cmake -S "$SDL_SRC" -B "$bdir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF
  cmake --build "$bdir" -j"$JOBS"
  cmake --install "$bdir"
  cp "$bdir/libSDL3.a" "$VENDOR/sdl3/lib/macos-arm64/libSDL3.a" 2>/dev/null \
    || cp "$(find "$bdir" -name libSDL3.a | head -1)" "$VENDOR/sdl3/lib/macos-arm64/libSDL3.a"
}

clone_or_checkout() {
  local dir="$1" url="$2" tag="$3"
  # 🎯T201: a cook killed mid-clone can leave $dir/.git half-written (e.g. a
  # stale config.lock or no reachable HEAD). The next cook then finds
  # "$dir/.git" present, tries to fetch/checkout against it, and fails with
  # "could not lock config file" / "Unable to read current working
  # directory" instead of just re-cloning. Detect that and start clean.
  if [[ -d "$dir/.git" ]] && ! git -C "$dir" rev-parse --verify -q HEAD >/dev/null; then
    echo "ge: $dir looks like a half-written clone (no reachable HEAD) — removing and re-cloning" >&2
    rm -rf "$dir"
  fi
  if [[ ! -d "$dir/.git" ]]; then
    git clone --branch "$tag" --depth 1 --recurse-submodules "$url" "$dir"
  else
    git -C "$dir" fetch --tags --depth 1 origin "refs/tags/$tag:refs/tags/$tag" 2>/dev/null || \
      git -C "$dir" fetch --tags origin
    git -C "$dir" checkout -f "$tag"
    git -C "$dir" submodule update --init --recursive || true
  fi
}

build_image_ttf_macos() {
  local dest="$VENDOR/sdl3/lib/macos-arm64"
  local prefix="$BUILD/sdl3-prefix"
  mkdir -p "$dest"

  local img_src="$BUILD/sdl3_image-src"
  clone_or_checkout "$img_src" https://github.com/libsdl-org/SDL_image.git "$SDL_IMAGE_TAG"
  local img_b="$BUILD/sdl3_image"
  cmake -S "$img_src" -B "$img_b" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL3_DIR="$prefix/lib/cmake/SDL3"
  cmake --build "$img_b" -j"$JOBS"
  cp "$(find "$img_b" -name libSDL3_image.a | head -1)" "$dest/"
  mkdir -p "$VENDOR/sdl3/include/SDL3_image"
  cp "$img_src/include/SDL3_image/SDL_image.h" "$VENDOR/sdl3/include/SDL3_image/"

  local ttf_src="$BUILD/sdl3_ttf-src"
  clone_or_checkout "$ttf_src" https://github.com/libsdl-org/SDL_ttf.git "$SDL_TTF_TAG"
  local ttf_b="$BUILD/sdl3_ttf"
  cmake -S "$ttf_src" -B "$ttf_b" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL3_DIR="$prefix/lib/cmake/SDL3" \
    -DSDLTTF_VENDORED=ON -DSDLTTF_HARFBUZZ=ON
  cmake --build "$ttf_b" -j"$JOBS"
  cp "$(find "$ttf_b" -name libSDL3_ttf.a | head -1)" "$dest/"
  for DIR in freetype harfbuzz plutosvg plutovg; do
    for LIB in "$ttf_b/external/$DIR/"*.a; do
      [[ -f "$LIB" ]] && cp "$LIB" "$dest/"
    done
  done
  mkdir -p "$VENDOR/sdl3/include/SDL3_ttf"
  cp "$ttf_src/include/SDL3_ttf/SDL_ttf.h" "$VENDOR/sdl3/include/SDL3_ttf/"
}

build_image_ttf_ios() {
  local dest_key="$1"   # ios-arm64 | ios-arm64-simulator
  local sdk sysroot arch
  if [[ "$dest_key" == "ios-arm64" ]]; then
    sdk=iphoneos
  else
    sdk=iphonesimulator
  fi
  sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  arch=arm64
  local dest="$VENDOR/sdl3/lib/$dest_key"
  local prefix="$BUILD/sdl3-prefix-$dest_key"
  mkdir -p "$dest"

  # Install SDL for this platform so image/ttf can find it
  local sdl_b="$BUILD/sdl-$dest_key"
  cmake --install "$sdl_b" --prefix "$prefix" 2>/dev/null || {
    # reconfigure with install prefix if needed
    cmake -S "$SDL_SRC" -B "$sdl_b" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES="$arch" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" \
      -DCMAKE_OSX_SYSROOT="$sysroot" \
      -DCMAKE_INSTALL_PREFIX="$prefix" \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF
    cmake --build "$sdl_b" -j"$JOBS"
    cmake --install "$sdl_b"
  }

  local img_src="$BUILD/sdl3_image-src"
  local img_b="$BUILD/sdl3_image-$dest_key"
  cmake -S "$img_src" -B "$img_b" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" \
    -DCMAKE_OSX_SYSROOT="$sysroot" \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL3_DIR="$prefix/lib/cmake/SDL3"
  cmake --build "$img_b" -j"$JOBS"
  cp "$(find "$img_b" -name libSDL3_image.a | head -1)" "$dest/"

  local ttf_src="$BUILD/sdl3_ttf-src"
  local ttf_b="$BUILD/sdl3_ttf-$dest_key"
  cmake -S "$ttf_src" -B "$ttf_b" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" \
    -DCMAKE_OSX_SYSROOT="$sysroot" \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL3_DIR="$prefix/lib/cmake/SDL3" \
    -DSDLTTF_VENDORED=ON -DSDLTTF_HARFBUZZ=ON
  cmake --build "$ttf_b" -j"$JOBS"
  cp "$(find "$ttf_b" -name libSDL3_ttf.a | head -1)" "$dest/"
  for DIR in freetype harfbuzz plutosvg plutovg; do
    for LIB in "$ttf_b/external/$DIR/"*.a; do
      [[ -f "$LIB" ]] && cp "$LIB" "$dest/"
    done
  done
}

build_sdl_android() {
  local ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
  if [[ -z "$ndk" ]]; then
    # Any installed major will do here (SDL has no libc++ ABI pin of its
    # own), so scan the whole ndk/ directory by glob — never by parsing `ls`,
    # whose output carries ANSI escapes on a colourised host (🎯T191).
    local d
    local cands=()
    for d in "$HOME"/Library/Android/sdk/ndk/*/; do
      [[ -d "$d" ]] || continue
      cands+=("${d%/}")
    done
    if [[ ${#cands[@]} -gt 0 ]]; then
      ndk="$(printf '%s\n' "${cands[@]}" | sort -V | tail -1)"
    fi
  fi
  if [[ -z "$ndk" || ! -d "$ndk" ]]; then
    echo "WARN: no Android NDK; skipping android-arm64 libSDL3.a (consumers build SDL from submodule)"
    return 0
  fi
  local dest="$VENDOR/sdl3/lib/android-arm64"
  mkdir -p "$dest"
  local bdir="$BUILD/sdl-android-arm64"
  local toolchain="$ndk/build/cmake/android.toolchain.cmake"
  echo "── SDL3 android-arm64 (NDK $ndk) ──"
  cmake -S "$SDL_SRC" -B "$bdir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF
  cmake --build "$bdir" -j"$JOBS"
  cp "$(find "$bdir" -name libSDL3.a | head -1)" "$dest/"
}

rebuild_xcframeworks() {
  echo "── xcframeworks ──"
  local xcf_root="$VENDOR/sdl3/xcframeworks"
  mkdir -p "$xcf_root"
  rebuild_one_xcf() {
    local name="$1"
    local a_device="$VENDOR/sdl3/lib/ios-arm64/lib${name}.a"
    local a_sim="$VENDOR/sdl3/lib/ios-arm64-simulator/lib${name}.a"
    [[ -f "$a_device" && -f "$a_sim" ]] || { echo "  skip $name (missing .a)"; return 0; }
    local out="$xcf_root/${name}.xcframework"
    rm -rf "$out"
    xcodebuild -create-xcframework \
      -library "$a_device" \
      -library "$a_sim" \
      -output "$out"
    echo "  → $out"
  }
  rebuild_one_xcf SDL3
  rebuild_one_xcf SDL3_image
  rebuild_one_xcf SDL3_ttf
  for name in freetype harfbuzz plutosvg plutovg; do
    rebuild_one_xcf "$name"
  done
}

# ── main ────────────────────────────────────────────────────────────
if [[ "$WANT_MACOS" -eq 1 ]]; then
  mkdir -p "$VENDOR/sdl3/lib/macos-arm64"
  echo "==> macOS SDL (+ install prefix for image/ttf)"
  install_sdl_macos_prefix
  build_image_ttf_macos
fi

if [[ "$WANT_IOS" -eq 1 ]]; then
  mkdir -p "$VENDOR/sdl3/lib/ios-arm64"
  echo "==> iOS device SDL"
  build_sdl_apple ios ios-arm64
fi
if [[ "$WANT_IOSSIM" -eq 1 ]]; then
  mkdir -p "$VENDOR/sdl3/lib/ios-arm64-simulator"
  echo "==> iOS simulator SDL"
  build_sdl_apple iossim ios-arm64-simulator
fi

if [[ "$WANT_IOS" -eq 1 || "$WANT_IOSSIM" -eq 1 ]]; then
  clone_or_checkout "$BUILD/sdl3_image-src" https://github.com/libsdl-org/SDL_image.git "$SDL_IMAGE_TAG"
  clone_or_checkout "$BUILD/sdl3_ttf-src" https://github.com/libsdl-org/SDL_ttf.git "$SDL_TTF_TAG"
fi
if [[ "$WANT_IOS" -eq 1 ]]; then
  echo "==> iOS device image/ttf"
  build_image_ttf_ios ios-arm64
fi
if [[ "$WANT_IOSSIM" -eq 1 ]]; then
  echo "==> iOS simulator image/ttf"
  build_image_ttf_ios ios-arm64-simulator
fi

if [[ "$WANT_ANDROID" -eq 1 ]]; then
  mkdir -p "$VENDOR/sdl3/lib/android-arm64"
  build_sdl_android
fi

if [[ "$WANT_XCF" -eq 1 ]]; then
  rebuild_xcframeworks
fi

echo ""
echo "Done. Next: tools/lift-headers.sh && make prebuild"
if [[ "$WANT_MACOS" -eq 1 ]]; then
  echo "Version check:"
  strings "$VENDOR/sdl3/lib/macos-arm64/libSDL3.a" | grep -E 'SDL-3\.[0-9]' | head -3 || true
fi
