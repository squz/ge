# ge engine module
# Included by a consuming project's Makefile.
#
# Typical usage — a minimal app Makefile looks like:
#
#   ge          := ge
#   APP_NAME    := mygame
#   APP_SRC     := src/main.cpp src/Scene.cpp
#   APP_SHADERS := build/shaders/simple_vs.bin build/shaders/simple_fs.bin
#
#   -include $(ge)/Module.mk
#   $(ge)/Module.mk:
#           git submodule update --init --recursive
#
# Module.mk derives the binary path ($(APP)=bin/$(APP_NAME)), object list,
# link rule, compile rule, default `all` target and `run`/`clean`.
#
# The `ge` variable is the relative path from the app Makefile to the ge
# repository root. Submodule apps use the default `ge := ge`. In-tree
# samples that live inside the ge repo set it to `../..` etc.
#
# Output paths under $(BUILD_DIR) always use a literal `ge/` namespace
# so objects land in sane locations regardless of where `$(ge)` points.
ge ?= ge

# ────────────────────────────────────────────────
# Build configuration (app-overridable)
# ────────────────────────────────────────────────

BUILD_DIR ?= build
CXX       ?= clang++
CC        ?= clang

# ────────────────────────────────────────────────
# make controls
# ────────────────────────────────────────────────

MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# ────────────────────────────────────────────────
# Variables exported to parent
# ────────────────────────────────────────────────

ge/INCLUDES = \
	-I$(ge)/include \
	-I$(ge)/vendor/include \
	-I$(ge)/vendor/github.com/gabime/spdlog/include \
	-I$(ge)/vendor/github.com/libsdl-org/SDL/include \
	-I$(ge)/vendor/sdl3/include \
	-I$(ge)/vendor/github.com/erincatto/box2d/include \
	-I$(ge)/vendor/github.com/chriskohlhoff/asio/include \
	-I$(ge)/vendor/github.com/sqliteai/liteparser/src \
	-I$(ge)/vendor/github.com/sammycage/lunasvg/include \
	-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_DESERIALIZE \
	-DLUNASVG_BUILD_STATIC

# bgfx + bx + bimg (vendored, compiled from source)
ge/BX_DIR = $(ge)/vendor/github.com/bkaradzic/bx
ge/BIMG_DIR = $(ge)/vendor/github.com/bkaradzic/bimg
ge/BGFX_DIR = $(ge)/vendor/github.com/bkaradzic/bgfx

ge/BX_INCLUDES = -I$(ge/BX_DIR)/include -I$(ge/BX_DIR)/include/compat/osx
ge/BIMG_INCLUDES = -I$(ge/BIMG_DIR)/include
ge/BGFX_INCLUDES = -I$(ge/BGFX_DIR)/include

ge/BGFX_ALL_INCLUDES = $(ge/BGFX_INCLUDES) $(ge/BX_INCLUDES) $(ge/BIMG_INCLUDES)
ge/BGFX_CXXFLAGS = -std=c++20 -O2 $(ge/BGFX_ALL_INCLUDES) -DBX_CONFIG_DEBUG=0 -DBGFX_CONFIG_RENDERER_METAL=1

# bx: platform abstraction (amalgamated minus crtnone.cpp — handled by #ifdef internally)
ge/BX_OBJ = $(BUILD_DIR)/ge/vendor/bx.o
ge/BX_LIB = $(BUILD_DIR)/libbx.a

# bimg: image processing (image.cpp + image_gnf.cpp — decode/encode not needed for bgfx runtime)
ge/BIMG_SRC = $(ge/BIMG_DIR)/src/image.cpp $(ge/BIMG_DIR)/src/image_gnf.cpp
ge/BIMG_OBJ = $(patsubst $(ge/BIMG_DIR)/src/%.cpp,$(BUILD_DIR)/ge/vendor/bimg_%.o,$(ge/BIMG_SRC))
ge/BIMG_LIB = $(BUILD_DIR)/libbimg.a

# bgfx: rendering
ge/BGFX_OBJ = $(BUILD_DIR)/ge/vendor/bgfx.o
ge/BGFX_LIB = $(BUILD_DIR)/libbgfx.a

ge/BGFX_LIBS = $(ge/BGFX_LIB) $(ge/BIMG_LIB) $(ge/BX_LIB)

# SDL3 libraries (static, vendored)
ge/SDL3_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libSDL3.a
ge/SDL3_IMAGE_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libSDL3_image.a
ge/SDL3_TTF_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libSDL3_ttf.a
ge/FREETYPE_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libfreetype.a
ge/HARFBUZZ_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libharfbuzz.a
ge/PLUTOSVG_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libplutosvg.a
ge/PLUTOVG_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libplutovg.a
ge/SDL_LIBS = $(ge/SDL3_LIB) $(ge/SDL3_IMAGE_LIB) $(ge/SDL3_TTF_LIB) $(ge/FREETYPE_LIB) $(ge/HARFBUZZ_LIB) $(ge/PLUTOSVG_LIB) $(ge/PLUTOVG_LIB)

# macOS frameworks needed by any ge desktop app (SDL3 + bgfx + VideoToolbox +
# CoreMotion). Apps can extend via FRAMEWORKS += ... after the include.
ge/FRAMEWORKS = \
    -framework Metal -framework MetalKit -framework QuartzCore \
    -framework Cocoa -framework IOKit -framework CoreFoundation \
    -framework Carbon -framework CoreAudio -framework AudioToolbox \
    -framework CoreHaptics -framework GameController -framework CoreVideo \
    -framework ForceFeedback -framework AVFoundation -framework CoreMedia \
    -framework UniformTypeIdentifiers -framework CoreGraphics \
    -framework VideoToolbox -framework CoreMotion

# Core engine sources — always needed. Split into "direct-only" (runs on
# any platform / modality) and "brokered" (only the server-side of the
# ged-paired modality) so mobile distribution builds can omit the latter.
ge/SRC_DIRECT = \
	$(ge)/src/Context.cpp \
	$(ge)/src/Resource.cpp \
	$(ge)/src/FileIO.cpp \
	$(ge)/src/FontLoader_apple.mm \
	$(ge)/src/BgfxContext.mm \
	$(ge)/src/Signal.cpp \
	$(ge)/src/SessionHost.mm \
	$(ge)/src/Immersive_stub.cpp \
	$(ge)/src/CutoutInsets_stub.cpp \
	$(ge)/src/Attitude_apple.mm \
	$(ge)/src/SvgRasterizer.cpp \
	$(ge)/src/SvgSprite.cpp \
	$(ge)/src/LoadTexture.cpp \
	$(ge)/src/render/DirectRenderHost.mm \
	$(ge)/tools/player_orientation_stub.cpp

ge/SRC_BROKERED = \
	$(ge)/src/bridge/SessionHost_brokered.mm \
	$(ge)/src/render/PlayerRender.cpp \
	$(ge)/src/bridge/ServerWireBridge.mm \
	$(ge)/src/bridge/PlayerWireBridge.cpp \
	$(ge)/src/bridge/WebSocketClient.cpp \
	$(ge)/src/bridge/VideoEncoder_apple.mm \
	$(ge)/src/bridge/VideoDecoder_apple.mm

ge/SRC = $(ge/SRC_DIRECT) $(ge/SRC_BROKERED)

