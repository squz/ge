cmake_minimum_required(VERSION 3.22)
project(GeRemote LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)

set(GE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

# ── 16 KB page alignment (🎯T75) ────────────────────────────────────
# This player CMakeLists does NOT include cmake/android-arm64.cmake (lean
# brokered path — no libge.a), so it must apply the same linker flag that
# ge's consumer template gets for free. Without it, Android 16 shows the
# system "ELF alignment check failed" dialog (Pixel Tablet — regressed
# 2026-07-11 and again 2026-07-14 when this block was lost off master).
# Must precede every add_library(SHARED) / add_subdirectory that produces
# a .so (SDL3 + libmain). See also tools/android/check-16kb.sh +
# build.gradle ANDROID_STL=c++_static.
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-z,max-page-size=16384")

# SDL3 — built from source as part of the Gradle build
add_subdirectory(
    ${GE_ROOT}/vendor/github.com/libsdl-org/SDL
    ${CMAKE_CURRENT_BINARY_DIR}/SDL3
)

# SDL_image — decode MakeEncodedImage (PNG) recipes on the player.
set(SDL3IMAGE_VENDORED OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${GE_ROOT}/vendor/github.com/libsdl-org/SDL_image
    ${CMAKE_CURRENT_BINARY_DIR}/SDL3_image
)

# SDL_ttf (vendored FreeType) — ge::rasterizeTextToPixelsFromMemory needs FT_*.
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
add_subdirectory(
    ${GE_ROOT}/vendor/github.com/libsdl-org/SDL_ttf
    ${CMAKE_CURRENT_BINARY_DIR}/SDL3_ttf
)

# FFmpeg (prebuilt, H.264 decode only)
set(FFMPEG_DIR ${GE_ROOT}/vendor/ffmpeg)

set(LUNASVG_DIR ${GE_ROOT}/vendor/github.com/sammycage/lunasvg)
file(GLOB LUNASVG_SRC ${LUNASVG_DIR}/source/*.cpp)
file(GLOB PLUTOVG_SRC ${LUNASVG_DIR}/plutovg/source/*.c)

# Sources — Android entry point + shared H.264 player core + FFmpeg decoder
# + cmdstream recipe materialize (SVG/text/encoded, GE_PLAYER_NO_SOKOL).
add_library(main SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/QRScanner_android.cpp
    ${GE_ROOT}/tools/player_core.cpp
    ${GE_ROOT}/tools/player_orientation_stub.cpp
    ${GE_ROOT}/tools/AudioPlayer.cpp
    ${GE_ROOT}/src/bridge/PlayerWireBridge.cpp
    ${GE_ROOT}/src/bridge/WebSocketClient.cpp
    ${GE_ROOT}/src/bridge/VideoDecoder_ffmpeg.cpp
    ${GE_ROOT}/src/render/PlayerRender.cpp
    ${GE_ROOT}/src/CmdStream.cpp
    ${GE_ROOT}/src/Signal.cpp
    ${GE_ROOT}/src/Resource.cpp
    ${GE_ROOT}/src/FileIO.cpp
    ${GE_ROOT}/src/audio.cpp
    ${GE_ROOT}/src/svg.cpp
    ${GE_ROOT}/src/text.cpp
    ${GE_ROOT}/src/FontLoader_android.cpp
    ${GE_ROOT}/src/Immersive_android.cpp
    ${GE_ROOT}/src/CutoutInsets_android.cpp
    # Slim stubs: appchannel full path needs nlohmann + sockets we don't ship
    # in the brokered player. Signal crash handler only needs flush().
    ${CMAKE_CURRENT_SOURCE_DIR}/player_stubs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/player_sprite_stub.cpp
    ${GE_ROOT}/vendor/src/sqlite3.c
    ${GE_ROOT}/vendor/src/sqlpipe.cpp
    ${GE_ROOT}/vendor/src/lz4.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/arena.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/liteparser.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/lp_tokenize.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/lp_unparse.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/parse.c
    ${LUNASVG_SRC}
    ${PLUTOVG_SRC}
)

target_compile_definitions(main PRIVATE
    GE_ANDROID
    GE_PLAYER_NO_SOKOL
    ASIO_STANDALONE
    SQLITE_ENABLE_SESSION
    SQLITE_ENABLE_PREUPDATE_HOOK
    SQLITE_ENABLE_DESERIALIZE
    LUNASVG_BUILD_STATIC
)

target_include_directories(main PRIVATE
    ${GE_ROOT}/include
    ${GE_ROOT}/vendor/include
    ${GE_ROOT}/vendor/github.com/gabime/spdlog/include
    ${GE_ROOT}/vendor/github.com/chriskohlhoff/asio/include
    ${GE_ROOT}/vendor/github.com/libsdl-org/SDL/include
    ${GE_ROOT}/vendor/github.com/libsdl-org/SDL_image/include
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src
    ${GE_ROOT}/vendor/github.com/floooh/sokol
    ${GE_ROOT}/vendor/github.com/sammycage/lunasvg/include
    ${GE_ROOT}/vendor/github.com/sammycage/lunasvg/plutovg/include
    ${GE_ROOT}/tools
    ${FFMPEG_DIR}/include
    ${GE_ROOT}/vendor/github.com/FFmpeg/FFmpeg
)

# FFmpeg static libs + SDL3 + image/ttf (freetype) + Android system libs
# (libswscale is no longer needed: VideoDecoder_ffmpeg.cpp delivers IYUV
# directly to the renderer, which handles YUV→RGB on the GPU via SDL.)
target_link_libraries(main PRIVATE
    ${FFMPEG_DIR}/lib/android-arm64/libavcodec.a
    ${FFMPEG_DIR}/lib/android-arm64/libavutil.a
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
    freetype
    android
    log
    z
)
