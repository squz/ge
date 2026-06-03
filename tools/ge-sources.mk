# Canonical ge source manifest — the single source of truth for which
# source files belong in libge.a.
#
# Consumed by two callers:
#   - Module.mk (consumer / desktop builds) `include`s this file directly
#     and references the variables below to compose its ge/SRC_DIRECT
#     and ge/SRC_BROKERED lists.
#   - tools/prebuild.sh (iOS / Android prebuilds) shells out to
#     `make -s -f tools/ge-sources.mk print-direct-<platform>` to extract
#     the same lists.
#
# Add a source here exactly once. Tag it with the platform groups it
# belongs to; the print targets at the bottom expose the per-platform
# rollups. Don't maintain a second copy anywhere.
#
# Style note: file paths use simple POSIX form (no $(ge)/ prefix) so
# tools/prebuild.sh can use them as-is. Module.mk adds the prefix in
# its include-time composition (see the conversion line below).

# ── platform-agnostic core ─────────────────────────────────────────
# These compile on every platform. No window/render-API init — just
# resource/file I/O, sprite/svg/text/png helpers, button + sdl_input,
# IAP / log / audio (cross-platform) entry points, the build manifest
# reader (🎯T64.5), the long-press detector (🎯T65.6), session host,
# the direct-render host, and sqlpipe. T38: sokol context setup lives
# in per-platform glue (SokolContext.mm on Apple, SokolContext_android.cpp
# on Android) — no single cross-platform file.
GE_SRC_COMMON := \
	src/Context.cpp \
	src/appchannel.cpp \
	src/Resource.cpp \
	src/FileIO.cpp \
	src/Signal.cpp \
	src/sprite.cpp \
	src/debug.cpp \
	src/Pass.cpp \
	src/svg.cpp \
	src/png.cpp \
	src/text.cpp \
	src/iap.cpp \
	src/log.cpp \
	src/audio.cpp \
	src/button.cpp \
	src/long_press.cpp \
	src/sdl_input.cpp \
	src/manifest.cpp \
	src/SessionHost.mm \
	src/render/DirectRenderHost.mm \
	vendor/src/sqlpipe.cpp

# ── per-platform glue ──────────────────────────────────────────────
# Same logical role on each platform; just different implementations.
# T38: SokolContext.mm (Apple Metal) + SokolContext_android.cpp (Android
# GLES3) replace the old BgfxContext.mm unified .mm.
# T74: iap_apple_local.swift is intentionally NOT in this list — the
# prebuild shell pipeline doesn't compile Swift, and the iOS xcodeproj-
# gem builder lists Swift sources in tools/ios-build/build_project.rb's
# GE_DIRECT_SOURCES separately. The LocalBridge runtime-fallback in
# iap_apple.mm tolerates the class being absent (logs + uses production
# bridge); ge::iap's T74 stub-mode fallback means no live calls.
GE_SRC_APPLE := \
	src/FontLoader_apple.mm \
	src/iap_apple.mm \
	src/audio_apple.mm \
	src/log_apple.mm \
	src/Attitude_apple.mm \
	src/SokolContext.mm \
	src/render/RefreshRateBoost_apple.mm

GE_SRC_ANDROID := \
	src/FontLoader_android.cpp \
	src/SdlContext_android.cpp \
	src/Immersive_android.cpp \
	src/CutoutInsets_android.cpp \
	src/Attitude_android.cpp \
	src/iap_android.cpp \
	src/log_android.cpp \
	src/SokolContext_android.cpp \
	src/render/RefreshRateBoost_android.cpp

# ── desktop / no-mobile stubs ──────────────────────────────────────
# Linux/macOS desktop and Apple platforms that don't have an Immersive
# or CutoutInsets concept get the stub. (Android has real
# Immersive_android.cpp + CutoutInsets_android.cpp.)
GE_SRC_STUB_IMMERSIVE := \
	src/Immersive_stub.cpp \
	src/CutoutInsets_stub.cpp

# Attitude has three: apple (CoreMotion), android (sensors), stub
# (no-op). Pick whichever the platform tag asks for.
GE_SRC_STUB_ATTITUDE := \
	src/Attitude_stub.cpp

# ── orientation lock ───────────────────────────────────────────────
# iOS uses a real swizzle (Apple TN3192). Every other platform — macOS
# desktop, Android, Linux desktop — uses the stub.
GE_SRC_ORIENTATION_IOS  := tools/player_orientation_ios.mm
GE_SRC_ORIENTATION_STUB := tools/player_orientation_stub.cpp

# ── brokered (wire / networking) mode ──────────────────────────────
# Included only when GE_DIRECT_ONLY is NOT set — i.e., when ge is built
# in wire mode (ged + remote-rendering server/player). The prebuilt
# .a's always omit these; only desktop builds in non-direct mode pick
# them up.
GE_SRC_BROKERED := \
	src/bridge/SessionHost_brokered.mm \
	src/render/PlayerRender.cpp \
	src/bridge/ServerWireBridge.mm \
	src/bridge/PlayerWireBridge.cpp \
	src/bridge/WebSocketClient.cpp \
	src/bridge/VideoEncoder_apple.mm \
	src/bridge/VideoDecoder_apple.mm

# ── per-platform rollups ───────────────────────────────────────────
# These are what callers actually consume. Each combines the right
# subset of the groups above.

# Apple direct-mode (macOS desktop, iOS device, iOS sim):
#   core + apple glue + immersive stubs + iOS-or-stub orientation
GE_SRC_DIRECT_APPLE_DESKTOP    := $(GE_SRC_COMMON) $(GE_SRC_APPLE) $(GE_SRC_STUB_IMMERSIVE) $(GE_SRC_ORIENTATION_STUB)
GE_SRC_DIRECT_IOS              := $(GE_SRC_COMMON) $(GE_SRC_APPLE) $(GE_SRC_STUB_IMMERSIVE) $(GE_SRC_ORIENTATION_IOS)

# Android direct-mode: core + android glue + stub orientation.
GE_SRC_DIRECT_ANDROID          := $(GE_SRC_COMMON) $(GE_SRC_ANDROID) $(GE_SRC_ORIENTATION_STUB)

# ── helper print targets (for tools/prebuild.sh extraction) ────────
# `make -s -f tools/ge-sources.mk print-direct-ios` etc. Newline-separated
# so bash mapfile / read can ingest line-at-a-time.
#
# These targets exist only for explicit `make -f … print-direct-<platform>`
# extraction by tools/prebuild.sh. As a manifest-only include, ge-sources.mk
# must stay GOAL-TRANSPARENT: the first target it defines (print-direct-ios)
# would otherwise become the parent's .DEFAULT_GOAL, so a bare `make` in a
# consumer would print the iOS source list instead of building the app.
# Save the parent's default goal here and restore it after the targets.
ge_sources_saved_goal := $(.DEFAULT_GOAL)

print-direct-ios:
	@printf '%s\n' $(GE_SRC_DIRECT_IOS)

print-direct-android:
	@printf '%s\n' $(GE_SRC_DIRECT_ANDROID)

print-direct-apple-desktop:
	@printf '%s\n' $(GE_SRC_DIRECT_APPLE_DESKTOP)

print-brokered:
	@printf '%s\n' $(GE_SRC_BROKERED)

# Restore the parent's default goal (see the goal-transparency note above
# the print targets). Empty before this include → stays empty, so the next
# real target the parent defines (Module.mk's `all:`) becomes the default.
.DEFAULT_GOAL := $(ge_sources_saved_goal)
