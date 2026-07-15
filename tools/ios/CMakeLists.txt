# ge stream player — iOS (device + simulator).
# ge-owned packaging: games do not maintain this.
#
# Three orientation packaging variants (🎯T154.3) — same binary code, different
# Info.plist UISupportedInterfaceOrientations (knob 1 of the two-knob lock):
#   Player      — don't-care (all orientations) — com.squz.player
#   PlayerLand  — landscape-only               — com.squz.player.land
#   PlayerPort  — portrait-only                — com.squz.player.port
#
#   cd tools/ios && cmake -B build/xcode -G Xcode
#   cmake --build build/xcode --config Debug --target PlayerLand -- -sdk iphonesimulator
cmake_minimum_required(VERSION 3.28)
project(Player LANGUAGES C CXX OBJCXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_XCODE_LINK_BUILD_PHASE_MODE KNOWN_LOCATION)

set(GE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(LUNASVG_DIR ${GE_ROOT}/vendor/github.com/sammycage/lunasvg)
file(GLOB LUNASVG_SRC ${LUNASVG_DIR}/source/*.cpp)
file(GLOB PLUTOVG_SRC ${LUNASVG_DIR}/plutovg/source/*.c)

set(PLAYER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/QRScanner.mm
    ${CMAKE_CURRENT_SOURCE_DIR}/KeychainStore.mm
    ${GE_ROOT}/tools/player_core.cpp
    ${GE_ROOT}/tools/player_orientation_ios.mm
    ${GE_ROOT}/tools/AudioPlayer.cpp
    ${GE_ROOT}/src/bridge/PlayerWireBridge.cpp
    ${GE_ROOT}/src/bridge/WebSocketClient.cpp
    ${GE_ROOT}/src/bridge/VideoDecoder_apple.mm
    ${GE_ROOT}/src/render/PlayerRender.cpp
    ${GE_ROOT}/src/CmdStream.cpp
    ${GE_ROOT}/src/Signal.cpp
    ${GE_ROOT}/src/Resource.cpp
    ${GE_ROOT}/src/FileIO.cpp
    ${GE_ROOT}/src/audio.cpp
    ${GE_ROOT}/src/svg.cpp
    ${GE_ROOT}/src/text.cpp
    ${GE_ROOT}/src/FontLoader_apple.mm
    ${GE_ROOT}/src/Immersive_apple.mm
    ${GE_ROOT}/src/CutoutInsets_stub.cpp

    # Lean player stubs (no full appchannel / sokol sprite path)
    ${GE_ROOT}/tools/android/app/src/main/cpp/player_stubs.cpp
    ${GE_ROOT}/tools/android/app/src/main/cpp/player_sprite_stub.cpp
    ${GE_ROOT}/vendor/src/sqlite3.c
    ${GE_ROOT}/vendor/src/sqlpipe.cpp
    # sqlift is bundled inside sqlpipe.cpp — do not also link sqlift.cpp
    ${GE_ROOT}/vendor/src/lz4.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/arena.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/liteparser.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/lp_tokenize.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/lp_unparse.c
    ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src/parse.c
    ${LUNASVG_SRC}
    ${PLUTOVG_SRC}
)

set(SDL_DEVICE_DIR ${GE_ROOT}/vendor/sdl3/lib/ios-arm64)
set(SDL_SIM_DIR ${GE_ROOT}/vendor/sdl3/lib/ios-arm64-simulator)

# add_player_variant(<cmake_target> <bundle_id> <info_plist> <xcassets_dir>)
function(add_player_variant target_name bundle_id info_plist xcassets_dir)
    add_executable(${target_name} MACOSX_BUNDLE ${PLAYER_SOURCES})

    target_compile_definitions(${target_name} PRIVATE
        ASIO_STANDALONE
        GE_IOS
        GE_PLAYER_NO_SOKOL
        SQLITE_ENABLE_SESSION
        SQLITE_ENABLE_PREUPDATE_HOOK
        SQLITE_ENABLE_DESERIALIZE
        LUNASVG_BUILD_STATIC
    )

    target_include_directories(${target_name} PRIVATE
        ${GE_ROOT}/include
        ${GE_ROOT}/vendor/include
        ${GE_ROOT}/vendor/github.com/gabime/spdlog/include
        ${GE_ROOT}/vendor/github.com/chriskohlhoff/asio/include
        ${GE_ROOT}/vendor/github.com/libsdl-org/SDL/include
        ${GE_ROOT}/vendor/sdl3/include
        ${GE_ROOT}/vendor/github.com/floooh/sokol
        ${GE_ROOT}/vendor/github.com/libsdl-org/SDL_ttf/external/freetype/include
        ${GE_ROOT}/vendor/github.com/marcelocantos/deepparser/src
        ${GE_ROOT}/vendor/github.com/sammycage/lunasvg/include
        ${GE_ROOT}/vendor/github.com/sammycage/lunasvg/plutovg/include
        ${GE_ROOT}/tools
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(${target_name} PRIVATE
        ${SDL_DEVICE_DIR}/libSDL3.a
        ${SDL_DEVICE_DIR}/libSDL3_image.a
        ${SDL_DEVICE_DIR}/libSDL3_ttf.a
        ${SDL_DEVICE_DIR}/libfreetype.a
        ${SDL_DEVICE_DIR}/libharfbuzz.a
        ${SDL_DEVICE_DIR}/libplutosvg.a
        ${SDL_DEVICE_DIR}/libplutovg.a
    )

    target_link_libraries(${target_name} PRIVATE
        "-framework Foundation"
        "-framework UIKit"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework CoreGraphics"
        "-framework CoreFoundation"
        "-framework CoreText"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework GameController"
        "-framework CoreHaptics"
        "-framework CoreMotion"
        "-framework VideoToolbox"
        "-framework Security"
        "-framework ImageIO"
        "-framework OpenGLES"
        "-framework MobileCoreServices"
        "-framework CFNetwork"
        "-framework CoreBluetooth"
        "-lobjc"
    )

    # App icon catalog (distinct per variant — L / P badges on Land / Port).
    set(_assets "${CMAKE_CURRENT_SOURCE_DIR}/${xcassets_dir}")
    if(EXISTS "${_assets}")
        target_sources(${target_name} PRIVATE "${_assets}")
        set_source_files_properties("${_assets}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
    endif()

    set_target_properties(${target_name} PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER ${bundle_id}
        MACOSX_BUNDLE_BUNDLE_VERSION 1
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/${info_plist}
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${bundle_id}
        XCODE_ATTRIBUTE_PRODUCT_NAME ${target_name}
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "SWA3H3N7TW"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_GENERATE_SCHEME TRUE
        XCODE_SCHEME_ENABLE_GPU_API_VALIDATION OFF
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 16.3
        "XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS[sdk=iphoneos*]" "${SDL_DEVICE_DIR}"
        "XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]" "${SDL_SIM_DIR}"
    )
endfunction()

# Don't-care keeps the historic name "Player" / com.squz.player.
add_player_variant(Player     com.squz.player      Info.plist      Assets-Player.xcassets)
add_player_variant(PlayerLand com.squz.player.land Info-Land.plist Assets-PlayerLand.xcassets)
add_player_variant(PlayerPort com.squz.player.port Info-Port.plist Assets-PlayerPort.xcassets)
