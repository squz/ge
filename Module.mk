# ge engine module
# Included by a consuming project's Makefile.
#
# ── Project vs ge responsibility ───────────────────────────────────
# The consuming project supplies APP_NAME / APP_SRC / game logic only.
# Build variants, deploy, stream server packaging, player, and host
# entrypoints are **ge's concern** — do not reinvent GE_SERVER_BUILD or
# dual-host stories in the game Makefile.
#
# Typical usage — minimal app Makefile:
#
#   ge          := ge
#   APP_NAME    := mygame
#   APP_SRC     := src/main.cpp src/Scene.cpp
#   APP_SHADERS := build/shaders/simple.h
#
#   -include $(ge)/Module.mk
#   $(ge)/Module.mk:
#           git submodule update --init --recursive
#
# First-class products (no project flags required):
#   make / make game     → bin/$(APP_NAME)         windowed game (direct)
#   make server          → bin/$(APP_NAME)-server  console stream host
#   (player glass is spyder's `make player`, not built here)
#   make run / make run-server
#
# These are totally different builds (separate object dirs, -DGE_SERVER_BUILD
# only on the server). Not a runtime mode inside one binary.
#
# The `ge` variable is the relative path from the app Makefile to the ge
# repository root. Submodule apps use the default `ge := ge`. In-tree
# samples that live inside the ge repo set it to `../..` etc.
ge ?= ge

# ────────────────────────────────────────────────
# Build configuration (app-overridable)
# ────────────────────────────────────────────────

# Internal: GE_SERVER=1 is set by the `server` target only — apps should
# call `make server`, not invent packaging flags.
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
	-I$(ge)/vendor/github.com/marcelocantos/deepparser/src \
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

# Desktop stream glass lives in marcelocantos/spyder (`make player` there).
# ge does not build the player binary. tools/player_orientation_* stay for
# DirectRenderHost orientation lock on direct apps.

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

# deepparser / liteparser (C code, used by sqlpipe for query analysis).
# sqlpipe ≥0.29 needs deepparser's sqldeep AST extensions (not sqliteai liteparser).
ge/LITEPARSER_DIR = $(ge)/vendor/github.com/marcelocantos/deepparser/src
ge/LITEPARSER_SRC = $(addprefix $(ge/LITEPARSER_DIR)/,arena.c liteparser.c lp_tokenize.c lp_unparse.c parse.c)
ge/LITEPARSER_OBJ = $(patsubst $(ge/LITEPARSER_DIR)/%.c,$(BUILD_DIR)/ge/vendor/liteparser/%.o,$(ge/LITEPARSER_SRC))

# Vendor C++ libraries (compiled into libge.a)
# sqlift is amalgamated inside sqlpipe.cpp (sqlpipe ≥0.22) — do not also
# compile vendor/src/sqlift.cpp (duplicate symbols).
ge/VENDOR_CPP_SRC = $(ge)/vendor/src/sqlpipe.cpp
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
	$(ge)/src/CmdStream_test.cpp \
	$(ge)/src/AccelScreen_test.cpp \
	$(ge)/src/AccelSynth_test.cpp \
	$(ge)/src/SeatPolicy_test.cpp \
	$(ge)/src/ViewerMetrics_test.cpp \
	$(ge)/src/StreamDbPolicy_test.cpp
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

# ────────────────────────────────────────────────
# Unit tests (doctest) — 🎯T156 oracles live here
# ────────────────────────────────────────────────
ge/TEST_BIN = bin/ge-test

.PHONY: unit-test
unit-test: $(ge/TEST_BIN)
	$(ge/TEST_BIN)

$(ge/TEST_BIN): $(ge/TEST_OBJ) $(ge/LIB) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(ge/TEST_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# Default target — `make` with no args builds the windowed game.
# Parent can declare its own `all:` BEFORE the include to win.
#
# Products are **ge-owned** names. Projects should not invent GE_SERVER=1
# packaging; use `make server` / `make run-server`.
# Player glass: spyder repo (`make player`), not here.
.PHONY: all game server run run-server player
all: game
game: $(APP)

# Console stream host — totally different build (build-server/, -DGE_SERVER_BUILD,
# bin/$(APP_NAME)-server). Recursive make isolates variables cleanly.
server:
	$(MAKE) GE_SERVER=1 APP_NAME=$(APP_NAME) ge=$(ge) bin/$(APP_NAME)-server

run: $(APP)
	./$(APP)

# Relay address is *runtime* env (spyder), not a build mode. Default local daemon.
run-server: server
	GE_SERVER=$${GE_SERVER:-127.0.0.1:3030} ./bin/$(APP_NAME)-server

# Player binary is owned by spyder (no path coupling from ge → spyder).
player ge/player:
	@echo "error: stream player lives in the spyder repo."
	@echo "  cd <spyder-checkout> && make player"
	@exit 1


# Default link rule. Parent can override by declaring its own $(APP) rule.
$(APP): $(APP_OBJ) $(APP_SHADERS) $(ge/RENDER_SHADERS) $(ge/LIB) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CXX) $(APP_OBJ) $(APP_LIBS) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

# App objects — .cpp files under src/ compile into $(BUILD_DIR)/src/*.o.
$(BUILD_DIR)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c $< -o $@

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

