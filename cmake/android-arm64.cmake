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

# ── 16 KB page alignment (🎯T75) ────────────────────────────────────
#
# Google Play rejects submissions whose shared libraries are not 16 KB
# page-aligned (Android 15+ devices ship 16 KB pages; Android 16 hard-
# blocks installs of 4 KB-aligned apps). NDK r28+ links .so files with
# 16 KB-aligned PT_LOAD segments by default, but NDK r27 and earlier
# (and any build that pins an older NDK) default to 4 KB — and the
# consumer's Gradle build, not ge's prebuild.sh, chooses the NDK that
# links the final .so. Make the contract explicit and NDK-version-
# independent here.
#
# This line sits before both the consumer's `add_library(main SHARED …)`
# (which runs in the same directory scope, since include() does not open
# a new scope) and the SDL `add_subdirectory()` calls below (which copy
# this scope's CMAKE_SHARED_LINKER_FLAGS at entry). So every shared
# object in the build — libmain.so, libSDL3.so, libSDL3_image.so,
# libSDL3_ttf.so, and their transitive deps — links 16 KB-aligned.
#
# Verify:  llvm-readelf -lW <lib>.so  → every PT_LOAD shows Align 0x4000.
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-z,max-page-size=16384")

# Debug packages link prebuilt/android-arm64-debug/ (-g -O2 cook of libge +
# every vendor archive — symbols without hiding optimised-only bugs).
# Release keeps prebuilt/android-arm64/ (-O2, no DWARF). Force with
# -DGE_ANDROID_DEBUG_PREBUILT=ON/OFF.
if(NOT DEFINED GE_ANDROID_DEBUG_PREBUILT)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(GE_ANDROID_DEBUG_PREBUILT ON)
    else()
        set(GE_ANDROID_DEBUG_PREBUILT OFF)
    endif()
endif()
if(GE_ANDROID_DEBUG_PREBUILT)
    set(GE_PREBUILT_DIR "${GE_ROOT}/prebuilt/android-arm64-debug")
else()
    set(GE_PREBUILT_DIR "${GE_ROOT}/prebuilt/android-arm64")
endif()
if(NOT EXISTS "${GE_PREBUILT_DIR}/libge.a")
    if(GE_ANDROID_DEBUG_PREBUILT)
        message(FATAL_ERROR
            "cmake/android-arm64.cmake: missing debug prebuilts at\n"
            "  ${GE_PREBUILT_DIR}\n"
            "Cook them with:\n"
            "  tools/prebuild.sh --debug android-arm64\n"
            "(or: make prebuild-android-arm64-debug)")
    else()
        message(FATAL_ERROR
            "cmake/android-arm64.cmake: missing prebuilts at\n"
            "  ${GE_PREBUILT_DIR}\n"
            "Cook them with: tools/prebuild.sh android-arm64")
    endif()
endif()

# Hard co-cook gate (same tool as iOS Xcode phase + ensure-prebuilt.sh):
# every .a must match cook.json from a single full prebuild. Prevents
# linking a fresh libge against a stale libliteparser (2026-07 Android
# sqldeep SIGSEGV). No human guideline can substitute.
if(GE_ANDROID_DEBUG_PREBUILT)
    set(_GE_PREBUILD_HINT "tools/prebuild.sh --debug android-arm64")
else()
    set(_GE_PREBUILD_HINT "tools/prebuild.sh android-arm64")
endif()
find_program(_GE_PYTHON3 NAMES python3 python REQUIRED)
execute_process(
    COMMAND "${_GE_PYTHON3}" "${GE_ROOT}/tools/verify-cook.py" "${GE_PREBUILT_DIR}"
    RESULT_VARIABLE _GE_COOK_RC
    OUTPUT_VARIABLE _GE_COOK_OUT
    ERROR_VARIABLE _GE_COOK_ERR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT _GE_COOK_RC EQUAL 0)
    message(FATAL_ERROR
        "cmake/android-arm64.cmake: prebuilt cook check failed\n"
        "  dir: ${GE_PREBUILT_DIR}\n"
        "  ${_GE_COOK_ERR}\n"
        "  ${_GE_COOK_OUT}\n"
        "Run: ${_GE_PREBUILD_HINT}")
endif()
set(_GE_PREBUILT_LIBS ge box2d lunasvg_ge plutovg_ge sqlite3_ge lz4_ge liteparser)
message(STATUS "ge: prebuilts from ${GE_PREBUILT_DIR}"
    " (debug=${GE_ANDROID_DEBUG_PREBUILT}, cook verified)")

set(GE_VENDOR "${GE_ROOT}/vendor")
set(GE_HEADERS "${GE_ROOT}/headers")

# ── ge + first-party vendor static libs (prebuilt) ──────────────────

foreach(_lib IN LISTS _GE_PREBUILT_LIBS)
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
    # SDL3_ttf links its vendored FreeType/HarfBuzz *privately*, so their
    # symbols don't reach a consumer's libmain.so. libge.a's text.cpp
    # (ge::rasterizeText / ge::debug::text) references FT_* directly, so a
    # consumer that actually pulls text.o (e.g. tiltbuggy's debug overlay)
    # needs FreeType on its own link line. Static linking only pulls these
    # in on demand, so consumers that don't use text (e.g. multimaze2) are
    # unaffected. (🎯T30 / 🎯T107 prerequisite.)
    freetype harfbuzz
    # 🎯T107: libge.a's SokolContext_android (VkM glue) + ge_vkprobe.c reference
    # Vulkan 1.0 core + KHR_surface/KHR_swapchain + vkGetInstanceProcAddr. Every
    # such symbol is exported by Android's libvulkan.so loader (mandatory since
    # API 24, < our minSdk 26), so this costs no minSdk. The 1.1/1.2/1.3 core
    # sokol's IMPL needs is NOT linked here — it's resolved inside libgesokol-vk.so.
    vulkan
    android log EGL GLESv3
)

# 🎯T119: ge's JNI entry points (ge.GeActivity native methods) live in the
# prebuilt static libge.a and are referenced only from Java, so the NDK's
# default --gc-sections strips them from the consumer's libmain.so before they
# reach the dynamic symbol table — the JVM's dlsym() then can't find them. A
# version script keeps + exports the Java_ge_* set (and is inherited by every
# consumer via this INTERFACE target). Purely additive — no `local:` clause, so
# SDL's own exports are untouched. See cmake/ge-android-exports.version.
target_link_options(ge INTERFACE
    "LINKER:--version-script=${CMAKE_CURRENT_LIST_DIR}/ge-android-exports.version")