ge/OBJ = $(patsubst $(ge)/src/%.cpp,$(BUILD_DIR)/ge/src/%.o,$(filter $(ge)/src/%.cpp,$(ge/SRC))) \
         $(patsubst $(ge)/src/%.mm,$(BUILD_DIR)/ge/src/%.o,$(filter $(ge)/src/%.mm,$(ge/SRC))) \
         $(patsubst $(ge)/tools/%.cpp,$(BUILD_DIR)/ge/tools/%.o,$(filter $(ge)/tools/%.cpp,$(ge/SRC))) \
         $(patsubst $(ge)/tools/%.mm,$(BUILD_DIR)/ge/tools/%.o,$(filter $(ge)/tools/%.mm,$(ge/SRC)))
ge/LIB = $(BUILD_DIR)/libge.a

# Desktop player (H.264 receiver). Built on demand via `make player`.
ge/PLAYER_SRC = $(ge)/tools/player.cpp $(ge)/tools/player_core.cpp $(ge)/tools/player_orientation_stub.cpp
ge/PLAYER = bin/player

# Small helper CLIs, built on demand.
ge/IMGDIFF = bin/imgdiff

# bgfx shader compiler (vendored binaries for common hosts).
# Parent lists desired `.bin` outputs (e.g. `$(BUILD_DIR)/shaders/foo_vs.bin`);
# Module.mk's pattern rules compile them from matching `.sc` sources.
ge/SHADERC_HOST := $(shell uname -s | tr A-Z a-z)-$(shell uname -m | sed 's/aarch64/arm64/')
ge/SHADERC = $(ge)/bin/$(ge/SHADERC_HOST)/shaderc

# bgfx shader include directory (bgfx_shader.sh lives here).
ge/SHADERC_BGFX_INCLUDE = $(ge/BGFX_DIR)/src

# Target GPU profile (Metal on Apple platforms — the only renderer enabled).
ge/SHADERC_PROFILE ?= metal
ge/SHADERC_PLATFORM ?= osx

# Parent defines its shader source directory (default `shaders`) and varying def.
ge/SHADER_DIR ?= shaders
ge/SHADERC_VARYINGDEF ?= $(ge/SHADER_DIR)/varying.def.sc

# ge's own internal render shaders (compose pass for viewport tilt).
# Consumers depend on $(ge/RENDER_SHADERS) so the binaries exist on
# disk at runtime. DirectRenderHost loads them from "build/ge/shaders/".
ge/RENDER_SHADER_DIR = $(ge)/src/render/shaders
ge/RENDER_SHADERS = \
	$(BUILD_DIR)/ge/shaders/ge_compose_vs.bin \
	$(BUILD_DIR)/ge/shaders/ge_compose_fs.bin

# Android shader variants. The APK ships BOTH so the runtime can pick
# based on the bgfx backend BgfxContext.mm chose for this device:
#   * shaders-spirv/ — for the Vulkan backend (Apple-Silicon emulator).
#   * shaders-gles/  — for the OpenGL ES backend (real Adreno/Mali devices).
# Consumed by the Gradle syncAssets task; both deposited into the APK
# under assets/build/shaders-{spirv,gles}/ and assets/build/ge/shaders-{spirv,gles}/.
# Engine helper ge::shaderDir() returns the right suffix at runtime.
ge/APP_SHADERS_SPIRV    = $(patsubst $(BUILD_DIR)/$(ge/SHADER_DIR)/%.bin,$(BUILD_DIR)/$(ge/SHADER_DIR)-spirv/%.bin,$(APP_SHADERS))
ge/RENDER_SHADERS_SPIRV = $(patsubst $(BUILD_DIR)/ge/shaders/%.bin,$(BUILD_DIR)/ge/shaders-spirv/%.bin,$(ge/RENDER_SHADERS))
ge/APP_SHADERS_GLES     = $(patsubst $(BUILD_DIR)/$(ge/SHADER_DIR)/%.bin,$(BUILD_DIR)/$(ge/SHADER_DIR)-gles/%.bin,$(APP_SHADERS))
ge/RENDER_SHADERS_GLES  = $(patsubst $(BUILD_DIR)/ge/shaders/%.bin,$(BUILD_DIR)/ge/shaders-gles/%.bin,$(ge/RENDER_SHADERS))

# Texture encoder (used by precompute tools, NOT part of libge.a)
ge/TEXTURE_ENCODER_SRC = $(ge)/src/TextureEncoder.cpp
ge/TEXTURE_ENCODER_OBJ = $(BUILD_DIR)/ge/src/TextureEncoder.o

