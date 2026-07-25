#!/bin/bash
# Build Dawn, SDL3, SDL3_image, and SDL3_ttf for macOS arm64.
# Run once (or after updating library versions).
#
# Usage: cd sq/tools/macos && bash build-deps.sh

set -euo pipefail
cd "$(dirname "$0")"

VENDOR="$(pwd)/../../vendor"
BUILD="$(pwd)/build"
JOBS=$(sysctl -n hw.ncpu)

# ── Library versions ─────────────────────────────
# Keep in sync with ios/build-deps.sh and android/build-deps.sh

DAWN_COMMIT="4764cd21387f41d979d6bdd662d08ec0f8bf267b"
DAWN_REPO="https://dawn.googlesource.com/dawn"

SDL_IMAGE_TAG="release-3.4.4"
SDL_IMAGE_REPO="https://github.com/libsdl-org/SDL_image.git"

SDL_TTF_TAG="release-3.2.2"
SDL_TTF_REPO="https://github.com/libsdl-org/SDL_ttf.git"

# Dawn's bundled jinja2 is incompatible with Python 3.13+.
PYTHON=$(/usr/bin/python3 -c 'import sys; print(sys.executable)')

# Common CMake flags for macOS arm64
MACOS_FLAGS=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_ARCHITECTURES=arm64
)

# ── Dawn ──────────────────────────────────────────

DAWN_SRC="$BUILD/dawn-src"

if [ ! -d "$DAWN_SRC/.git" ]; then
    echo "==> Cloning Dawn..."
    git clone "$DAWN_REPO" "$DAWN_SRC"
fi

echo "==> Checking out Dawn $DAWN_COMMIT..."
git -C "$DAWN_SRC" fetch --no-recurse-submodules origin
git -C "$DAWN_SRC" checkout --no-recurse-submodules "$DAWN_COMMIT"

echo "==> Fetching Dawn dependencies..."
python3 "$DAWN_SRC/tools/fetch_dawn_dependencies.py"

DAWN_B="$BUILD/dawn"
DAWN_DEST="$VENDOR/dawn/lib/macos-arm64"

echo "==> Configuring Dawn for macOS arm64..."
cmake -S "$DAWN_SRC" -B "$DAWN_B" \
    -DPython3_EXECUTABLE="$PYTHON" \
    "${MACOS_FLAGS[@]}" \
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC \
    -DDAWN_BUILD_SAMPLES=OFF \
    -DDAWN_BUILD_TESTS=OFF \
    -DDAWN_BUILD_PROTOBUF=OFF \
    -DTINT_BUILD_IR_BINARY=OFF \
    -DTINT_BUILD_TESTS=OFF \
    -DDAWN_ENABLE_METAL=ON \
    -DDAWN_ENABLE_NULL=OFF \
    -DDAWN_ENABLE_DESKTOP_GL=OFF \
    -DDAWN_ENABLE_OPENGLES=OFF \
    -DDAWN_ENABLE_D3D11=OFF \
    -DDAWN_ENABLE_D3D12=OFF \
    -DDAWN_ENABLE_VULKAN=OFF \
    -DDAWN_USE_GLFW=OFF \
    -DTINT_BUILD_CMD_TOOLS=OFF \
    -DTINT_BUILD_GLSL_VALIDATOR=OFF

echo "==> Building Dawn (this takes a while)..."
cmake --build "$DAWN_B" -j"$JOBS"

mkdir -p "$DAWN_DEST"
cp "$DAWN_B/src/dawn/libdawn_proc.a" "$DAWN_DEST/"
cp "$DAWN_B/src/dawn/native/libwebgpu_dawn.a" "$DAWN_DEST/"
cp "$DAWN_B/src/dawn/wire/libdawn_wire.a" "$DAWN_DEST/"
echo "==> Dawn installed to $DAWN_DEST"

# ── SDL3 ──────────────────────────────────────────

SDL_SRC="$VENDOR/github.com/libsdl-org/SDL"
SDL_B="$BUILD/sdl3"
SDL_PREFIX="$BUILD/sdl3-prefix"
SDL_DEST="$VENDOR/sdl3/lib/macos-arm64"

