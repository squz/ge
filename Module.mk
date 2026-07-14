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

# 🎯T92.2.2 Server build variant: `make GE_SERVER=1` compiles ge::run to the
# hidden-window streaming path (-DGE_SERVER_BUILD, below) and emits a distinct
# binary (bin/<app>-server) into a distinct object dir (so its SessionHost.o,
# built with the flag, never clobbers the desktop build's). Plain `make` is the
# desktop/windowed build. The app source is identical in both.
BUILD_DIR ?= build$(if $(GE_SERVER),-server)
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
	-I/opt/homebrew/opt/freetype/include/freetype2 \
	-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_DESERIALIZE \
	-DLUNASVG_BUILD_STATIC \
	-I$(ge)/vendor/github.com/floooh/sokol

# sokol_gfx (vendored single-header — no separate compile step;
# SokolContext.mm / SokolContext_android.cpp do the SOKOL_IMPL include).
# T38: bgfx/bx/bimg vendor compile retired; the empty ge/BGFX_* shim
# variables were removed in the 🎯T98 cleanup (an undefined make variable
# expands to nothing, so any external Makefile that still referenced them
# is unaffected).

# SDL3 libraries (static, vendored)
ge/SDL3_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libSDL3.a
ge/SDL3_IMAGE_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libSDL3_image.a
ge/SDL3_TTF_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libSDL3_ttf.a
ge/FREETYPE_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libfreetype.a
ge/HARFBUZZ_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libharfbuzz.a
ge/PLUTOSVG_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libplutosvg.a
ge/PLUTOVG_LIB = $(ge)/vendor/sdl3/lib/macos-arm64/libplutovg.a
ge/SDL_LIBS = $(ge/SDL3_LIB) $(ge/SDL3_IMAGE_LIB) $(ge/SDL3_TTF_LIB) $(ge/FREETYPE_LIB) $(ge/HARFBUZZ_LIB) $(ge/PLUTOSVG_LIB) $(ge/PLUTOVG_LIB)

# macOS frameworks needed by any ge desktop app (SDL3 + sokol/Metal + VideoToolbox +
# CoreMotion). Apps can extend via FRAMEWORKS += ... after the include.
ge/FRAMEWORKS = \
    -framework Metal -framework MetalKit -framework QuartzCore \
    -framework Cocoa -framework IOKit -framework CoreFoundation \
    -framework Carbon -framework CoreAudio -framework AudioToolbox \
    -framework CoreHaptics -framework GameController -framework CoreVideo \
    -framework ForceFeedback -framework AVFoundation -framework CoreMedia \
    -framework UniformTypeIdentifiers -framework CoreGraphics \
    -framework VideoToolbox -framework CoreMotion -framework StoreKit

# Core engine sources — pulled from the canonical manifest at
# tools/ge-sources.mk so Module.mk and tools/prebuild.sh stay in sync.
# Module.mk targets a macOS desktop / Apple consumer, hence the
# DIRECT_APPLE_DESKTOP rollup; the BROKERED suffix layers on the
# wire/networking sources when a consumer builds in non-direct mode.
include $(ge)/tools/ge-sources.mk

# Prefix every relative path with $(ge)/ so existing $(ge/SRC_*) consumers
# keep working without each having to learn the new file's path style.
ge/SRC_DIRECT   := $(addprefix $(ge)/,$(GE_SRC_DIRECT_APPLE_DESKTOP))
ge/SRC_BROKERED := $(addprefix $(ge)/,$(GE_SRC_BROKERED))

# T38: brokered streaming sources still reference bgfx and are not being
# ported in this phase. Direct-only build for libge.a until the bridge
# subsystem is ported.
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

# sokol-shdc shader cross-compiler (vendored binary; macOS-arm64 host only
# for now — Linux/Windows hosts would point at a different bin/<host>/ dir).
# Parent lists desired `.h` outputs (e.g. `$(BUILD_DIR)/shaders/simple.h`);
# Module.mk's pattern rules generate them from matching `.glsl` sources.
ge/SOKOL_SHDC = $(ge)/vendor/github.com/floooh/sokol-tools-bin/bin/osx_arm64/sokol-shdc

