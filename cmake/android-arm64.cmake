# ge — Android arm64 link interface for CMake consumers.
#
# Hand-maintained. Consumer's Android CMakeLists.txt sets GE_ROOT, then
# include()s this file. After the include, `ge` is a STATIC IMPORTED
# CMake target the consumer's app library links against:
#
#     set(GE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")
#     include("${GE_ROOT}/cmake/android-arm64.cmake")
#     add_library(main SHARED ${APP_SOURCES})
#     target_link_libraries(main PRIVATE ge)
#
# What this file does:
#   - Declares ge + the first-party vendor static libs (box2d, lunasvg_ge,
#     plutovg_ge, sqlite3_ge, lz4_ge, liteparser) as STATIC IMPORTED,
#     pointing at the prebuilts under ${GE_ROOT}/prebuilt/android-arm64/.
#   - Adds SDL3 + SDL3_image + SDL3_ttf via add_subdirectory of their
#     submodules. SDL ships its own CMakeLists.txt, which is the only
#     reason CMake stays in ge's Android build at all.
#   - Sets public include directories on `ge` so the consumer compiles
#     against the same header surface as the iOS prebuilt path.
#   - Wires the ge → vendor link edges so consumer-side
#     target_link_libraries(main PRIVATE ge) pulls in everything ge needs.
#
# T38 (sokol_gfx migration): bgfx / bx / bimg are no longer prebuilt or
# linked. ge migrated to sokol_gfx — a single-header library compiled
# inside libge.a itself via src/SokolContext_android.cpp. Consumer apps
# that only `target_link_libraries(main PRIVATE ge)` need no source
# changes; the link edges below already cover the new world.
#
# What this file does NOT do:
#   - Generate anything. This file is hand-edited; the prebuild script
#     does not write it. Source-list-shaped contents (the list of
#     IMPORTED libs, the include path list) live here precisely so they
#     are reviewable and stable.
#
# T73.1: this file is part of the cutover that retired the top-level
# CMakeLists.txt's role in compiling ge from sources for Android.

if(NOT DEFINED GE_ROOT)
    message(FATAL_ERROR "cmake/android-arm64.cmake: GE_ROOT must be set before include()")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    message(FATAL_ERROR "cmake/android-arm64.cmake: this file is Android-only "
        "(CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}')")
endif()

set(GE_PREBUILT_DIR "${GE_ROOT}/prebuilt/android-arm64")
set(GE_VENDOR "${GE_ROOT}/vendor")
set(GE_HEADERS "${GE_ROOT}/headers")

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
#
# SDL ships its own CMakeLists.txt. Per ge's "CMake is only acceptable
# when invoking third-party libraries that ship their own CMake build"
# rule, these add_subdirectory calls are allowed. Prebuilding them is a
# separate future scope (see T73 acceptance).

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

# Include directories — match the iOS xcodeproj-gem path
# (tools/ios-build/build_project.rb#header_search_paths) post-T71
# header lifting, so consumers see the same #include landscape on
# both platforms.
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
    # T38: sokol_gfx single-header. Consumer code that includes
    # `sokol_gfx.h` (e.g. their own renderer next to libge.a) needs
    # this on the include path.
    "${GE_VENDOR}/github.com/floooh/sokol"
)

# SQLite defines must be visible to consumers (Tweak.h uses SQLite).
target_compile_definitions(ge INTERFACE
    SQLITE_ENABLE_SESSION
    SQLITE_ENABLE_PREUPDATE_HOOK
    SQLITE_ENABLE_DESERIALIZE
    GE_ANDROID
    GE_DIRECT
    GE_DIRECT_ONLY
)

# Static-linker order matters. Consumer's `target_link_libraries(main
# PRIVATE ge)` pulls in this list in order, so symbols flow:
#   ge → vendor → SDL → Android system.
target_link_libraries(ge INTERFACE
    sqlite3_ge liteparser lz4_ge
    box2d
    lunasvg_ge plutovg_ge
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
    android log EGL GLESv3
)
