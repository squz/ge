# ge engine — static library for CMake consumers (iOS, etc.)
# Desktop builds use Module.mk instead.
#
# Options:
#   GE_DIRECT  — Build for direct rendering (no wire/networking). Default: OFF.

cmake_minimum_required(VERSION 3.28...4.2)
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    project(ge LANGUAGES C CXX)
else()
    project(ge LANGUAGES C CXX OBJCXX)
endif()
set(CMAKE_CXX_STANDARD 23)

option(GE_DIRECT "Direct rendering mode (no wire/networking)" OFF)

# ── Vendor paths ──────────────────────────────────────
set(GE_VENDOR     ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(BX_DIR        ${GE_VENDOR}/github.com/bkaradzic/bx)
set(BIMG_DIR      ${GE_VENDOR}/github.com/bkaradzic/bimg)
set(BGFX_DIR      ${GE_VENDOR}/github.com/bkaradzic/bgfx)
set(BOX2D_DIR     ${GE_VENDOR}/github.com/erincatto/box2d)
set(LITEPARSER_DIR ${GE_VENDOR}/github.com/sqliteai/liteparser/src)
set(LUNASVG_DIR   ${GE_VENDOR}/github.com/sammycage/lunasvg)
set(PLUTOVG_DIR   ${LUNASVG_DIR}/plutovg)

# ── bgfx/bx/bimg static libraries ────────────────────

set(BGFX_COMPILE_FLAGS -DBX_CONFIG_DEBUG=0)
if(APPLE)
    list(APPEND BGFX_COMPILE_FLAGS -DBGFX_CONFIG_RENDERER_METAL=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    # Single-threaded mode is non-negotiable on Android: bgfx's default
    # multi-thread mode spawns a render thread that keeps hammering the
    # SurfaceView's BufferQueue after the activity backgrounds (the queue
    # is "abandoned" but bgfx's thread doesn't know). Forcing single-thread
    # routes every bgfx call through the main thread, where the SDL3
    # WINDOW_FOCUS_LOST / RESTORED handler in DirectRenderHost gates them
    # via BgfxContext::paused().
    list(APPEND BGFX_COMPILE_FLAGS -DBGFX_CONFIG_MULTITHREADED=0)
    # Compile in BOTH backends. BgfxContext.mm picks one at runtime per
    # device:
    #   * Real Adreno / Mali devices → OpenGL ES 3.1 (Vulkan path stalls;
    #     see docs/papers/adreno-830-bgfx-vulkan-crash.md).
    #   * Apple-Silicon AVD emulator → Vulkan (the AVD's Apple-Metal-backed
    #     EGL translator exposes only GLES 3.0, so a 3.1 request fails).
    list(APPEND BGFX_COMPILE_FLAGS
        -DBGFX_CONFIG_RENDERER_VULKAN=1
        -DBGFX_CONFIG_RENDERER_OPENGLES=31
    )
endif()

# bx
add_library(bx STATIC ${BX_DIR}/src/amalgamated.cpp)
target_include_directories(bx PUBLIC
    ${BX_DIR}/include
    ${BX_DIR}/3rdparty
)
# Apple needs osx compat; Android uses the linux compat path (bx handles
# platform-specific bits internally via __ANDROID__).
if(APPLE)
    target_include_directories(bx PUBLIC ${BX_DIR}/include/compat/osx)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_include_directories(bx PUBLIC ${BX_DIR}/include/compat/linux)
endif()
target_include_directories(bx PRIVATE
    ${BIMG_DIR}/include
    ${BGFX_DIR}/include
)
target_compile_options(bx PUBLIC ${BGFX_COMPILE_FLAGS})
set_target_properties(bx PROPERTIES CXX_STANDARD 20)

# bimg (image.cpp + image_gnf.cpp — decode/encode not needed for bgfx runtime)
add_library(bimg STATIC
    ${BIMG_DIR}/src/image.cpp
    ${BIMG_DIR}/src/image_gnf.cpp
)
target_include_directories(bimg PUBLIC ${BIMG_DIR}/include)
target_include_directories(bimg PRIVATE
    ${BX_DIR}/include
    ${BGFX_DIR}/include
    ${BIMG_DIR}/3rdparty
    ${BIMG_DIR}/3rdparty/astc-encoder/include
)
if(APPLE)
    target_include_directories(bimg PRIVATE ${BX_DIR}/include/compat/osx)
endif()
target_compile_options(bimg PUBLIC ${BGFX_COMPILE_FLAGS})
target_compile_options(bimg PRIVATE -DBIMG_CONFIG_DECODE_ENABLE=0)
set_target_properties(bimg PROPERTIES CXX_STANDARD 20)

# bgfx (amalgamated; Metal backend requires ObjC++ on Apple)
add_library(bgfx STATIC ${BGFX_DIR}/src/amalgamated.cpp)
target_include_directories(bgfx PUBLIC ${BGFX_DIR}/include)
target_include_directories(bgfx PRIVATE
    ${BX_DIR}/include
    ${BIMG_DIR}/include
    ${BGFX_DIR}/3rdparty
    ${BGFX_DIR}/3rdparty/khronos
    ${BGFX_DIR}/src
)
if(APPLE)
    target_include_directories(bgfx PRIVATE ${BX_DIR}/include/compat/osx)
    # Metal backend needs ObjC++ compilation
    set_source_files_properties(${BGFX_DIR}/src/amalgamated.cpp
        PROPERTIES LANGUAGE OBJCXX)
endif()
target_compile_options(bgfx PUBLIC ${BGFX_COMPILE_FLAGS})
set_target_properties(bgfx PROPERTIES CXX_STANDARD 20)

# ── Box2D (uses its own CMakeLists.txt) ───────────────

set(BOX2D_SAMPLES   OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS      OFF CACHE BOOL "" FORCE)
add_subdirectory(${BOX2D_DIR} ${CMAKE_BINARY_DIR}/box2d)

# ── SQLite3 ───────────────────────────────────────────

add_library(sqlite3_ge STATIC ${GE_VENDOR}/src/sqlite3.c)
set_target_properties(sqlite3_ge PROPERTIES C_STANDARD 11)
target_include_directories(sqlite3_ge PUBLIC ${GE_VENDOR}/include)
target_compile_options(sqlite3_ge PRIVATE
    -DSQLITE_ENABLE_SESSION
    -DSQLITE_ENABLE_PREUPDATE_HOOK
    -DSQLITE_ENABLE_DESERIALIZE
)

# ── liteparser (C code, used by sqlpipe) ─────────────

add_library(liteparser STATIC
    ${LITEPARSER_DIR}/arena.c
    ${LITEPARSER_DIR}/liteparser.c
    ${LITEPARSER_DIR}/lp_tokenize.c
    ${LITEPARSER_DIR}/lp_unparse.c
    ${LITEPARSER_DIR}/parse.c
)
target_include_directories(liteparser PUBLIC ${LITEPARSER_DIR})
set_target_properties(liteparser PROPERTIES C_STANDARD 11)

# ── lz4 ──────────────────────────────────────────────

add_library(lz4_ge STATIC ${GE_VENDOR}/src/lz4.c)
target_include_directories(lz4_ge PUBLIC ${GE_VENDOR}/include)
set_target_properties(lz4_ge PROPERTIES C_STANDARD 11)

# ── lunasvg + bundled plutovg ─────────────────────────
# lunasvg is the canonical SVG rasterizer used by ge::rasterizeSvg.
# It bundles its own pinned plutovg (distinct from the SDL_ttf one).
# Both are compiled with hidden visibility and static-build macros to
# avoid symbol clashes with the prebuilt libplutovg.a in SDL3_ttf.

file(GLOB LUNASVG_SRC ${LUNASVG_DIR}/source/*.cpp)
file(GLOB PLUTOVG_SRC ${PLUTOVG_DIR}/source/*.c)

add_library(lunasvg_ge STATIC ${LUNASVG_SRC})
target_include_directories(lunasvg_ge PUBLIC  ${LUNASVG_DIR}/include)
target_include_directories(lunasvg_ge PRIVATE ${LUNASVG_DIR}/source ${PLUTOVG_DIR}/include)
target_compile_definitions(lunasvg_ge PRIVATE LUNASVG_BUILD LUNASVG_BUILD_STATIC PLUTOVG_BUILD_STATIC)
target_compile_options(lunasvg_ge PRIVATE -fvisibility=hidden)
set_target_properties(lunasvg_ge PROPERTIES CXX_STANDARD 17)

add_library(plutovg_ge STATIC ${PLUTOVG_SRC})
target_include_directories(plutovg_ge PUBLIC  ${PLUTOVG_DIR}/include)
target_include_directories(plutovg_ge PRIVATE ${PLUTOVG_DIR}/source)
target_compile_definitions(plutovg_ge PRIVATE PLUTOVG_BUILD PLUTOVG_BUILD_STATIC)
target_compile_options(plutovg_ge PRIVATE -fvisibility=hidden)
set_target_properties(plutovg_ge PROPERTIES C_STANDARD 11)

# ── ge core sources ───────────────────────────────────

set(GE_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Resource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FileIO.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Signal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sprite.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/svg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/png.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/button.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdl_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/iap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/src/sqlift.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/src/sqlpipe.cpp
)

if(APPLE)
    list(APPEND GE_CORE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontLoader_apple.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SdlContext.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Immersive_stub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CutoutInsets_stub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/iap_apple.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/log_apple.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/audio_apple.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RefreshRateBoost_apple.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Attitude_apple.mm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    list(APPEND GE_CORE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FontLoader_android.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SdlContext_android.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Immersive_android.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CutoutInsets_android.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Attitude_android.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/iap_android.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/log_android.cpp)
else()
    list(APPEND GE_CORE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Immersive_stub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CutoutInsets_stub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Attitude_stub.cpp)
endif()

# Sources that live in both direct and wire modes — bgfx init + session
# plumbing + the direct-rendering composite path. These are .mm on Apple
# (ObjC++) and .mm compiled as CXX on Android (same files, but the .mm
# extension is just symmetry with the Apple build).
set(GE_DIRECT_RENDER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/BgfxContext.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SessionHost.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/DirectRenderHost.mm
)

# DirectRenderHost calls playerForceOrientation() in send(); the symbol
# lives in tools/player_orientation_{ios,stub}.{mm,cpp}. iOS bundles want
# the swizzle (Apple TN3192); every other platform takes the no-op stub.
# Bundling either one into libge so consuming apps don't have to remember.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND GE_DIRECT_RENDER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/player_orientation_ios.mm)
else()
    list(APPEND GE_DIRECT_RENDER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/player_orientation_stub.cpp)
endif()

if(GE_DIRECT)
    # Direct mode: app owns the frame loop; ge provides bgfx init,
    # DirectRenderHost, SDL context, font loader, and utilities.
    set(GE_SOURCES ${GE_CORE_SOURCES} ${GE_DIRECT_RENDER_SOURCES})
else()
    # Wire mode: direct-render sources plus networking and H.264 codec
    # services for the brokered ged/player modality.
    set(GE_SOURCES ${GE_CORE_SOURCES} ${GE_DIRECT_RENDER_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WebSocketClient.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoEncoder_apple.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoDecoder_apple.mm
    )
endif()

add_library(ge STATIC ${GE_SOURCES})

# ── ObjC++ for .mm files ──────────────────────────────
# On Apple, .mm files are ObjC++. On Android, the .mm extension is symmetry
# with the Apple build — compile as plain CXX since there's no ObjC runtime.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set_source_files_properties(${GE_DIRECT_RENDER_SOURCES}
        PROPERTIES LANGUAGE CXX)
else()
    set(MM_FILES ${GE_DIRECT_RENDER_SOURCES})
    if(NOT GE_DIRECT)
        list(APPEND MM_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoEncoder_apple.mm
            ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoDecoder_apple.mm
        )
    endif()
    set_source_files_properties(${MM_FILES} PROPERTIES LANGUAGE OBJCXX)
endif()

# ── Public include directories ────────────────────────

target_include_directories(ge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GE_VENDOR}/include
    ${GE_VENDOR}/github.com/gabime/spdlog/include
    ${GE_VENDOR}/github.com/libsdl-org/SDL/include
    ${GE_VENDOR}/sdl3/include
    ${GE_VENDOR}/github.com/erincatto/box2d/include
    ${LITEPARSER_DIR}
    ${BGFX_DIR}/include
    ${BX_DIR}/include
    ${BIMG_DIR}/include
    ${LUNASVG_DIR}/include
)
if(APPLE)
    target_include_directories(ge PUBLIC ${BX_DIR}/include/compat/osx)
endif()

# SQLite defines must be visible to ge consumers (Tweak.h uses SQLite)
target_compile_definitions(ge PUBLIC
    SQLITE_ENABLE_SESSION
    SQLITE_ENABLE_PREUPDATE_HOOK
    SQLITE_ENABLE_DESERIALIZE
)

if(GE_DIRECT)
    # GE_DIRECT — general "direct mode is enabled" marker.
    # GE_DIRECT_ONLY — specifically signals the brokered session code (and
    #                  its asio/WebSocket dependency tree) is absent, so
    #                  SessionHost.mm stubs runBrokered at compile time.
    target_compile_definitions(ge PUBLIC GE_DIRECT GE_DIRECT_ONLY)
else()
    target_include_directories(ge PUBLIC
        ${GE_VENDOR}/github.com/chriskohlhoff/asio/include
    )
    target_compile_definitions(ge PUBLIC ASIO_STANDALONE)
endif()

# ── Link internal vendor libs into ge ─────────────────

target_link_libraries(ge PRIVATE
    bgfx bimg bx
    sqlite3_ge liteparser lz4_ge
    box2d
    lunasvg_ge plutovg_ge
)

# ── Platform-specific dependencies ────────────────────

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    # SDL3 — built from source
    add_subdirectory(
        ${GE_VENDOR}/github.com/libsdl-org/SDL
        ${CMAKE_BINARY_DIR}/SDL3
    )
    target_link_libraries(ge PUBLIC SDL3::SDL3)

    # SDL3_image — built from source
    set(SDL3IMAGE_VENDORED OFF)
    add_subdirectory(
        ${GE_VENDOR}/github.com/libsdl-org/SDL_image
        ${CMAKE_BINARY_DIR}/SDL3_image
    )
    target_link_libraries(ge PUBLIC SDL3_image::SDL3_image)

    # SDL3_ttf — built from source; brings freetype, harfbuzz,
    # plutosvg, plutovg with it via its bundled external/ sources.
    set(SDLTTF_VENDORED ON)
    add_subdirectory(
        ${GE_VENDOR}/github.com/libsdl-org/SDL_ttf
        ${CMAKE_BINARY_DIR}/SDL3_ttf
    )
    target_link_libraries(ge PUBLIC SDL3_ttf::SDL3_ttf)
    # SDL3_ttf::SDL3_ttf does not propagate FreeType headers, but ge/src/text.cpp
    # uses the FreeType C API directly. Pull the bundled headers in by hand.
    target_include_directories(ge PRIVATE
        ${GE_VENDOR}/github.com/libsdl-org/SDL_ttf/external/freetype/include
    )

    # Android system libs. GLESv3 (not GLESv2) for the OpenGL ES 3.1
    # backend bgfx uses on real Adreno/Mali devices.
    target_link_libraries(ge INTERFACE android log EGL GLESv3)

elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # SDL3 and friends — pre-built xcframeworks
    target_link_libraries(ge INTERFACE
        ${GE_VENDOR}/sdl3/xcframeworks/SDL3.xcframework
        ${GE_VENDOR}/sdl3/xcframeworks/SDL3_image.xcframework
        ${GE_VENDOR}/sdl3/xcframeworks/SDL3_ttf.xcframework
        ${GE_VENDOR}/sdl3/xcframeworks/freetype.xcframework
        ${GE_VENDOR}/sdl3/xcframeworks/harfbuzz.xcframework
        ${GE_VENDOR}/sdl3/xcframeworks/plutosvg.xcframework
        ${GE_VENDOR}/sdl3/xcframeworks/plutovg.xcframework
    )
    # The freetype xcframework ships only the static lib — no headers. Take
    # them from the bundled SDL_ttf vendored copy so ge/src/text.cpp can
    # include <ft2build.h>.
    target_include_directories(ge PRIVATE
        ${GE_VENDOR}/github.com/libsdl-org/SDL_ttf/external/freetype/include
    )

    # System frameworks
    target_link_libraries(ge INTERFACE
        "-framework Foundation"
        "-framework UIKit"
        "-framework CoreText"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework IOSurface"
        "-framework OpenGLES"
        "-framework CoreGraphics"
        "-framework CoreFoundation"
        "-framework CoreBluetooth"
        "-framework CoreServices"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework GameController"
        "-framework CoreHaptics"
        "-framework CoreMotion"
        "-framework ImageIO"
        "-framework UniformTypeIdentifiers"
        "-lobjc"
    )
endif()