# Target shader languages emitted into each generated header. sokol-shdc
# bakes all listed backends into one sokol_shader_desc — at runtime
# sokol_gfx picks the right one based on sg_query_backend(). The set is
# the union of every backend SokolContext.mm / SokolContext_android.cpp
# can pick: Metal on macOS + iOS device + iOS simulator, GLES3 (GLSL
# 300 es) on Android GLES.
#
# metal_sim (🎯T84) is mandatory: the Apple-Silicon iOS Simulator reports
# SG_BACKEND_METAL_SIMULATOR from sg_query_backend(), a distinct enum from
# SG_BACKEND_METAL_IOS. Without a metal_sim slot the generated
# <name>_shader_desc() returns NULL for that backend and sg_make_shader
# aborts ("Assertion failed: (desc)") on the first SpriteBatch::submit.
ge/SOKOL_SHDC_LANGS ?= metal_macos:metal_ios:metal_sim:glsl300es:spirv_vk

# Parent defines its shader source directory (default `shaders`).
ge/SHADER_DIR ?= shaders

# ge's own internal render shaders. Consumers depend on $(ge/RENDER_SHADERS)
# so the headers exist on disk at compile time — the sprite/render code
# `#include`s them out of build/ge/shaders/.
# T38: ge_compose.* dropped (compose pass stripped from DirectRenderHost).
# ge_sprite.h (ge::Sprite/SpriteBatch) + ge_debug.h (🎯T97 ge::debug overlay) +
# ge_solid.h (ge::drawSolid unlit mesh fill), each generated from the matching
# .glsl in $(ge/RENDER_SHADER_DIR).
ge/RENDER_SHADER_DIR = $(ge)/src/render/shaders
ge/RENDER_SHADERS = \
	$(BUILD_DIR)/ge/shaders/ge_sprite.h \
	$(BUILD_DIR)/ge/shaders/ge_debug.h \
	$(BUILD_DIR)/ge/shaders/ge_solid.h

# Android shader variants — no longer separated (sokol-shdc bakes all
# backends into one header per shader). Kept as aliases of the base set
# so existing references in this Makefile and consumer Makefiles continue
# to expand to the same target list.
ge/APP_SHADERS_SPIRV    = $(APP_SHADERS)
ge/RENDER_SHADERS_SPIRV = $(ge/RENDER_SHADERS)
ge/APP_SHADERS_GLES     = $(APP_SHADERS)
ge/RENDER_SHADERS_GLES  = $(ge/RENDER_SHADERS)

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
	$(ge)/src/manifest_test.cpp \
	$(ge)/src/DampedRotation_test.cpp \
	$(ge)/src/GlobeController_test.cpp \
	$(ge)/src/Rect_test.cpp \
	$(ge)/src/Rect_constexpr_test.cpp \
	$(ge)/src/svg_test.cpp \
	$(ge)/src/png_test.cpp \
	$(ge)/src/text_test.cpp \
	$(ge)/src/sprite_test.cpp \
	$(ge)/src/appchannel_test.cpp \
	$(ge)/src/box2d_slice_test.cpp \
	$(ge)/src/transform_test.cpp \
	$(ge)/src/Ortho_test.cpp \
	$(ge)/src/gesture_test.cpp \
	$(ge)/src/layout_test.cpp \
	$(ge)/src/button_test.cpp \
	$(ge)/src/long_press_test.cpp \
	$(ge)/src/sdl_input_test.cpp \
	$(ge)/src/iap_test.cpp \
	$(ge)/src/iap_entitlement_cache_test.cpp \
	$(ge)/src/iap_local_test.cpp \
	$(ge)/src/audio_test.cpp \
	$(ge)/src/debug_test.cpp \
	$(ge)/src/Context_test.cpp \
	$(ge)/src/render_on_demand_test.cpp \
	$(ge)/src/Signal_test.cpp \
	$(ge)/src/render/RefreshRateBoost_test.cpp \
	$(ge)/src/wire_input_test.cpp \
	$(ge)/src/VideoRoundtrip_test.cpp \
	$(ge)/src/CmdStream_test.cpp
ge/TEST_OBJ = $(patsubst $(ge)/src/%.cpp,$(BUILD_DIR)/ge/src/%.o,$(ge/TEST_SRC))

# Shared variables (parent can += to extend)
CLEAN = bin build
COMPILE_DB_DEPS = $(ge/SRC) $(ge/TEST_SRC) $(ge)/Module.mk $(APP_SRC) Makefile

# ────────────────────────────────────────────────
# Default compile/link flags (app-overridable)
# ────────────────────────────────────────────────

