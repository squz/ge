# ge — web (Emscripten/wasm, WebGL2) link interface for CMake consumers. 🎯T157
#
# Hand-maintained, cloned from cmake/android-arm64.cmake. The consumer's web
# CMakeLists (normally ge's own tools/web-template/CMakeLists.txt) sets
# GE_ROOT, then include()s this file. After the include, `ge` is a STATIC
# IMPORTED CMake target the consumer's executable links against:
#
#     set(GE_ROOT "...")
#     include("${GE_ROOT}/cmake/web-wasm.cmake")
#     add_executable(myapp ${APP_SOURCES})
#     target_link_libraries(myapp PRIVATE ge)
#
# What this file does:
#   - Declares ge + the first-party vendor static libs as STATIC IMPORTED,
#     pointing at ${GE_ROOT}/prebuilt/web-wasm/ (cooked by
#     tools/prebuild.sh web-wasm).
#   - Appends the compile flags the prebuilt was cooked with
#     (-fwasm-exceptions -msimd128 -msse2) to this scope's C/CXX flags so
#     consumer sources and the SDL subdirectory builds match the archive ABI.
#   - Adds SDL3 + SDL3_image + SDL3_ttf via add_subdirectory (static).
#   - Sets the same include / define surface as the other platform lanes.
#   - Wires the Emscripten link options every ge web executable needs
#     (WebGL2, wasm exceptions, memory growth, IDBFS for persistence).
#
# Unlike Android there is no dispatch shim / second backend: the browser has
# exactly one backend (WebGL2), so SOKOL_IMPL lives inline in libge.a
# (src/SokolContext_web.cpp) — the Apple model. WebGPU is 🎯T157.1.

if(NOT DEFINED GE_ROOT)
    message(FATAL_ERROR "cmake/web-wasm.cmake: GE_ROOT must be set before include()")
endif()
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "cmake/web-wasm.cmake: this file is Emscripten-only "
        "(configure with emcmake; CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}')")
endif()

set(GE_PREBUILT_DIR "${GE_ROOT}/prebuilt/web-wasm")
set(GE_VENDOR "${GE_ROOT}/vendor")
set(GE_HEADERS "${GE_ROOT}/headers")

# ── ABI-matching compile flags ──────────────────────────────────────
#
# Must mirror tools/prebuild.sh's web-wasm case: JS-based exceptions (emcc
# disables catching by default; ge relies on exceptions; -fwasm-exceptions
# is ruled out because Asyncify — the run-loop suspend — cannot instrument
# wasm-EH frames) and box2d's SSE2-over-wasm-SIMD path. include() does not
# open a scope, and the SDL add_subdirectory calls below copy these flags
# at entry, so consumer sources and every SDL-family library compile
# identically.
string(APPEND CMAKE_C_FLAGS " -fexceptions -msimd128 -msse2")
string(APPEND CMAKE_CXX_FLAGS " -fexceptions -msimd128 -msse2")

# ── ge + first-party vendor static libs (prebuilt) ──────────────────

foreach(_lib IN ITEMS
        ge
        box2d
        lunasvg_ge plutovg_ge
        sqlite3_ge lz4_ge liteparser)
    add_library(${_lib} STATIC IMPORTED GLOBAL)
    set_target_properties(${_lib} PROPERTIES
        IMPORTED_LOCATION "${GE_PREBUILT_DIR}/lib${_lib}.a"
    )
endforeach()

# ── SDL3 + SDL3_image + SDL3_ttf (third-party CMake, built from source) ──

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${GE_VENDOR}/github.com/libsdl-org/SDL"
    "${CMAKE_BINARY_DIR}/SDL3"
)

set(SDL3IMAGE_VENDORED OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${GE_VENDOR}/github.com/libsdl-org/SDL_image"
    "${CMAKE_BINARY_DIR}/SDL3_image"
)

set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
add_subdirectory(
    "${GE_VENDOR}/github.com/libsdl-org/SDL_ttf"
    "${CMAKE_BINARY_DIR}/SDL3_ttf"
)

# ── ge link / include surface ──────────────────────────────────────

target_include_directories(ge INTERFACE
    "${GE_ROOT}/include"
    "${GE_VENDOR}/include"
    "${GE_HEADERS}/spdlog/include"
    "${GE_HEADERS}/asio/include"
    "${GE_HEADERS}/sdl3/include"
    "${GE_VENDOR}/sdl3/include"
    "${GE_HEADERS}/liteparser/include"
    "${GE_HEADERS}/lunasvg/include"
    "${GE_HEADERS}/plutovg/include"
    "${GE_HEADERS}/box2d/include"
    "${GE_VENDOR}/github.com/floooh/sokol"
)

target_compile_definitions(ge INTERFACE
    SQLITE_ENABLE_SESSION
    SQLITE_ENABLE_PREUPDATE_HOOK
    SQLITE_ENABLE_DESERIALIZE
    GE_WEB
    GE_DIRECT
    GE_DIRECT_ONLY
)

# Static-linker order matters: ge → vendor → SDL.
target_link_libraries(ge INTERFACE
    sqlite3_ge liteparser lz4_ge
    box2d
    lunasvg_ge plutovg_ge
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
    # Same rationale as Android: SDL3_ttf links its vendored FreeType/
    # HarfBuzz privately; libge.a's text.cpp references FT_* directly, so
    # consumers that pull text.o need them on the link line.
    freetype harfbuzz
)

# Emscripten link surface every ge web executable needs:
#   -fexceptions           — must match the compile flag above (JS-based EH;
#                            enables exception catching at link).
#   -sUSE_WEBGL2 / MIN=2   — sokol's GLES3 calls target a WebGL2 context.
#   -sALLOW_MEMORY_GROWTH  — games allocate past the 16 MB default heap.
#   -sSTACK_SIZE=1MB       — emcc's 64 KB default is tight for game code.
#   -lidbfs.js             — IDBFS for reload-surviving persistence (🎯T157
#                            phase: game.db under SDL_GetPrefPath).
#   -sASYNCIFY             — ge::run's blocking loop suspends between frames
#                            via ge_web_await_frame (SessionHost.mm). Scoped:
#                            IGNORE_INDIRECT keeps instrumentation to the
#                            direct main → ge::run → runDirectHosted chain
#                            (the suspend point is never reached through an
#                            indirect call), so game callbacks pay nothing.
target_link_options(ge INTERFACE
    -fexceptions
    -sUSE_WEBGL2=1
    -sMIN_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=1048576
    -lidbfs.js
    -sASYNCIFY
    -sASYNCIFY_IGNORE_INDIRECT=1
    -sASYNCIFY_STACK_SIZE=65536
)