# Box2D v3 physics library (C code)
ge/BOX2D_DIR = $(ge)/vendor/github.com/erincatto/box2d
ge/BOX2D_SRC = $(wildcard $(ge/BOX2D_DIR)/src/*.c)
ge/BOX2D_OBJ = $(patsubst $(ge/BOX2D_DIR)/src/%.c,$(BUILD_DIR)/ge/vendor/box2d/%.o,$(ge/BOX2D_SRC))
ge/BOX2D_CFLAGS = -I$(ge/BOX2D_DIR)/include -I$(ge/BOX2D_DIR)/src -O2 -std=c17

# SQLite3 (C code, vendored amalgamation — used by Tweak.h)
ge/SQLITE_SRC = $(ge)/vendor/src/sqlite3.c
ge/SQLITE_OBJ = $(BUILD_DIR)/ge/vendor/sqlite3.o

# Triangle library (C code, used by build-time precompute tools in
# consuming projects). OPT-IN ONLY — not linked into libge.a. Reference
# $(ge/TRIANGLE_OBJ) in your link line explicitly to pull it in.
#
# LICENCE WARNING: Triangle is NOT permissively licensed. Commercial
# distribution requires direct arrangement with Jonathan Shewchuk
# (jrs@cs.berkeley.edu). See NOTICES.md "Triangle (J. R. Shewchuk)"
# for the full terms and the three options if you're shipping a paid
# product. Safe default for commercial builds: do not reference
# $(ge/TRIANGLE_OBJ) anywhere in your Makefile.
ge/TRIANGLE_SRC = $(ge)/vendor/src/triangle.c
ge/TRIANGLE_OBJ = $(BUILD_DIR)/ge/vendor/triangle.o
ge/TRIANGLE_CFLAGS = -O2 -I$(ge)/vendor/include -DTRILIBRARY -DREAL=double -DANSI_DECLARATORS -DNO_TIMER

# lz4 compression (C code, used by sqlpipe)
ge/LZ4_SRC = $(ge)/vendor/src/lz4.c
ge/LZ4_OBJ = $(BUILD_DIR)/ge/vendor/lz4.o

# lunasvg + bundled plutovg (vendored from sammycage/lunasvg).
#
# lunasvg is the canonical SVG rasterizer used by `ge::rasterizeSvg`. It depends
# on plutovg for 2D rendering; lunasvg ships its own pinned plutovg as a nested
# subdirectory and we compile that copy rather than the SDL3-prebuilt
# libplutovg.a (which is a much older 0.0.x release used by SDL_ttf for color
# emoji glyphs, see vendor/sdl3/lib/macos-arm64/libplutosvg.a). The two are not
# symbol-compatible; to avoid clashes at app link time, lunasvg's plutovg is
# kept namespace-isolated — see the rename header / `-include` flag below
# (added in T42.2 when the public API ships).
ge/LUNASVG_DIR = $(ge)/vendor/github.com/sammycage/lunasvg
ge/PLUTOVG_DIR = $(ge/LUNASVG_DIR)/plutovg

ge/LUNASVG_SRC = $(wildcard $(ge/LUNASVG_DIR)/source/*.cpp)
ge/LUNASVG_OBJ = $(patsubst $(ge/LUNASVG_DIR)/source/%.cpp,$(BUILD_DIR)/ge/vendor/lunasvg/%.o,$(ge/LUNASVG_SRC))

ge/PLUTOVG_SRC = $(wildcard $(ge/PLUTOVG_DIR)/source/*.c)
ge/PLUTOVG_OBJ = $(patsubst $(ge/PLUTOVG_DIR)/source/%.c,$(BUILD_DIR)/ge/vendor/plutovg/%.o,$(ge/PLUTOVG_SRC))

ge/LUNASVG_CXXFLAGS = -O2 -std=c++17 -fvisibility=hidden \
    -DLUNASVG_BUILD -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC \
    -I$(ge/LUNASVG_DIR)/include -I$(ge/LUNASVG_DIR)/source -I$(ge/PLUTOVG_DIR)/include
ge/PLUTOVG_CFLAGS = -O2 -std=c11 -fvisibility=hidden \
    -DPLUTOVG_BUILD -DPLUTOVG_BUILD_STATIC \
    -I$(ge/PLUTOVG_DIR)/include -I$(ge/PLUTOVG_DIR)/source

# liteparser (C code, used by sqlpipe for query analysis)
ge/LITEPARSER_DIR = $(ge)/vendor/github.com/sqliteai/liteparser/src
ge/LITEPARSER_SRC = $(addprefix $(ge/LITEPARSER_DIR)/,arena.c liteparser.c lp_tokenize.c lp_unparse.c parse.c)
ge/LITEPARSER_OBJ = $(patsubst $(ge/LITEPARSER_DIR)/%.c,$(BUILD_DIR)/ge/vendor/liteparser/%.o,$(ge/LITEPARSER_SRC))

# Vendor C++ libraries (compiled into libge.a)
ge/VENDOR_CPP_SRC = $(ge)/vendor/src/sqlift.cpp $(ge)/vendor/src/sqlpipe.cpp
ge/VENDOR_CPP_OBJ = $(patsubst $(ge)/vendor/src/%.cpp,$(BUILD_DIR)/ge/vendor/%.o,$(ge/VENDOR_CPP_SRC))

# Test sources
ge/TEST_SRC = \
	$(ge)/src/main_test.cpp \
	$(ge)/src/DampedRotation_test.cpp \
	$(ge)/src/Rect_test.cpp \
	$(ge)/src/SvgRasterizer_test.cpp \
	$(ge)/src/LoadTexture_test.cpp
ge/TEST_OBJ = $(patsubst $(ge)/src/%.cpp,$(BUILD_DIR)/ge/src/%.o,$(ge/TEST_SRC))

# Shared variables (parent can += to extend)
CLEAN = bin build deps.dot deps.svg deps.png $(ge)/ged/web
COMPILE_DB_DEPS = $(ge/SRC) $(ge/TEST_SRC) $(ge)/Module.mk $(APP_SRC) Makefile
ge/DEPGRAPH_DEPS = $(ge/SRC) $(wildcard $(ge)/include/ge/*.h) $(ge)/tools/depgraph.py

# ────────────────────────────────────────────────
# Default compile/link flags (app-overridable)
# ────────────────────────────────────────────────

# Engine-managed base flags. Apps that want to extend CXXFLAGS keep these by
# default (via `CXXFLAGS ?=` below) or reference $(ge/CXXFLAGS_BASE) explicitly
# when constructing their own.
ge/CXXFLAGS_BASE = -std=c++20 -O2 -g $(ge/INCLUDES) $(ge/BGFX_ALL_INCLUDES) -DBX_CONFIG_DEBUG=0

CXXFLAGS   ?= $(ge/CXXFLAGS_BASE) -Isrc
SDL_CFLAGS ?= -I$(ge)/vendor/sdl3/include
FRAMEWORKS ?= $(ge/FRAMEWORKS)

# ────────────────────────────────────────────────
# App convention — parent declares APP_NAME / APP_SRC / APP_SHADERS
# ────────────────────────────────────────────────

# Derived from the parent's APP_NAME and APP_SRC. The parent can override
# $(APP) (e.g. to change the binary location) or $(APP_OBJ) (unusual) before
# the include.
APP         ?= bin/$(APP_NAME)
APP_OBJ     ?= $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(APP_SRC))

# Display name used for iOS / Android bundles and Xcode targets/schemes.
# Defaults to APP_NAME; set to a Pascal-cased variant if you want a pretty
# string on the home screen while keeping a lowercase binary name.
APP_DISPLAY_NAME ?= $(APP_NAME)

# Extra static libs/objects the app needs beyond the ge engine (e.g. ship a
# specialised third-party library). Defaults to Box2D since many ge apps use
# it and its link cost is negligible for those that don't.
APP_LIBS    ?= $(ge/BOX2D_OBJ)

# ────────────────────────────────────────────────
# Rules
# ────────────────────────────────────────────────

# Default target — `make` with no args builds the app. Parent can declare its
# own `all:` BEFORE the include to win (the first target make sees is the
# default).
.PHONY: all run
all: $(APP)

# Default link rule. Parent can override by declaring its own $(APP) rule.
$(APP): $(APP_OBJ) $(APP_SHADERS) $(ge/RENDER_SHADERS) $(ge/LIB) $(ge/BGFX_LIBS) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(APP_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/BGFX_LIBS) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# App objects — .cpp files under src/ compile into $(BUILD_DIR)/src/*.o.
$(BUILD_DIR)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

# Convenience: build and run.
run: $(APP)
	./$(APP)

# Engine + render + bridge objects (.cpp)
$(BUILD_DIR)/ge/src/%.o: $(ge)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

# Engine + render + bridge objects (.mm)
$(BUILD_DIR)/ge/src/%.o: $(ge)/src/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

# tools/ objects (currently just player_orientation_stub.cpp — pulled into
# libge.a so DirectRenderHost::send's playerForceOrientation() call resolves
# without consuming desktop apps having to add the stub object themselves).
$(BUILD_DIR)/ge/tools/%.o: $(ge)/tools/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

# Tools ObjC++ objects (.mm) — e.g. player_capture_apple.mm
$(BUILD_DIR)/ge/tools/%.o: $(ge)/tools/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

# Static library
$(ge/LIB): $(ge/OBJ) $(ge/SQLITE_OBJ) $(ge/LZ4_OBJ) $(ge/LITEPARSER_OBJ) $(ge/VENDOR_CPP_OBJ) $(ge/LUNASVG_OBJ) $(ge/PLUTOVG_OBJ)
	@mkdir -p $(dir $@)
	libtool -static -o $@ $^

# Vendor C++ sources
$(BUILD_DIR)/ge/vendor/%.o: $(ge)/vendor/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Box2D library
$(BUILD_DIR)/ge/vendor/box2d/%.o: $(ge/BOX2D_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ge/BOX2D_CFLAGS) -MMD -MP -c $< -o $@

# SQLite3 (C amalgamation)
$(ge/SQLITE_OBJ): $(ge/SQLITE_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O2 -I$(ge)/vendor/include -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_DESERIALIZE -c $< -o $@

# lz4 compression
$(ge/LZ4_OBJ): $(ge/LZ4_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O2 -I$(ge)/vendor/include -c $< -o $@

# liteparser
$(BUILD_DIR)/ge/vendor/liteparser/%.o: $(ge/LITEPARSER_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -w -O2 -I$(ge/LITEPARSER_DIR) -c $< -o $@

# Triangle library
$(ge/TRIANGLE_OBJ): $(ge/TRIANGLE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(ge/TRIANGLE_CFLAGS) -c $< -o $@

# lunasvg (C++17) — SVG rasterizer used by ge::rasterizeSvg.
$(BUILD_DIR)/ge/vendor/lunasvg/%.o: $(ge/LUNASVG_DIR)/source/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ge/LUNASVG_CXXFLAGS) -MMD -MP -c $< -o $@

# plutovg (C11) — 2D vector backend bundled by lunasvg.
$(BUILD_DIR)/ge/vendor/plutovg/%.o: $(ge/PLUTOVG_DIR)/source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ge/PLUTOVG_CFLAGS) -MMD -MP -c $< -o $@

# bx (amalgamated build)
$(ge/BX_OBJ): $(ge/BX_DIR)/src/amalgamated.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ge/BGFX_CXXFLAGS) -I$(ge/BX_DIR)/3rdparty -MMD -MP -c $< -o $@

$(ge/BX_LIB): $(ge/BX_OBJ)
	@mkdir -p $(dir $@)
	libtool -static -o $@ $^

# bimg (amalgamated build — needs bx + bimg internal headers)
$(BUILD_DIR)/ge/vendor/bimg_%.o: $(ge/BIMG_DIR)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ge/BGFX_CXXFLAGS) -DBIMG_CONFIG_DECODE_ENABLE=0 -I$(ge/BIMG_DIR)/3rdparty -I$(ge/BIMG_DIR)/3rdparty/astc-encoder/include -MMD -MP -c $< -o $@

$(ge/BIMG_LIB): $(ge/BIMG_OBJ)
	@mkdir -p $(dir $@)
	libtool -static -o $@ $^

# bgfx (amalgamated build — Metal renderer needs ObjC++ for MTLCopyAllDevicesWithObserver)
$(ge/BGFX_OBJ): $(ge/BGFX_DIR)/src/amalgamated.cpp
	@mkdir -p $(dir $@)
	$(CXX) -ObjC++ $(ge/BGFX_CXXFLAGS) -I$(ge/BGFX_DIR)/3rdparty -I$(ge/BGFX_DIR)/3rdparty/khronos -I$(ge/BGFX_DIR)/src -MMD -MP -c $< -o $@

$(ge/BGFX_LIB): $(ge/BGFX_OBJ)
	@mkdir -p $(dir $@)
	libtool -static -o $@ $^

# bgfx shader compilation. Parent just lists the `.bin` targets (e.g. in
# a SHADERS variable) and makes its app depend on them. These pattern
# rules do the rest.
#
# Convention: files named `*_vs.sc` are vertex shaders, `*_fs.sc` fragment,
# `*_cs.sc` compute. Outputs mirror the source path under $(BUILD_DIR).
#
# Override `ge/SHADER_DIR` (default: `shaders`) or `ge/SHADERC_VARYINGDEF`
# (default: `$(ge/SHADER_DIR)/varying.def.sc`) if your layout differs.
$(BUILD_DIR)/$(ge/SHADER_DIR)/%_vs.bin: $(ge/SHADER_DIR)/%_vs.sc $(ge/SHADERC_VARYINGDEF) $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type vertex \
	    --platform $(ge/SHADERC_PLATFORM) -p $(ge/SHADERC_PROFILE) \
	    --varyingdef $(ge/SHADERC_VARYINGDEF) \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/SHADER_DIR)

$(BUILD_DIR)/$(ge/SHADER_DIR)/%_fs.bin: $(ge/SHADER_DIR)/%_fs.sc $(ge/SHADERC_VARYINGDEF) $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type fragment \
	    --platform $(ge/SHADERC_PLATFORM) -p $(ge/SHADERC_PROFILE) \
	    --varyingdef $(ge/SHADERC_VARYINGDEF) \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/SHADER_DIR)

$(BUILD_DIR)/$(ge/SHADER_DIR)/%_cs.bin: $(ge/SHADER_DIR)/%_cs.sc $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type compute \
	    --platform $(ge/SHADERC_PLATFORM) -p $(ge/SHADERC_PROFILE) \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/SHADER_DIR)

# Engine-internal render shaders (compose pass for viewport tilt).
$(BUILD_DIR)/ge/shaders/%_vs.bin: $(ge/RENDER_SHADER_DIR)/%_vs.sc $(ge/RENDER_SHADER_DIR)/varying.def.sc $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type vertex \
	    --platform $(ge/SHADERC_PLATFORM) -p $(ge/SHADERC_PROFILE) \
	    --varyingdef $(ge/RENDER_SHADER_DIR)/varying.def.sc \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/RENDER_SHADER_DIR)

$(BUILD_DIR)/ge/shaders/%_fs.bin: $(ge/RENDER_SHADER_DIR)/%_fs.sc $(ge/RENDER_SHADER_DIR)/varying.def.sc $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type fragment \
	    --platform $(ge/SHADERC_PLATFORM) -p $(ge/SHADERC_PROFILE) \
	    --varyingdef $(ge/RENDER_SHADER_DIR)/varying.def.sc \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/RENDER_SHADER_DIR)

# SPIR-V variants — for the Android Vulkan backend (Apple-Silicon AVD).
# Output lives in $(BUILD_DIR)/shaders-spirv/ and $(BUILD_DIR)/ge/shaders-spirv/.
# Bypasses the glsl-optimizer (known vec4-constructor bug in 300_es that
# sets the w component to -Infinity).
$(BUILD_DIR)/$(ge/SHADER_DIR)-spirv/%_vs.bin: $(ge/SHADER_DIR)/%_vs.sc $(ge/SHADERC_VARYINGDEF) $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type vertex \
	    --platform android -p spirv \
	    --varyingdef $(ge/SHADERC_VARYINGDEF) \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/SHADER_DIR)

$(BUILD_DIR)/$(ge/SHADER_DIR)-spirv/%_fs.bin: $(ge/SHADER_DIR)/%_fs.sc $(ge/SHADERC_VARYINGDEF) $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type fragment \
	    --platform android -p spirv \
	    --varyingdef $(ge/SHADERC_VARYINGDEF) \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/SHADER_DIR)

$(BUILD_DIR)/ge/shaders-spirv/%_vs.bin: $(ge/RENDER_SHADER_DIR)/%_vs.sc $(ge/RENDER_SHADER_DIR)/varying.def.sc $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type vertex \
	    --platform android -p spirv \
	    --varyingdef $(ge/RENDER_SHADER_DIR)/varying.def.sc \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/RENDER_SHADER_DIR)

$(BUILD_DIR)/ge/shaders-spirv/%_fs.bin: $(ge/RENDER_SHADER_DIR)/%_fs.sc $(ge/RENDER_SHADER_DIR)/varying.def.sc $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type fragment \
	    --platform android -p spirv \
	    --varyingdef $(ge/RENDER_SHADER_DIR)/varying.def.sc \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/RENDER_SHADER_DIR)

# OpenGL ES 3.1 variants — for the Android GLES backend (real Adreno/Mali).
# Output lives in $(BUILD_DIR)/shaders-gles/ and $(BUILD_DIR)/ge/shaders-gles/.
# Profile is 310_es (not 300_es) to dodge the glsl-optimizer vec4 bug.
$(BUILD_DIR)/$(ge/SHADER_DIR)-gles/%_vs.bin: $(ge/SHADER_DIR)/%_vs.sc $(ge/SHADERC_VARYINGDEF) $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type vertex \
	    --platform android -p 310_es \
	    --varyingdef $(ge/SHADERC_VARYINGDEF) \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/SHADER_DIR)

$(BUILD_DIR)/$(ge/SHADER_DIR)-gles/%_fs.bin: $(ge/SHADER_DIR)/%_fs.sc $(ge/SHADERC_VARYINGDEF) $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type fragment \
	    --platform android -p 310_es \
	    --varyingdef $(ge/SHADERC_VARYINGDEF) \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/SHADER_DIR)

$(BUILD_DIR)/ge/shaders-gles/%_vs.bin: $(ge/RENDER_SHADER_DIR)/%_vs.sc $(ge/RENDER_SHADER_DIR)/varying.def.sc $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type vertex \
	    --platform android -p 310_es \
	    --varyingdef $(ge/RENDER_SHADER_DIR)/varying.def.sc \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/RENDER_SHADER_DIR)

$(BUILD_DIR)/ge/shaders-gles/%_fs.bin: $(ge/RENDER_SHADER_DIR)/%_fs.sc $(ge/RENDER_SHADER_DIR)/varying.def.sc $(ge/SHADERC)
	@mkdir -p $(dir $@)
	$(ge/SHADERC) -f $< -o $@ --type fragment \
	    --platform android -p 310_es \
	    --varyingdef $(ge/RENDER_SHADER_DIR)/varying.def.sc \
	    -i $(ge/SHADERC_BGFX_INCLUDE) -i $(ge/RENDER_SHADER_DIR)

# Desktop player binary (symmetry with ge/ios and ge/android).
.PHONY: ge/player
ge/player: $(ge/PLAYER)

$(ge/PLAYER): $(ge/PLAYER_SRC) $(ge/LIB) $(ge/BGFX_LIBS)
	@mkdir -p $(@D)
	$(CXX) -std=c++20 -DGE_DESKTOP $(ge/INCLUDES) $(ge/BGFX_ALL_INCLUDES) $(ge/PLAYER_SRC) $(ge/LIB) $(ge/BGFX_LIBS) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# imgdiff helper — used by matrix-test.sh for reference-image checks.
.PHONY: ge/imgdiff
ge/imgdiff: $(ge/IMGDIFF)

$(ge/IMGDIFF): $(ge)/tools/imgdiff.cpp
	@mkdir -p $(@D)
	$(CXX) -std=c++20 -O2 -I$(ge)/include -I$(ge)/vendor/include $< -o $@

# ────────────────────────────────────────────────
# Unit tests (doctest)
#
# `make unit-test` builds bin/ge-test from $(ge/TEST_SRC) and runs it.
# Tests link against libge.a, so they have access to all engine headers
# and engine-defined classes. The runner reaches into bgfx/SDL only at
# link time (libge.a's references resolve through them); the tests
# themselves don't initialise bgfx, so they're side-effect free.
# ────────────────────────────────────────────────

ge/TEST_BIN = bin/ge-test

.PHONY: unit-test
unit-test: $(ge/TEST_BIN)
	$(ge/TEST_BIN)

$(ge/TEST_BIN): $(ge/TEST_OBJ) $(ge/LIB) $(ge/BGFX_LIBS) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(ge/TEST_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/BGFX_LIBS) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# ────────────────────────────────────────────────
# Mobile targets
#
#   ge/ios, ge/android — build the *consuming app's* mobile distribution
#     project (the `ios/` or `android/` directory produced by ge/ios-init /
#     ge/android-init). These are the usual entry points for app authors.
#
#   ge/player-ios, ge/player-android — build the brokered ge *player* binary
#     for iOS / Android. Used for remote-rendering (ged + server) setups, and
#     by matrix-test.sh's player cells. Independent of the consuming app.
#
#   ge/ios-init, ge/android-init — generate the app-side ios/ or android/
#     scaffolding from ge/tools/{ios,android}-template. Parent passes APP_ID
#     and APP_NAME.
# ────────────────────────────────────────────────

# ── Consuming app's iOS build ──────────────────────────────────────

# Generate the Xcode project (if not already generated) and build the .app
# into ios/build/xcode/Debug-iphonesimulator/ (or the device equivalent).
# Expects ios/CMakeLists.txt to exist — run `make ge/ios-init` first.
#
# We depend on $(APP_SHADERS) and $(ge/RENDER_SHADERS) so they exist on disk
# when CMake's file(GLOB ...) for the bundle Resources runs.
.PHONY: ge/ios
ge/ios: $(APP_SHADERS) $(ge/RENDER_SHADERS)
	@if [ ! -d ios ]; then \
	    echo "ios/ not found — run 'make ge/ios-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	cd ios && cmake -G Xcode -B build/xcode \
	    -DCMAKE_SYSTEM_NAME=iOS \
	    -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DCMAKE_OSX_SYSROOT=iphonesimulator \
	    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
	cd ios && xcodebuild \
	    -project build/xcode/$(APP_DISPLAY_NAME).xcodeproj -scheme $(APP_DISPLAY_NAME) \
	    -configuration Debug -destination "generic/platform=iOS Simulator" \
	    build

# ── Consuming app's Android build ──────────────────────────────────

.PHONY: ge/android
ge/android: $(ge/APP_SHADERS_SPIRV) $(ge/RENDER_SHADERS_SPIRV) $(ge/APP_SHADERS_GLES) $(ge/RENDER_SHADERS_GLES)
	@if [ ! -d android ]; then \
	    echo "android/ not found — run 'make ge/android-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	cd android && ./gradlew assembleDebug
	@echo "APK: android/app/build/outputs/apk/debug/app-debug.apk"

# ── ge player for iOS / Android ────────────────────────────────────

# Generate the Xcode project for the ge player binary (tools/ios/).
.PHONY: ge/player-ios
ge/player-ios:
	cd $(ge)/tools/ios && cmake -G Xcode -B build/xcode \
	    -DCMAKE_SYSTEM_NAME=iOS \
	    -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DCMAKE_OSX_SYSROOT=iphoneos \
	    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
	@echo "Open $(ge)/tools/ios/build/xcode/Player.xcodeproj in Xcode"

# ge player iOS archive (generate project + xcodebuild archive)
.PHONY: ge/player-ios-archive
ge/player-ios-archive: ge/player-ios
	cd $(ge)/tools/ios && xcodebuild \
	    -project build/xcode/Player.xcodeproj \
	    -scheme Player \
	    -destination "generic/platform=iOS" \
	    -archivePath build/Player.xcarchive \
	    -allowProvisioningUpdates \
	    archive

# ge player TestFlight upload (archive + export/upload to App Store Connect)
.PHONY: ge/player-ios-testflight
ge/player-ios-testflight: ge/player-ios-archive
	cd $(ge)/tools/ios && xcodebuild -exportArchive \
	    -archivePath build/Player.xcarchive \
	    -exportOptionsPlist ExportOptions.plist \
	    -exportPath build/export \
	    -allowProvisioningUpdates
	@echo "Uploaded to App Store Connect — check TestFlight in https://appstoreconnect.apple.com"

# ge player Android debug APK
.PHONY: ge/player-android
ge/player-android:
	cd $(ge)/tools/android && ./gradlew assembleDebug
	@echo "APK: $(ge)/tools/android/app/build/outputs/apk/debug/app-debug.apk"

# ge player Android release AAB for Play Store upload
.PHONY: ge/player-android-release
ge/player-android-release:
	cd $(ge)/tools/android && ./gradlew bundleRelease
	@echo "AAB: $(ge)/tools/android/app/build/outputs/bundle/release/app-release.aab"

# ── Mobile project scaffolding (consuming app) ─────────────────────

# Parent Makefile sets APP_ID (bundle id / package) and APP_NAME before
# calling. APP_DISPLAY_NAME defaults to APP_NAME; override for a prettier
# on-device name while keeping APP_NAME as the lowercase binary name.
.PHONY: ge/android-init
ge/android-init:
	@if [ -z "$(APP_ID)" ] || [ -z "$(APP_NAME)" ]; then \
		echo "Error: set APP_ID and APP_NAME"; exit 1; fi
	$(ge)/tools/init-android.sh "$(APP_ID)" "$(APP_DISPLAY_NAME)"

.PHONY: ge/ios-init
ge/ios-init:
	@if [ -z "$(APP_ID)" ] || [ -z "$(APP_NAME)" ]; then \
		echo "Error: set APP_ID and APP_NAME"; exit 1; fi
	$(ge)/tools/init-ios.sh "$(APP_ID)" "$(APP_DISPLAY_NAME)" "$(IOS_DEVELOPMENT_TEAM)"

# ────────────────────────────────────────────────
# Generic targets (use CLEAN, COMPILE_DB_DEPS)
# ────────────────────────────────────────────────

.PHONY: clean
clean:
	rm -rf $(CLEAN)

# ────────────────────────────────────────────────
# Dependency graph (parent can extend ge/DEPGRAPH_DEPS)
# ────────────────────────────────────────────────

.PHONY: depgraph clean-depgraph
depgraph: deps.svg

deps.svg: $(ge/DEPGRAPH_DEPS)
	python3 $(ge)/tools/depgraph.py --format svg --output deps

deps.dot: $(ge/DEPGRAPH_DEPS)
	python3 $(ge)/tools/depgraph.py --format dot --output deps

clean-depgraph:
	rm -f deps.dot deps.svg deps.png

# Generate compile_commands.json for IDE support (clangd, VS Code).
# compiledb captures all sub-make commands,
# so we filter to only project entries afterward.
compile_commands.json: $(COMPILE_DB_DEPS)
	@compiledb -n make
	@python3 -c "import json,pathlib; p=pathlib.Path('compile_commands.json'); d=json.loads(p.read_text()); p.write_text(json.dumps([e for e in d if e['directory']=='$(CURDIR)'],indent=1)+'\n')"

# ────────────────────────────────────────────────
# Developer setup (common engine prerequisites)
# ────────────────────────────────────────────────

.PHONY: ge/init
ge/init:
	@echo "── ge engine setup ──"
	@command -v brew >/dev/null 2>&1 || { echo "ERROR: Homebrew not found. Install from https://brew.sh"; exit 1; }
	@echo "  Homebrew installed"
	@command -v xcode-select >/dev/null 2>&1 && xcode-select -p >/dev/null 2>&1 || { echo "ERROR: Xcode Command Line Tools not found. Run: xcode-select --install"; exit 1; }
	@echo "  Xcode Command Line Tools installed"
	@brew install git-lfs
	@git lfs install
	@git lfs pull
	@echo "  Dependencies installed"
	@brew install compiledb
	@$(MAKE) compile_commands.json
	@echo "  compile_commands.json generated"

# Dashboard web app (Vite + React)
.PHONY: web
web:
	cd $(ge)/web && npm install && npm run build

# Game Engine Daemon (Go binary with embedded web UI)
.PHONY: ged
ged: web
	@if [ ! -d $(ge)/web/dist ]; then \
		echo "ERROR: $(ge)/web/dist not found. Run 'make web' first."; exit 1; \
	fi
	@rm -rf $(ge)/ged/web/dist
	@mkdir -p $(ge)/ged/web
	@cp -R $(ge)/web/dist $(ge)/ged/web/dist
	cd $(ge)/ged && go build -o $(CURDIR)/bin/ged .

.PHONY: ged-test
ged-test:
	@if [ ! -d $(ge)/ged/web/dist ]; then \
		mkdir -p $(ge)/ged/web/dist && touch $(ge)/ged/web/dist/index.html; \
	fi
	cd $(ge)/ged && go test ./...

# ────────────────────────────────────────────────
# Debug build
#
# `make ge/debug` builds bin/$(APP_NAME)-debug with assertions enabled,
# debug symbols, and no optimisation.  The debug matrix cells exercise
# this binary rather than the default release binary.
# ────────────────────────────────────────────────

# Override points: apps can extend APP_DEBUG_OBJ or set APP_DEBUG separately.
APP_DEBUG     ?= bin/$(APP_NAME)-debug
APP_DEBUG_OBJ ?= $(patsubst %.cpp,$(BUILD_DIR)/debug/%.o,$(APP_SRC))

ge/CXXFLAGS_DEBUG = -std=c++20 -O0 -g -DDEBUG $(ge/INCLUDES) $(ge/BGFX_ALL_INCLUDES) -DBX_CONFIG_DEBUG=1

.PHONY: ge/debug
ge/debug: $(APP_DEBUG)

$(APP_DEBUG): $(APP_DEBUG_OBJ) $(APP_SHADERS) $(ge/RENDER_SHADERS) $(ge/LIB) $(ge/BGFX_LIBS) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(APP_DEBUG_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/BGFX_LIBS) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

$(BUILD_DIR)/debug/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ge/CXXFLAGS_DEBUG) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

# ── iOS release build ──────────────────────────────────────────────────
# `make ge/ios-release` builds the consuming app's iOS .app with
# -configuration Release.  The regular `make ge/ios` builds Debug.
# Release cells use ge/ios-release; debug cells use ge/ios.

.PHONY: ge/ios-release
ge/ios-release: $(APP_SHADERS) $(ge/RENDER_SHADERS)
	@if [ ! -d ios ]; then \
	    echo "ios/ not found — run 'make ge/ios-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	cd ios && cmake -G Xcode -B build/xcode \
	    -DCMAKE_SYSTEM_NAME=iOS \
	    -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DCMAKE_OSX_SYSROOT=iphonesimulator \
	    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
	cd ios && xcodebuild \
	    -project build/xcode/$(APP_DISPLAY_NAME).xcodeproj -scheme $(APP_DISPLAY_NAME) \
	    -configuration Release -destination "generic/platform=iOS Simulator" \
	    build

# ── iOS physical-device release build ─────────────────────────────────
# `make ge/ios-device-release` builds the consuming app's iOS .app targeting
# iphoneos (physical device) with Release configuration.
# Requires ios/CMakeLists.txt to set DEVELOPMENT_TEAM (see ge/ios-init).

.PHONY: ge/ios-device-release
ge/ios-device-release: $(APP_SHADERS) $(ge/RENDER_SHADERS)
	@if [ ! -d ios ]; then \
	    echo "ios/ not found — run 'make ge/ios-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	cd ios && cmake -G Xcode -B build/xcode-device \
	    -DCMAKE_SYSTEM_NAME=iOS \
	    -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DCMAKE_OSX_SYSROOT=iphoneos \
	    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
	cd ios && xcodebuild \
	    -project build/xcode-device/$(APP_DISPLAY_NAME).xcodeproj -scheme $(APP_DISPLAY_NAME) \
	    -configuration Release -destination "generic/platform=iOS" \
	    -allowProvisioningUpdates \
	    build

# ge player iOS physical-device build.
# ge/player-ios generates the Xcode project with iphoneos sysroot.
# This target builds a Debug .app from that project targeting a physical device.
.PHONY: ge/player-ios-device
ge/player-ios-device: ge/player-ios
	cd $(ge)/tools/ios && xcodebuild \
	    -project build/xcode/Player.xcodeproj -scheme Player \
	    -configuration Debug -destination "generic/platform=iOS" \
	    -allowProvisioningUpdates \
	    build

# ── Android release build ──────────────────────────────────────────────
# `make ge/android-release` builds the consuming app's Android APK with
# assembleRelease.  The regular `make ge/android` builds assembleDebug.
# Release cells use ge/android-release; debug cells use ge/android.

.PHONY: ge/android-release
ge/android-release: $(ge/APP_SHADERS_SPIRV) $(ge/RENDER_SHADERS_SPIRV) $(ge/APP_SHADERS_GLES) $(ge/RENDER_SHADERS_GLES)
	@if [ ! -d android ]; then \
	    echo "android/ not found — run 'make ge/android-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	cd android && ./gradlew assembleRelease
	@echo "APK: android/app/build/outputs/apk/release/app-release-unsigned.apk"

# ────────────────────────────────────────────────
# End-to-end test matrix
# ────────────────────────────────────────────────
#
# `make check` runs the full mobile/desktop test matrix — each cell is its
# own make rule (cell.<name>) that shells out to ge/tools/matrix-cell.sh.
# Cells fail loudly if they can't run (missing device, missing pipeline,
# etc.). Consumers silence known-impossible cells with CHECK_EXCLUDE:
#
#     make check CHECK_EXCLUDE='android-device-tablet-*'
#
# Glob syntax: `*` matches any run of characters within a cell name.
# Multiple patterns: space-separated.
#
# Soak timing:
#   Standard cells run a 10s soak (SOAK_TIMEOUT=10). This keeps `make -j check`
#   well under 8 minutes on M4 Max with primed caches. To run with the full 60s
#   soak across all cells: `make check SOAK_TIMEOUT=60`.
#
#   For the dedicated long-soak reliability sweep (always 60s, no override
#   needed): `make cell.long-soak`.
#
# Parallelism:
#   `make -j check` runs all cells concurrently. Cells targeting different
#   devices run in parallel. Cells racing for the same device serialise via
#   spyder's reservation queue. Desktop cells are always concurrent.
#   Same-platform cells (e.g. ios-sim-tablet-dist + ios-sim-tablet-player)
#   resolve to distinct sims when two are available in the spyder pool.

# Device pin variables — passed to matrix-cell.sh to target a specific inventory
# alias or UDID for each device class.  When set, the cell skips spyder reserve
# --on selector dispatch and pins the given alias directly.  Leave empty to let
# spyder pick from the available pool via selector predicates.
#
# Simulator / emulator cells (selector-based by default):
GE_IOS_SIM_PHONE_DEVICE   ?=
GE_IOS_SIM_TABLET_DEVICE  ?=
GE_ANDROID_EMU_PHONE_DEVICE  ?=
GE_ANDROID_EMU_TABLET_DEVICE ?=
#
# Physical device cells (common to pin for developer setups):
GE_IOS_PHONE_DEVICE   ?=
GE_IOS_TABLET_DEVICE  ?= Pippa
GE_ANDROID_PHONE_DEVICE  ?=
GE_ANDROID_TABLET_DEVICE ?=

# Soak timeout for standard cells (seconds). Default 10s — fast enough for
# `make -j check` to finish well under 8 min. Override on the command line:
#   make check SOAK_TIMEOUT=60       # run all cells with the full 60s soak
# For a single long-soak sweep at 60s against the representative desktop cell,
# use the dedicated `make cell.long-soak` target.
SOAK_TIMEOUT ?= 10

# Canonical 24-cell list. Cells are grouped for readability.
ge/CELLS := \
    desktop-dist desktop-player \
    ios-sim-phone-dist ios-sim-phone-player \
    ios-sim-tablet-dist ios-sim-tablet-player \
    ios-device-phone-dist ios-device-phone-player \
    ios-device-tablet-dist ios-device-tablet-player \
    android-emu-phone-dist android-emu-phone-player \
    android-emu-tablet-dist android-emu-tablet-player \
    android-device-phone-dist android-device-phone-player \
    android-device-tablet-dist android-device-tablet-player \
    desktop-debug-dist desktop-debug-player \
    ios-debug-dist ios-debug-player \
    android-debug-dist android-debug-player

# Translate shell-style globs in CHECK_EXCLUDE to make's `%` syntax and
# filter them out of the cell list. Accepts space-separated patterns.
ge/CHECK_EXCLUDE_PATTERNS := $(subst *,%,$(CHECK_EXCLUDE))
ge/CHECK_CELLS := $(filter-out $(ge/CHECK_EXCLUDE_PATTERNS),$(ge/CELLS))

# Per-cell rule — every cell name is its own make target prefixed `cell.`.
# Boilerplate enumeration is deliberate: the spec calls for explicit
# enumeration so `make cell.ios-sim-tablet-dist` works and `make -n check`
# prints a readable dep list.
# Cell rules — each cell resolves its own device reservation via spyder.
# Pin variables (GE_IOS_SIM_PHONE_DEVICE, etc.) are exported so matrix-cell.sh
# can pick them up when a specific alias is configured; leave empty to let
# spyder resolve via selector predicates (--on platform=ios-sim,...).
#
# Cells targeting different devices run concurrently under parallel make.
# Cells racing for the same device serialise via spyder's reservation queue.
.PHONY: $(addprefix cell.,$(ge/CELLS))
cell.desktop-dist:               ; $(ge)/tools/matrix-cell.sh desktop-dist --soak-timeout $(SOAK_TIMEOUT)
cell.desktop-player:             ; $(ge)/tools/matrix-cell.sh desktop-player --soak-timeout $(SOAK_TIMEOUT)
cell.ios-sim-phone-dist:         ; GE_IOS_SIM_PHONE_DEVICE=$(GE_IOS_SIM_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh ios-sim-phone-dist --soak-timeout $(SOAK_TIMEOUT)
cell.ios-sim-phone-player:       ; GE_IOS_SIM_PHONE_DEVICE=$(GE_IOS_SIM_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh ios-sim-phone-player --soak-timeout $(SOAK_TIMEOUT)
cell.ios-sim-tablet-dist:        ; GE_IOS_SIM_TABLET_DEVICE=$(GE_IOS_SIM_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh ios-sim-tablet-dist --soak-timeout $(SOAK_TIMEOUT)
cell.ios-sim-tablet-player:      ; GE_IOS_SIM_TABLET_DEVICE=$(GE_IOS_SIM_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh ios-sim-tablet-player --soak-timeout $(SOAK_TIMEOUT)
cell.ios-device-phone-dist:      ; GE_IOS_PHONE_DEVICE=$(GE_IOS_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh ios-device-phone-dist --soak-timeout $(SOAK_TIMEOUT)
cell.ios-device-phone-player:    ; GE_IOS_PHONE_DEVICE=$(GE_IOS_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh ios-device-phone-player --soak-timeout $(SOAK_TIMEOUT)
cell.ios-device-tablet-dist:     ; GE_IOS_TABLET_DEVICE=$(GE_IOS_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh ios-device-tablet-dist --soak-timeout $(SOAK_TIMEOUT)
cell.ios-device-tablet-player:   ; GE_IOS_TABLET_DEVICE=$(GE_IOS_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh ios-device-tablet-player --soak-timeout $(SOAK_TIMEOUT)
cell.android-emu-phone-dist:     ; GE_ANDROID_EMU_PHONE_DEVICE=$(GE_ANDROID_EMU_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh android-emu-phone-dist --soak-timeout $(SOAK_TIMEOUT)
cell.android-emu-phone-player:   ; GE_ANDROID_EMU_PHONE_DEVICE=$(GE_ANDROID_EMU_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh android-emu-phone-player --soak-timeout $(SOAK_TIMEOUT)
cell.android-emu-tablet-dist:    ; GE_ANDROID_EMU_TABLET_DEVICE=$(GE_ANDROID_EMU_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh android-emu-tablet-dist --soak-timeout $(SOAK_TIMEOUT)
cell.android-emu-tablet-player:  ; GE_ANDROID_EMU_TABLET_DEVICE=$(GE_ANDROID_EMU_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh android-emu-tablet-player --soak-timeout $(SOAK_TIMEOUT)
cell.android-device-phone-dist:  ; GE_ANDROID_PHONE_DEVICE=$(GE_ANDROID_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh android-device-phone-dist --soak-timeout $(SOAK_TIMEOUT)
cell.android-device-phone-player: ; GE_ANDROID_PHONE_DEVICE=$(GE_ANDROID_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh android-device-phone-player --soak-timeout $(SOAK_TIMEOUT)
cell.android-device-tablet-dist: ; GE_ANDROID_TABLET_DEVICE=$(GE_ANDROID_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh android-device-tablet-dist --soak-timeout $(SOAK_TIMEOUT)
cell.android-device-tablet-player: ; GE_ANDROID_TABLET_DEVICE=$(GE_ANDROID_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh android-device-tablet-player --soak-timeout $(SOAK_TIMEOUT)
cell.desktop-debug-dist:         ; $(ge)/tools/matrix-cell.sh desktop-debug-dist --soak-timeout $(SOAK_TIMEOUT)
cell.desktop-debug-player:       ; $(ge)/tools/matrix-cell.sh desktop-debug-player --soak-timeout $(SOAK_TIMEOUT)
cell.ios-debug-dist:             ; GE_IOS_SIM_PHONE_DEVICE=$(GE_IOS_SIM_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh ios-debug-dist --soak-timeout $(SOAK_TIMEOUT)
cell.ios-debug-player:           ; GE_IOS_SIM_PHONE_DEVICE=$(GE_IOS_SIM_PHONE_DEVICE) $(ge)/tools/matrix-cell.sh ios-debug-player --soak-timeout $(SOAK_TIMEOUT)
cell.android-debug-dist:         ; GE_ANDROID_EMU_TABLET_DEVICE=$(GE_ANDROID_EMU_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh android-debug-dist --soak-timeout $(SOAK_TIMEOUT)
cell.android-debug-player:       ; GE_ANDROID_EMU_TABLET_DEVICE=$(GE_ANDROID_EMU_TABLET_DEVICE) $(ge)/tools/matrix-cell.sh android-debug-player --soak-timeout $(SOAK_TIMEOUT)

# Long-soak cell: runs the desktop-dist cell with a 60s soak.
# This is the dedicated reliability sweep preserved so the standard 10s cells
# don't regress into missing crash-over-time failures. Run independently:
#   make cell.long-soak
.PHONY: cell.long-soak
cell.long-soak: ; $(ge)/tools/matrix-cell.sh desktop-dist --soak-timeout 60

.PHONY: check matrix-test
check matrix-test: $(addprefix cell.,$(ge/CHECK_CELLS))
	@echo "── Matrix summary ──"
	@printf '  %-40s %s\n' "Cells run:" "$(words $(ge/CHECK_CELLS))"
	@printf '  %-40s %s\n' "Cells excluded via CHECK_EXCLUDE:" "$(words $(filter-out $(ge/CHECK_CELLS),$(ge/CELLS)))"
	@[ -z "$(filter-out $(ge/CHECK_CELLS),$(ge/CELLS))" ] || \
	    printf '  %-40s %s\n' "  Excluded:" "$(filter-out $(ge/CHECK_CELLS),$(ge/CELLS))"

# Report the resolved cell list without running anything.
.PHONY: check-list
check-list:
	@echo "Cells that would run:"
	@printf '  %s\n' $(ge/CHECK_CELLS)
	@echo "Cells excluded:"
	@printf '  %s\n' $(filter-out $(ge/CHECK_CELLS),$(ge/CELLS))
	@echo "Soak timeout: $(SOAK_TIMEOUT)s  (override with SOAK_TIMEOUT=N)"
	@echo "Long-soak cell: 'make cell.long-soak'  (always 60s, desktop-dist)"

# ────────────────────────────────────────────────
# Spyder pool management
#
# `make pool-init` symlinks tools/spyder-pool.yaml to ~/.spyder/pool.yaml so
# spyder's daemon picks up pre-warm templates for the matrix's sim/emu cells.
# This is a one-time setup step; re-running is safe (idempotent).
#
# `make pool-drain` shuts down all pool instances and removes the symlink.
# Run before the machine goes idle for an extended period to avoid leaving
# background emulators running.
#
# Pool template names mirror the three matrix platforms that benefit from
# pre-warming:
#   ios-phone   — iPhone 16 Pro sim, iOS 18.4
#   ios-tablet  — iPad Air 11-inch (M4) sim, iOS 18.4
#   android-phone — Pixel 9 Pro XL AVD (android-36, google_apis_playstore)
# ────────────────────────────────────────────────

ge/POOL_YAML    := $(ge)/tools/spyder-pool.yaml
ge/POOL_DEST    := $(HOME)/.spyder/pool.yaml
ge/POOL_TMPLS   := ios-phone ios-tablet android-phone

.PHONY: pool-init
pool-init:
	@echo "── Spyder pool setup ──"
	@command -v spyder >/dev/null 2>&1 || { echo "WARN: spyder not found; skipping pool setup. Install spyder v0.17.0+ to enable pre-warming."; exit 0; }
	@mkdir -p "$(dir $(ge/POOL_DEST))"
	@if [ -e "$(ge/POOL_DEST)" ] && [ ! -L "$(ge/POOL_DEST)" ]; then \
	    echo "WARN: $(ge/POOL_DEST) exists and is not a symlink; leaving it alone."; \
	    echo "  To adopt ge's pool config, remove the existing file and re-run."; \
	else \
	    ln -sf "$(abspath $(ge/POOL_YAML))" "$(ge/POOL_DEST)"; \
	    echo "  Symlinked: $(ge/POOL_DEST) -> $(abspath $(ge/POOL_YAML))"; \
	fi
	@echo "  Restarting spyder daemon to pick up new pool config..."
	@spyder daemon restart 2>/dev/null || spyder daemon start 2>/dev/null || \
	    echo "  WARN: could not restart spyder daemon; restart it manually."
	@echo "  Warming pool instances (this may take 30-60 s)..."
	@for tmpl in $(ge/POOL_TMPLS); do \
	    echo "    spyder pool_warm $$tmpl --count 1"; \
	    spyder pool_warm "$$tmpl" --count 1 2>&1 || \
	        echo "    WARN: pool_warm $$tmpl failed (daemon may still be starting; retry with 'make pool-init')"; \
	done
	@echo "  Pool ready. Run 'make pool-drain' to shut down instances."

.PHONY: pool-drain
pool-drain:
	@echo "── Spyder pool drain ──"
	@command -v spyder >/dev/null 2>&1 || { echo "WARN: spyder not found; nothing to drain."; exit 0; }
	@for tmpl in $(ge/POOL_TMPLS); do \
	    echo "  spyder pool_drain $$tmpl"; \
	    spyder pool_drain "$$tmpl" 2>&1 || \
	        echo "  WARN: pool_drain $$tmpl failed (may already be empty)"; \
	done
	@if [ -L "$(ge/POOL_DEST)" ]; then \
	    rm "$(ge/POOL_DEST)"; \
	    echo "  Removed symlink: $(ge/POOL_DEST)"; \
	fi
	@echo "  Pool drained."

# Canned recipe for the parent to expand at the end of its init target.
define ge/INIT_DONE
	@echo ""
	@echo "Setup complete. Next steps:"
	@echo "  make              # Build the application"
	@echo "  make run          # Build and run"
	@echo "  make test         # Run all tests"
endef

# Dep-file include for the app's own objects. Engine object .d files are
# already picked up by their own implicit pattern-rule dep tracking.
-include $(APP_OBJ:.o=.d)