# Engine-managed base flags. Apps that want to extend CXXFLAGS keep these by
# default (via `CXXFLAGS ?=` below) or reference $(ge/CXXFLAGS_BASE) explicitly
# when constructing their own.
ge/CXXFLAGS_BASE = -std=c++20 -O2 -g $(ge/INCLUDES) -I$(BUILD_DIR)/ge/shaders -I$(BUILD_DIR)/$(ge/SHADER_DIR) $(if $(GE_SERVER),-DGE_SERVER_BUILD)

CXXFLAGS   ?= $(ge/CXXFLAGS_BASE) -Isrc
SDL_CFLAGS ?= -I$(ge)/vendor/sdl3/include
FRAMEWORKS ?= $(ge/FRAMEWORKS)

# ────────────────────────────────────────────────
# App convention — parent declares APP_NAME / APP_SRC / APP_SHADERS
# ────────────────────────────────────────────────

# Derived from the parent's APP_NAME and APP_SRC. The parent can override
# $(APP) (e.g. to change the binary location) or $(APP_OBJ) (unusual) before
# the include.
APP         ?= bin/$(APP_NAME)$(if $(GE_SERVER),-server)
APP_OBJ     ?= $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(APP_SRC))

# Display name used for iOS / Android bundles and Xcode targets/schemes.
# Defaults to APP_NAME; set to a Pascal-cased variant if you want a pretty
# string on the home screen while keeping a lowercase binary name.
APP_DISPLAY_NAME ?= $(APP_NAME)

# Stripped form of APP_DISPLAY_NAME (no spaces). Used as the Xcode
# target / scheme / xcodeproj file basename — mirrors init-ios.sh's
# APP_BIN_NAME computation so the Module.mk iOS rules can address the
# right .xcodeproj without re-running the init script.
NULL  :=
SPACE := $(NULL) $(NULL)
APP_BIN_NAME = $(subst $(SPACE),,$(APP_DISPLAY_NAME))

