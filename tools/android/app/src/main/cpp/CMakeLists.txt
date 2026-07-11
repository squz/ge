cmake_minimum_required(VERSION 3.22)
project(GeRemote LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)

set(GE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

# SDL3 — built from source as part of the Gradle build
add_subdirectory(
    ${GE_ROOT}/vendor/github.com/libsdl-org/SDL
    ${CMAKE_CURRENT_BINARY_DIR}/SDL3
)

# FFmpeg (prebuilt, H.264 decode only)
set(FFMPEG_DIR ${GE_ROOT}/vendor/ffmpeg)

# Sources — Android entry point + shared H.264 player core + FFmpeg decoder.
# Keep this lean: full libge (Resource/sokol/sqlite/…) is not required for the
# brokered player path and would pull in sokol_gfx headers the NDK build does
# not need.
add_library(main SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/QRScanner_android.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/appchannel_stub.cpp
    ${GE_ROOT}/tools/player_core.cpp
    ${GE_ROOT}/tools/player_orientation_stub.cpp
    ${GE_ROOT}/src/bridge/PlayerWireBridge.cpp
    ${GE_ROOT}/src/bridge/WebSocketClient.cpp
    ${GE_ROOT}/src/bridge/VideoDecoder_ffmpeg.cpp
    ${GE_ROOT}/src/render/PlayerRender.cpp
    ${GE_ROOT}/src/Signal.cpp
)

target_compile_definitions(main PRIVATE
    GE_ANDROID
    ASIO_STANDALONE
)

target_include_directories(main PRIVATE
    ${GE_ROOT}/include
    ${GE_ROOT}/vendor/include
    ${GE_ROOT}/vendor/github.com/gabime/spdlog/include
    ${GE_ROOT}/vendor/github.com/chriskohlhoff/asio/include
    ${GE_ROOT}/vendor/github.com/libsdl-org/SDL/include
    ${GE_ROOT}/tools
    ${FFMPEG_DIR}/include
    ${GE_ROOT}/vendor/github.com/FFmpeg/FFmpeg
)

# FFmpeg static libs + SDL3 + Android system libs
# (libswscale is no longer needed: VideoDecoder_ffmpeg.cpp delivers IYUV
# directly to the renderer, which handles YUV→RGB on the GPU via SDL.)
target_link_libraries(main PRIVATE
    ${FFMPEG_DIR}/lib/android-arm64/libavcodec.a
    ${FFMPEG_DIR}/lib/android-arm64/libavutil.a
    SDL3::SDL3
    android
    log
    z
)