if [ ! -e "$SDL_SRC/.git" ]; then
    echo "ERROR: SDL submodule not initialized. Run:"
    echo "  cd sq && git submodule update --init vendor/github.com/libsdl-org/SDL"
    exit 1
fi

echo "==> Configuring SDL3 for macOS arm64..."
cmake -S "$SDL_SRC" -B "$SDL_B" \
    "${MACOS_FLAGS[@]}" \
    -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
    -DSDL_SHARED=OFF \
    -DSDL_STATIC=ON

echo "==> Building SDL3..."
cmake --build "$SDL_B" -j"$JOBS"

echo "==> Installing SDL3 to staging prefix..."
cmake --install "$SDL_B"

mkdir -p "$SDL_DEST"
cp "$SDL_B/libSDL3.a" "$SDL_DEST/"
echo "==> SDL3 installed to $SDL_DEST"

# ── SDL3_image ────────────────────────────────────

SDL_IMAGE_SRC="$BUILD/sdl3_image-src"
SDL_IMAGE_B="$BUILD/sdl3_image"

if [ ! -d "$SDL_IMAGE_SRC/.git" ]; then
    echo "==> Cloning SDL3_image..."
    git clone --branch "$SDL_IMAGE_TAG" --depth 1 "$SDL_IMAGE_REPO" "$SDL_IMAGE_SRC"
fi

echo "==> Configuring SDL3_image for macOS arm64..."
cmake -S "$SDL_IMAGE_SRC" -B "$SDL_IMAGE_B" \
    "${MACOS_FLAGS[@]}" \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL3_DIR="$SDL_PREFIX/lib/cmake/SDL3"

echo "==> Building SDL3_image..."
cmake --build "$SDL_IMAGE_B" -j"$JOBS"

cp "$SDL_IMAGE_B/libSDL3_image.a" "$SDL_DEST/"

# Install header alongside SDL3 headers
INC_DEST="$VENDOR/sdl3/include/SDL3_image"
mkdir -p "$INC_DEST"
cp "$SDL_IMAGE_SRC/include/SDL3_image/SDL_image.h" "$INC_DEST/"
echo "==> SDL3_image installed to $SDL_DEST"

# ── SDL3_ttf ──────────────────────────────────────

SDL_TTF_SRC="$BUILD/sdl3_ttf-src"
SDL_TTF_B="$BUILD/sdl3_ttf"

if [ ! -d "$SDL_TTF_SRC/.git" ]; then
    echo "==> Cloning SDL3_ttf..."
    git clone --branch "$SDL_TTF_TAG" --depth 1 --recurse-submodules "$SDL_TTF_REPO" "$SDL_TTF_SRC"
fi

echo "==> Configuring SDL3_ttf for macOS arm64..."
cmake -S "$SDL_TTF_SRC" -B "$SDL_TTF_B" \
    "${MACOS_FLAGS[@]}" \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDL3_DIR="$SDL_PREFIX/lib/cmake/SDL3" \
    -DSDLTTF_VENDORED=ON \
    -DSDLTTF_HARFBUZZ=ON

echo "==> Building SDL3_ttf..."
cmake --build "$SDL_TTF_B" -j"$JOBS"

cp "$SDL_TTF_B/libSDL3_ttf.a" "$SDL_DEST/"

# Copy vendored dependency static libs (needed for static linking)
for DIR in freetype harfbuzz plutosvg plutovg; do
    for LIB in "$SDL_TTF_B/external/$DIR/"*.a; do
        [ -f "$LIB" ] && cp "$LIB" "$SDL_DEST/"
    done
done

# Install header alongside SDL3 headers
INC_DEST="$VENDOR/sdl3/include/SDL3_ttf"
mkdir -p "$INC_DEST"
cp "$SDL_TTF_SRC/include/SDL3_ttf/SDL_ttf.h" "$INC_DEST/"
echo "==> SDL3_ttf installed to $SDL_DEST"

echo ""
echo "Done. Libraries are in:"
echo "  $VENDOR/dawn/lib/macos-arm64/"
echo "  $VENDOR/sdl3/lib/macos-arm64/"