# Extra static libs/objects the app needs beyond the ge engine (e.g. ship a
# specialized third-party library). Defaults to Box2D since many ge apps use
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
$(APP): $(APP_OBJ) $(APP_SHADERS) $(ge/RENDER_SHADERS) $(ge/LIB) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(APP_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

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

# sprite.cpp #includes the sokol-shdc-generated ge_sprite.h, so the render
# shader headers must exist before any engine object compiles. The $(APP)
# link rule lists $(ge/RENDER_SHADERS), but lib-only consumers (e.g. the
# unit-test target, which builds $(ge/LIB) without $(APP)) never trigger
# that rule and would fail a clean build with "ge_sprite.h not found".
# Order-only so regenerating a shader doesn't force a full engine rebuild —
# the -MMD .d files capture the real header dependency for incremental builds.
$(ge/OBJ): | $(ge/RENDER_SHADERS)

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

# T38: bgfx / bx / bimg vendor build rules removed. The sokol_gfx single-header
# is dropped in at #include time by SokolContext.mm (Apple) and
# SokolContext_android.cpp (Android) via SOKOL_IMPL — no separate static
# library to build. Include path lives in ge/INCLUDES; vendored at
# vendor/github.com/floooh/sokol/{sokol_gfx,sokol_log}.h.

# sokol-shdc shader compilation. Parent lists desired `.h` outputs (e.g.
# in APP_SHADERS) and makes its app depend on them. Pattern rules below
# emit a single header per .glsl with embedded bytecode for every backend
# in $(ge/SOKOL_SHDC_LANGS).
$(BUILD_DIR)/$(ge/SHADER_DIR)/%.h: $(ge/SHADER_DIR)/%.glsl $(ge/SOKOL_SHDC)
	@mkdir -p $(dir $@)
	$(ge/SOKOL_SHDC) -i $< -o $@ -l $(ge/SOKOL_SHDC_LANGS) -f sokol

$(BUILD_DIR)/ge/shaders/%.h: $(ge/RENDER_SHADER_DIR)/%.glsl $(ge/SOKOL_SHDC)
	@mkdir -p $(dir $@)
	$(ge/SOKOL_SHDC) -i $< -o $@ -l $(ge/SOKOL_SHDC_LANGS) -f sokol

# Desktop player binary (symmetry with ge/ios and ge/android).
.PHONY: ge/player
ge/player: $(ge/PLAYER)

$(ge/PLAYER): $(ge/PLAYER_SRC) $(ge/LIB)
	@mkdir -p $(@D)
	$(CXX) -std=c++20 -DGE_DESKTOP $(ge/INCLUDES) $(ge/PLAYER_SRC) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# imgdiff helper — used by matrix-test.sh for reference-image checks.
.PHONY: ge/imgdiff
ge/imgdiff: $(ge/IMGDIFF)

$(ge/IMGDIFF): $(ge)/tools/imgdiff.cpp
	@mkdir -p $(@D)
	$(CXX) -std=c++20 -O2 -I$(ge)/include -I$(ge)/vendor/include $< -o $@

# icon-gen — build-time app-icon expander (🎯T50). Takes a single source SVG
# and writes both platforms' icon resource layouts via ge::rasterizeSvgToPixels.
# Same link line as the player ($(ge/PLAYER) above): linking libge.a pulls in
# its sokol / SDL render symbols even though icon-gen only calls the CPU
# rasterization path.
ge/ICON_GEN = bin/ge-icon-gen

.PHONY: ge/icon-gen
ge/icon-gen: $(ge/ICON_GEN)

$(ge/ICON_GEN): $(ge)/tools/icon-gen.cpp $(ge/LIB)
	@mkdir -p $(@D)
	$(CXX) -std=c++20 -O2 $(ge/INCLUDES) $< $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# app-icons — convention-driven invocation of icon-gen. Reads icons/icon.svg
# from the consuming project's root and writes into the existing ios/ and
# android/ directories (assumes ge/ios-init / ge/android-init have run).
#
# Override knobs: ge/APP_ICON_SVG, ge/APP_ICON_BG_COLOR, ge/APP_ICON_IOS_OUT,
# ge/APP_ICON_ANDROID_RES_OUT.
ge/APP_ICON_SVG            ?= icons/icon.svg
# Hex fill color for the Android adaptive-icon background. NO '#' here —
# Make treats '#' as the start of a comment and would set this to empty.
# icon-gen prepends '#' itself.
ge/APP_ICON_BG_COLOR       ?= FFFFFF
ge/APP_ICON_IOS_OUT        ?= ios/Assets.xcassets/AppIcon.appiconset
ge/APP_ICON_ANDROID_RES_OUT?= android/app/src/main/res
# Project files that the wire-icons step patches idempotently so the
# generated icon resources are actually picked up by the native build.
ge/APP_ICON_IOS_INFOPLIST  ?= ios/Info.plist
ge/APP_ICON_IOS_CMAKELISTS ?= ios/CMakeLists.txt
ge/APP_ICON_ANDROID_MANIFEST ?= android/app/src/main/AndroidManifest.xml

.PHONY: ge/app-icons
ge/app-icons: $(ge/ICON_GEN) $(ge/APP_ICON_SVG)
	$(ge/ICON_GEN) \
	    --svg "$(ge/APP_ICON_SVG)" \
	    --ios-out "$(ge/APP_ICON_IOS_OUT)" \
	    --android-res-out "$(ge/APP_ICON_ANDROID_RES_OUT)" \
	    --bg-color "$(ge/APP_ICON_BG_COLOR)"
	$(ge)/tools/wire-icons.py \
	    --ios-info-plist "$(ge/APP_ICON_IOS_INFOPLIST)" \
	    --ios-cmakelists "$(ge/APP_ICON_IOS_CMAKELISTS)" \
	    --android-manifest "$(ge/APP_ICON_ANDROID_MANIFEST)"

# ────────────────────────────────────────────────
# Unit tests (doctest)
#
# `make unit-test` builds bin/ge-test from $(ge/TEST_SRC) and runs it.
# Tests link against libge.a, so they have access to all engine headers
# and engine-defined classes. The runner reaches into sokol / SDL only at
# link time (libge.a's references resolve through them); the tests
# themselves don't initialize the render backend, so they're side-effect free.
# ────────────────────────────────────────────────

ge/TEST_BIN = bin/ge-test

.PHONY: unit-test
unit-test: $(ge/TEST_BIN)
	$(ge/TEST_BIN)

$(ge/TEST_BIN): $(ge/TEST_OBJ) $(ge/LIB) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(ge/TEST_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# ────────────────────────────────────────────────
# Mobile targets
#
#   ge/ios, ge/android — build the *consuming app's* mobile distribution
#     project (the `ios/` or `android/` directory produced by ge/ios-init /
#     ge/android-init). These are the usual entry points for app authors.
#
#   (The legacy ge/player-ios* and ge/player-android* rules were retired
#    in 🎯T73.3; the brokered player is dormant pending 🎯T34.)
#
#   ge/ios-init, ge/android-init — generate the app-side ios/ or android/
#     scaffolding from ge/tools/{ios,android}-template. Parent passes APP_ID
#     and APP_NAME.
# ────────────────────────────────────────────────

# ── Consuming app's iOS build ──────────────────────────────────────

# Generate the Xcode project via build_project.rb (the xcodeproj-gem
# builder — 🎯T73.2; replaces CMake) and build .app into
# ios/build/Build/Products/Debug-iphonesimulator/.
#
# Expects ios/project.rb to exist — run `make ge/ios-init` first.
.PHONY: ge/ios
ge/ios: $(APP_SHADERS) $(ge/RENDER_SHADERS)
	@if [ ! -d ios ]; then \
	    echo "ios/ not found — run 'make ge/ios-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	bundle exec ruby ios/project.rb --simulator
	cd ios && xcodebuild \
	    -project $(APP_BIN_NAME).xcodeproj -scheme $(APP_BIN_NAME) \
	    -configuration Debug -destination "generic/platform=iOS Simulator" \
	    -derivedDataPath build \
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
#
# The brokered ge player (tools/ios + tools/android) is dormant pending
# 🎯T34, which rewrites it as a regular ge app (no bespoke per-platform
# entry points). All build rules were removed in 🎯T73.3 along with
# the iOS CMakeLists.txt that drove them. Android's gradle wrapper +
# tools/android/app/src/main/cpp/CMakeLists.txt remain in tree as
# reference until T34 lands; they currently produce a TODO-Dawn-broken
# build and aren't wired to any make target.

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

# storekit-init — copy the engine's StoreKit.storekit template into the
# consuming app's ios/ directory for use with GE_IAP_MODE=local (🎯T65.4).
#
# Run once after `make ge/ios-init`, then customise the product IDs to match
# your ge::iap::setCatalogue() registration. The file must be added to the
# Xcode target's Build Phases → Copy Bundle Resources so it lands in the app
# bundle where GEStoreKit2LocalBridgeImpl can find it at runtime.
#
# Safe to re-run — will not overwrite an existing ios/StoreKit.storekit.
.PHONY: ge/storekit-init
ge/storekit-init:
	@if [ -f ios/StoreKit.storekit ]; then \
		echo "ios/StoreKit.storekit already exists — not overwriting."; \
	else \
		mkdir -p ios; \
		cp "$(ge)/ios/StoreKit.storekit" ios/StoreKit.storekit; \
		echo "Created ios/StoreKit.storekit from engine template."; \
		echo "Next steps:"; \
		echo "  1. Edit ios/StoreKit.storekit: set product IDs to match your setCatalogue() registration."; \
		echo "  2. In Xcode: add StoreKit.storekit to Build Phases → Copy Bundle Resources."; \
		echo "  3. In Xcode: add StoreKitTest.framework as Optional in Build Phases → Link Binary With Libraries."; \
		echo "  4. Run with GE_IAP_MODE=local to activate."; \
	fi

# ────────────────────────────────────────────────
# Vendor prebuild / header lift (iOS arm64)
# ────────────────────────────────────────────────
#
# Used to refresh ge/prebuilt/ios-arm64/lib*.a + ge/headers/<vendor>/include/
# after bumping a vendor submodule SHA. Consumer apps' iOS builds link the
# prebuilt static libs and read the lifted headers, avoiding recursive
# submodule init on CI. See docs/vendor-prebuilds.md.

# ge-maintenance rules — `prebuild*`, `ge/lift-headers`, `depgraph` — live
# in ge's top-level Makefile, not here. They operate on ge's own tree and
# aren't useful to consuming apps. Run them from the ge repo root.

# ────────────────────────────────────────────────
# Generic targets (use CLEAN, COMPILE_DB_DEPS)
# ────────────────────────────────────────────────

.PHONY: clean
clean:
	rm -rf $(CLEAN)

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

# ────────────────────────────────────────────────
# Debug build
#
# `make ge/debug` builds bin/$(APP_NAME)-debug with assertions enabled,
# debug symbols, and no optimization.  The debug matrix cells exercise
# this binary rather than the default release binary.
# ────────────────────────────────────────────────

# Override points: apps can extend APP_DEBUG_OBJ or set APP_DEBUG separately.
APP_DEBUG     ?= bin/$(APP_NAME)-debug
APP_DEBUG_OBJ ?= $(patsubst %.cpp,$(BUILD_DIR)/debug/%.o,$(APP_SRC))

ge/CXXFLAGS_DEBUG = -std=c++20 -O0 -g -DDEBUG $(ge/INCLUDES)

.PHONY: ge/debug
ge/debug: $(APP_DEBUG)

$(APP_DEBUG): $(APP_DEBUG_OBJ) $(APP_SHADERS) $(ge/RENDER_SHADERS) $(ge/LIB) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(APP_DEBUG_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

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
	bundle exec ruby ios/project.rb --simulator
	cd ios && xcodebuild \
	    -project $(APP_BIN_NAME).xcodeproj -scheme $(APP_BIN_NAME) \
	    -configuration Release -destination "generic/platform=iOS Simulator" \
	    -derivedDataPath build \
	    build

# ── iOS physical-device release build ─────────────────────────────────
# `make ge/ios-device-release` builds the consuming app's iOS .app targeting
# iphoneos (physical device) with Release configuration. Uses the same
# build_project.rb-generated xcodeproj as the sim lanes — no separate
# build-tree path (🎯T73.2).

.PHONY: ge/ios-device-release
ge/ios-device-release: $(APP_SHADERS) $(ge/RENDER_SHADERS)
	@if [ ! -d ios ]; then \
	    echo "ios/ not found — run 'make ge/ios-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	bundle exec ruby ios/project.rb
	cd ios && xcodebuild \
	    -project $(APP_BIN_NAME).xcodeproj -scheme $(APP_BIN_NAME) \
	    -configuration Release -destination "generic/platform=iOS" \
	    -derivedDataPath build-device \
	    -allowProvisioningUpdates \
	    build

# ── iOS physical-device DEVELOPMENT build (deploy to your own devices) ─
# `make ge/ios-device` builds a Debug, development-signed .app for installing
# on a developer's own *already-registered* test devices.
# GE_IOS_SIGNING=development → Automatic + Apple Development (no match);
# `-allowProvisioningUpdates` mints/renews the per-developer cert + Team
# Provisioning Profile for devices the portal already knows. It does NOT
# enroll a brand-new UDID — use `make ge/ios-register-devices` first (🎯T110).
# Optional: REGISTER_DEVICES=1 make ge/ios-device  enrolls then builds.
.PHONY: ge/ios-device
ge/ios-device: $(APP_SHADERS) $(ge/RENDER_SHADERS)
	@if [ ! -d ios ]; then \
	    echo "ios/ not found — run 'make ge/ios-init APP_ID=... APP_NAME=...' first"; \
	    exit 1; \
	fi
	@if [ "$(REGISTER_DEVICES)" = "1" ]; then \
	    $(MAKE) ge/ios-register-devices; \
	fi
	GE_IOS_SIGNING=development bundle exec ruby ios/project.rb
	cd ios && xcodebuild \
	    -project $(APP_BIN_NAME).xcodeproj -scheme $(APP_BIN_NAME) \
	    -configuration Debug -destination "generic/platform=iOS" \
	    -derivedDataPath build-device-dev \
	    -allowProvisioningUpdates \
	    build

# ── iOS device enrollment (Developer Portal) — 🎯T110 ────────────────
# Headless UDID registration via ASC API key + fastlane register_devices.
# Once per device, ever. Idempotent. Does not require Xcode GUI.
#
#   make ge/ios-register-devices
#   make ge/ios-register-devices UDID=00008110-… NAME="iPhone 13"
#   DEVICES_FILE=./devices.txt make ge/ios-register-devices
#   make ge/ios-register-devices LIST_ONLY=1   # discovery smoke, no ASC call
.PHONY: ge/ios-register-devices
ge/ios-register-devices:
	@flags=""; \
	  [ "$(LIST_ONLY)" = "1" ] && flags="$$flags --list-only"; \
	  [ "$(DRY_RUN)" = "1" ] && flags="$$flags --dry-run"; \
	  UDID="$(UDID)" NAME="$(NAME)" DEVICES_FILE="$(DEVICES_FILE)" \
	    $(ge)/tools/ios-register-devices.sh $$flags

# (ge player iOS physical-device build target retired in 🎯T73.3 — see
# the player section above.)

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
#   devices run in parallel. Cells racing for the same device serialize via
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
# Cells racing for the same device serialize via spyder's reservation queue.
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

# ────────────────────────────────────────────────
# Claude Code plugin installation
#
# `make ship-init` symlinks the ge repo into ~/.claude/plugins/ge so that
# Claude Code picks up the /ge:ship, /ge:release-notes, /ge:ship-status, and
# /ge:onboard skills. Run once per checkout; re-running is safe (idempotent).
#
# The symlink points at the ge submodule root ($(ge) resolved to its absolute
# path), not at the consuming project. Claude Code loads the plugin from
# .claude-plugin/plugin.json at that root.
# ────────────────────────────────────────────────

.PHONY: ship-init
ship-init:
	@mkdir -p ~/.claude/plugins
	@if [ ! -L ~/.claude/plugins/ge ]; then \
		ln -s "$$(cd $(ge) && pwd)" ~/.claude/plugins/ge && echo "Linked ~/.claude/plugins/ge → $$(cd $(ge) && pwd)"; \
	else \
		echo "~/.claude/plugins/ge already exists"; \
	fi

# Canned recipe for the parent to expand at the end of its init target.
define ge/INIT_DONE
	@echo ""
	@echo "Setup complete. Next steps:"
	@echo "  make              # Build the application"
	@echo "  make run          # Build and run"
	@echo "  make test         # Run all tests"
endef

# ── 🎯T64.* ship substrate ───────────────────────────────────────────────────
#
# Targets for the studio-wide deployment pipeline (ge submodule).
# Consuming games include ge/Module.mk and get these targets automatically.
#
# Usage in a game repo (all require SHIP_SCHEME + env vars from docs/release-setup.md):
#
#   make ship-preflight                     # ← run first; prints READY or BLOCKED
#   make ship-alpha                         # TestFlight internal, no version bump
#   make ship-beta VERSION=0.31.0           # cuts v0.31.0-beta.N, external TF
#   make ship-release VERSION=0.31.0 CONFIRM=1  # store release, tags, submits
#   make ship-worktree TAG=v0.4.0           # create isolated build worktree
#   make ship-clean                         # prune stale worktrees (>14 days)
#   make ge/ci-init                         # copy GHA release workflow into game repo
#
# Override knobs (set in your game Makefile before the -include $(ge)/Module.mk):
#   SHIP_SCHEME        — Xcode scheme (e.g. MultiMaze2). Default: $(APP_DISPLAY_NAME).
#   SHIP_BUILD_DIR     — Where worktrees and IPAs land. Default: build/ship.
#   SHIP_CLEAN_DAYS    — Prune worktrees older than N days. Default: 14.
#   GHA_WORKFLOW_DEST  — Where ge/ci-init copies the workflow. Default: .github/workflows.

SHIP_SCHEME     ?= $(APP_DISPLAY_NAME)
SHIP_BUILD_DIR  ?= build/ship
SHIP_CLEAN_DAYS ?= 14
GHA_WORKFLOW_DEST ?= .github/workflows

# PATH for ship rules: keep the user's PATH FIRST so Homebrew Ruby/bundler/
# etc. resolve correctly, and append system paths as a fallback (for tools
# like /usr/bin/rsync that xcodebuild's codesign step needs). The original
# prepend-system pattern broke `bundle exec` on Homebrew-Ruby laptops by
# resolving to macOS system Ruby 2.6 instead.
ge/SHIP_PATH = $(PATH):/usr/bin:/bin:/usr/sbin:/sbin

# ── ship-preflight ─────────────────────────────────────────────────────────
# Standalone pre-build gate: git clean? env vars set? match works?

.PHONY: ship-preflight
ship-preflight:
	SHIP_SCHEME=$(SHIP_SCHEME) \
	APP_ID=$(APP_ID) \
	    $(ge)/tools/ship/preflight.sh \
	    --lane $(if $(LANE),$(LANE),alpha) \
	    $(if $(VERSION),--version $(VERSION),)

# ── ship-worktree ──────────────────────────────────────────────────────────
# Create an isolated worktree at TAG. Used by ship-build and tests.
# TAG is required: `make ship-worktree TAG=v0.4.0`

.PHONY: ship-worktree
ship-worktree:
	@if [ -z "$(TAG)" ]; then \
	    echo "Usage: make ship-worktree TAG=v0.4.0"; exit 1; fi
	$(ge)/tools/ship/worktree.sh --create $(TAG)

# ── ship-alpha ─────────────────────────────────────────────────────────────
# Build at HEAD, upload to TestFlight internal testers, no semver bump.

# Default SHIP_PROJECT to the conventional CMake-generated Xcode project
# path. Consumer can override (e.g. SHIP_PROJECT := ios/MyHandwritten.xcodeproj)
# before including Module.mk.
SHIP_PROJECT ?= ios/build/xcode/$(SHIP_SCHEME).xcodeproj

# `ios` is the consumer's CMake-Xcode codegen target (defined in their
# Makefile, e.g. `cd ios && cmake -G Xcode -B build/xcode ...`). ge's
# ship-* lanes depend on it so the .xcodeproj exists at SHIP_PROJECT
# before gym tries to read it. If the consumer has a hand-written
# .xcodeproj (no codegen needed), they can stub `ios:` as a no-op.

.PHONY: ship-alpha
ship-alpha: ios
	PATH=$(ge/SHIP_PATH) \
	SHIP_SCHEME=$(SHIP_SCHEME) \
	SHIP_PROJECT=$(SHIP_PROJECT) \
	APP_ID=$(APP_ID) \
	    $(ge)/tools/ship/release.sh --lane alpha

# ── ship-beta ──────────────────────────────────────────────────────────────
# Cut v{VERSION}-beta.N, upload to TestFlight external, push beta tag.
# VERSION is required: `make ship-beta VERSION=0.31.0`

.PHONY: ship-beta
ship-beta: ios
	@if [ -z "$(VERSION)" ]; then \
	    echo "Usage: make ship-beta VERSION=0.31.0"; exit 1; fi
	PATH=$(ge/SHIP_PATH) \
	SHIP_SCHEME=$(SHIP_SCHEME) \
	SHIP_PROJECT=$(SHIP_PROJECT) \
	APP_ID=$(APP_ID) \
	    $(ge)/tools/ship/release.sh --lane beta --version $(VERSION)

# ── ship-release ───────────────────────────────────────────────────────────
# Tag v{VERSION}, build, upload, submit for App Store review.
# Requires CONFIRM=1 to prevent accidental store submissions.
# VERSION is required: `make ship-release VERSION=0.31.0 CONFIRM=1`

.PHONY: ship-release
ship-release: ios
	@if [ -z "$(VERSION)" ]; then \
	    echo "Usage: make ship-release VERSION=0.31.0 CONFIRM=1"; exit 1; fi
	@if [ -z "$(CONFIRM)" ] || [ "$(CONFIRM)" != "1" ]; then \
	    echo "ERROR: make ship-release requires CONFIRM=1."; \
	    echo "  make ship-release VERSION=$(VERSION) CONFIRM=1"; exit 1; fi
	PATH=$(ge/SHIP_PATH) \
	SHIP_SCHEME=$(SHIP_SCHEME) \
	SHIP_PROJECT=$(SHIP_PROJECT) \
	APP_ID=$(APP_ID) \
	    $(ge)/tools/ship/release.sh --lane release --version $(VERSION) --confirm

# ── ship-clean ─────────────────────────────────────────────────────────────
# Prune worktrees in build/ship/ older than SHIP_CLEAN_DAYS (default 14).

.PHONY: ship-clean
ship-clean:
	$(ge)/tools/ship/worktree.sh --clean --max-age-days $(SHIP_CLEAN_DAYS)

# ── ge/ci-init ─────────────────────────────────────────────────────────────
# Copy the GHA release workflow template from ge into the consuming game repo.
# Idempotent — safe to re-run after ge submodule bumps. Copies only the
# template; the game repo's own .github/workflows/ may have other files.

.PHONY: ge/ci-init
ge/ci-init:
	@mkdir -p $(GHA_WORKFLOW_DEST)
	@src="$(ge)/.github/workflows/release.yml"; \
	dest="$(GHA_WORKFLOW_DEST)/release.yml"; \
	if [ -f "$$dest" ]; then \
	    if cmp -s "$$src" "$$dest"; then \
	        echo "ge/ci-init: $(GHA_WORKFLOW_DEST)/release.yml already up to date"; \
	    else \
	        cp "$$src" "$$dest"; \
	        echo "ge/ci-init: updated $(GHA_WORKFLOW_DEST)/release.yml (ge submodule bumped)"; \
	    fi; \
	else \
	    cp "$$src" "$$dest"; \
	    echo "ge/ci-init: created $(GHA_WORKFLOW_DEST)/release.yml"; \
	    echo "  Edit it to set APP_PACKAGE_NAME, SHIP_SCHEME, and enable the Android job."; \
	fi

# ── End of 🎯T64.* ship substrate ─────────────────────────────────────────

# Dep-file include for the app's own objects. Engine object .d files are
# already picked up by their own implicit pattern-rule dep tracking.
-include $(APP_OBJ:.o=.d)
