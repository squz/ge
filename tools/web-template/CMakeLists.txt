# ge — generic web (Emscripten) consumer build. 🎯T157
#
# ge-owned packaging: consumers do not scaffold or copy this file. The
# Module.mk `web` target configures it with the app's existing Make vars:
#
#     emcmake cmake -B build/web -S $(ge)/tools/web-template \
#         -DGE_ROOT=<abs ge>  -DAPP_ROOT=<abs consumer root> \
#         -DAPP_NAME=<name>   -DAPP_DISPLAY_NAME=<title> \
#         "-DAPP_SOURCES=<abs src;abs src;...>"
#     cmake --build build/web -j
#
# Output: <build>/web/<APP_NAME>.html + .js + .wasm (+ .data when assets
# are preloaded). Serve the build dir over HTTP; opening the file directly
# fails (wasm fetch needs a server).
#
# What it does:
#   - include(cmake/web-wasm.cmake) → the `ge` imported target + SDL.
#   - Compiles ${APP_ROOT}/shaders/*.glsl via sokol-shdc (glsl300es — the
#     only backend a WebGL2 build can select) into <build>/shaders/.
#   - Preloads ${APP_ROOT}/assets and ${APP_ROOT}/fonts into the wasm FS
#     when those directories exist (matching src/Resource.cpp's web base
#     path "/" — e.g. assets land at /assets, fonts at /fonts).
#   - Emits the page from shell.html with the app's display name.

cmake_minimum_required(VERSION 3.24)
project(ge-web-app C CXX)

foreach(_v IN ITEMS GE_ROOT APP_ROOT APP_NAME APP_SOURCES)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "tools/web-template: -D${_v}=... is required")
    endif()
endforeach()
if(NOT DEFINED APP_DISPLAY_NAME)
    set(APP_DISPLAY_NAME "${APP_NAME}")
endif()

include("${GE_ROOT}/cmake/web-wasm.cmake")

# ge's public headers use C++20 (std::span, std::endian); emcc defaults lower.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── app shaders (sokol-shdc → generated headers) ────────────────────

set(SOKOL_SHDC "${GE_ROOT}/vendor/github.com/floooh/sokol-tools-bin/bin/osx_arm64/sokol-shdc")
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")
file(GLOB APP_SHADER_SOURCES "${APP_ROOT}/shaders/*.glsl")
set(APP_SHADER_HEADERS)
foreach(_glsl IN LISTS APP_SHADER_SOURCES)
    get_filename_component(_name "${_glsl}" NAME_WE)
    set(_hdr "${SHADER_OUT_DIR}/${_name}.h")
    add_custom_command(
        OUTPUT "${_hdr}"
        COMMAND "${SOKOL_SHDC}" -i "${_glsl}" -o "${_hdr}" -l glsl300es -f sokol
        DEPENDS "${_glsl}"
        COMMENT "sokol-shdc ${_name}.glsl → ${_name}.h (glsl300es)"
    )
    list(APPEND APP_SHADER_HEADERS "${_hdr}")
endforeach()
add_custom_target(app-shaders DEPENDS ${APP_SHADER_HEADERS})

# ── app executable ──────────────────────────────────────────────────

set(CMAKE_EXECUTABLE_SUFFIX ".html")

add_executable(${APP_NAME} ${APP_SOURCES})
add_dependencies(${APP_NAME} app-shaders)
target_include_directories(${APP_NAME} PRIVATE
    "${APP_ROOT}/src"
    "${APP_ROOT}/include"
    "${APP_ROOT}/vendor/include"
    "${SHADER_OUT_DIR}"
)
target_link_libraries(${APP_NAME} PRIVATE ge)

# ── page shell + preloaded assets ───────────────────────────────────

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
               "${CMAKE_BINARY_DIR}/shell.html" @ONLY)
target_link_options(${APP_NAME} PRIVATE
    "SHELL:--shell-file ${CMAKE_BINARY_DIR}/shell.html")
set_property(TARGET ${APP_NAME} APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_BINARY_DIR}/shell.html")

# Persistence pre-run JS: mounts IDBFS over SDL's pref-path root so
# game.db survives page reloads (🎯T157; see src/StreamHostPolicy.h).
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/pre.js"
               "${CMAKE_BINARY_DIR}/pre.js" COPYONLY)
target_link_options(${APP_NAME} PRIVATE
    "SHELL:--pre-js ${CMAKE_BINARY_DIR}/pre.js")
set_property(TARGET ${APP_NAME} APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_BINARY_DIR}/pre.js")

# Preload conventional asset dirs when present, at the absolute FS paths
# src/Resource.cpp's web base ("/") expects.
foreach(_dir IN ITEMS assets data fonts)
    if(IS_DIRECTORY "${APP_ROOT}/${_dir}")
        target_link_options(${APP_NAME} PRIVATE
            "SHELL:--preload-file ${APP_ROOT}/${_dir}@/${_dir}")
    endif()
endforeach()
