#!/bin/bash
# Build the differential harness (box2d Scene + Chipmunk oracle).
set -e
GE=/Users/marcelo/work/github.com/squz/ge
CM=/Users/marcelo/work/marcelocantos.com/unwisegames/tiltbuggy/bricabrac/Chipmunk2D
TB=$GE/sample/tiltbuggy
mkdir -p build
# Chipmunk lib (once).
if [ ! -f build/libchipmunk.a ]; then
  for f in $(find $CM/src -name '*.c'); do
    cc -c -O2 -w -DNDEBUG -I$CM/include -I$CM/include/chipmunk "$f" -o build/cm_$(basename $f .c).o
  done
  ar rcs build/libchipmunk.a build/cm_*.o
fi
# Ensure Scene.o is fresh.
make -C $TB >/dev/null 2>&1 || true
c++ -std=c++20 -O2 -w -DNDEBUG -DGE_DIRECT_ONLY -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_DESERIALIZE -DLUNASVG_BUILD_STATIC \
  -I$GE/include -I$GE/vendor/include -I$GE/vendor/github.com/gabime/spdlog/include -I$GE/vendor/github.com/libsdl-org/SDL/include -I$GE/vendor/sdl3/include \
  -I$GE/vendor/github.com/erincatto/box2d/include -I$GE/vendor/github.com/chriskohlhoff/asio/include -I$GE/vendor/github.com/marcelocantos/deepparser/src \
  -I$GE/vendor/github.com/sammycage/lunasvg/include -I/opt/homebrew/opt/freetype/include/freetype2 -I$CM/include -I$CM/include/chipmunk -I$TB/src \
  -c harness.cpp -o build/harness.o
c++ build/harness.o $TB/build/src/Scene.o $TB/build/ge/vendor/box2d/*.o build/libchipmunk.a $TB/build/libge.a \
  $GE/vendor/sdl3/lib/macos-arm64/lib*.a \
  -framework Metal -framework MetalKit -framework QuartzCore -framework Cocoa -framework IOKit -framework CoreFoundation -framework Carbon \
  -framework CoreAudio -framework AudioToolbox -framework CoreHaptics -framework GameController -framework CoreVideo -framework ForceFeedback \
  -framework AVFoundation -framework CoreMedia -framework UniformTypeIdentifiers -framework CoreGraphics -framework VideoToolbox -framework CoreMotion -framework StoreKit \
  -o build/harness
echo "built build/harness"
