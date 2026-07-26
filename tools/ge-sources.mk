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
	src/metrics.cpp \
	src/DeviceTier.cpp \
	src/Resource.cpp \
	src/FileIO.cpp \
	src/Signal.cpp \
	src/sprite.cpp \
	src/debug.cpp \
	src/solid.cpp \
	src/Pass.cpp \
	src/svg.cpp \
	src/png.cpp \
	src/texture_load.cpp \
	src/CubeSphere.cpp \
	src/text.cpp \
	src/iap.cpp \
	src/log.cpp \
	src/audio.cpp \
	src/button.cpp \
	src/long_press.cpp \
	src/sdl_input.cpp \
	src/manifest.cpp \
	src/CmdStream.cpp \
	src/SessionHost.mm \
	src/render/DirectRenderHost.mm \
	src/render/SensorControl.cpp \
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
	src/DeviceTier_apple.mm \
	src/SokolContext.mm \
	src/render/RefreshRateBoost_apple.mm

GE_SRC_ANDROID := \
	src/FontLoader_android.cpp \
	src/SdlContext_android.cpp \
	src/Immersive_android.cpp \
	src/CutoutInsets_android.cpp \
	src/Attitude_android.cpp \
	src/DeviceTier_android.cpp \
	src/iap_android.cpp \
	src/log_android.cpp \
	src/SokolContext_android.cpp \
	src/render/RefreshRateBoost_android.cpp \
	tools/sokol-dispatch/generated/ge_sokol_dispatch.c \
	tools/vkprobe/ge_vkprobe.c

# 🎯T157 Web (Emscripten/wasm) glue. Single WebGL2 backend, so SOKOL_IMPL is
# inline in SokolContext_web.cpp (the Apple model — no T107 dispatch shim).
# iap/log/audio need no web variant: neither __APPLE__ nor __ANDROID__ is
# defined, so their cross-platform paths (StubStore, stderr sink → browser
# console, plain SDL audio) apply.
GE_SRC_WEB := \
	src/FontLoader_web.cpp \
	src/SokolContext_web.cpp \
	src/render/RefreshRateBoost_web.cpp
# 🎯T107: the sokol IMPL is NOT in libge on Android — it lives in
# libgesokol-{gles,vk}.so. libge instead links the generated dispatch shim
# (real sg_* → the table a backend .so installs). Apple keeps SOKOL_IMPL inline
# in SokolContext.mm (one backend), so GE_SRC_APPLE is unchanged.
# ge_vkprobe.c (compiled WITHOUT GE_VKPROBE_CLI, so no main) is the Vulkan
# capability probe SokolContext_android calls to pick a backend; it references
# only vkGetInstanceProcAddr (resolved at the consumer's libmain link).

# ── desktop / no-mobile stubs ──────────────────────────────────────
# Linux/macOS desktop has no Immersive/CutoutInsets concept (stub).
# iOS has real Immersive_apple.mm (status-bar hide for stream/direct
# pixel parity); CutoutInsets stay stub on Apple. Android has real
# Immersive_android.cpp + CutoutInsets_android.cpp.
GE_SRC_STUB_IMMERSIVE := \
	src/Immersive_stub.cpp \
	src/CutoutInsets_stub.cpp

GE_SRC_IOS_IMMERSIVE := \
	src/Immersive_apple.mm \
	src/CutoutInsets_stub.cpp

# Attitude has three: apple (CoreMotion), android (sensors), stub
# (no-op). Pick whichever the platform tag asks for.
GE_SRC_STUB_ATTITUDE := \
	src/Attitude_stub.cpp

# DeviceTier has three: apple (sysctlbyname), android (sysinfo), stub
# (0 RAM — web / other, where physical RAM isn't queryable).
GE_SRC_STUB_DEVICE_TIER := \
	src/DeviceTier_stub.cpp

# ── orientation lock ───────────────────────────────────────────────
# iOS uses a real swizzle (Apple TN3192). Every other platform — macOS
# desktop, Android, Linux desktop — uses the stub.
GE_SRC_ORIENTATION_IOS  := tools/player_orientation_ios.mm
GE_SRC_ORIENTATION_STUB := tools/player_orientation_stub.cpp

# ── wire (server) mode ─────────────────────────────────────────────
# 🎯T92.2.2 Included only when GE_DIRECT_ONLY is NOT set — i.e., when ge is
# built in wire mode. Server mode (SessionHost_server.mm + ServerSession.mm) is
# a hidden-window DirectRenderHost streaming ge's canonical H.264 wire to a
# player attached via spyder's relay. The player client (decode/render/input)
# lives in the spyder repo — protocol-only coupling; do not link player
# sources back into libge. Prebuilt .a's always omit these; only desktop
# builds in non-direct mode pick them up.
GE_SRC_BROKERED := \
	src/bridge/SessionHost_server.mm \
	src/bridge/ServerSession.mm \
	src/bridge/WebSocketClient.cpp \
	src/bridge/VideoEncoder_apple.mm \
	src/bridge/VideoDecoder_apple.mm

# ── per-platform rollups ───────────────────────────────────────────
# These are what callers actually consume. Each combines the right
# subset of the groups above.

# Apple direct-mode (macOS desktop, iOS device, iOS sim):
#   core + apple glue + immersive (real on iOS, stub on desktop) + orientation
GE_SRC_DIRECT_APPLE_DESKTOP    := $(GE_SRC_COMMON) $(GE_SRC_APPLE) $(GE_SRC_STUB_IMMERSIVE) $(GE_SRC_ORIENTATION_STUB)
GE_SRC_DIRECT_IOS              := $(GE_SRC_COMMON) $(GE_SRC_APPLE) $(GE_SRC_IOS_IMMERSIVE) $(GE_SRC_ORIENTATION_IOS)

# Android direct-mode: core + android glue + stub orientation.
GE_SRC_DIRECT_ANDROID          := $(GE_SRC_COMMON) $(GE_SRC_ANDROID) $(GE_SRC_ORIENTATION_STUB)

# 🎯T157 Web direct-mode: core + web glue + every stub (no chrome, no
# sensors, no orientation lock in the browser). Never combined with
# GE_SRC_BROKERED — the web build is direct-only by construction.
GE_SRC_DIRECT_WEB              := $(GE_SRC_COMMON) $(GE_SRC_WEB) $(GE_SRC_STUB_IMMERSIVE) $(GE_SRC_STUB_ATTITUDE) $(GE_SRC_STUB_DEVICE_TIER) $(GE_SRC_ORIENTATION_STUB)

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

print-direct-web:
	@printf '%s\n' $(GE_SRC_DIRECT_WEB)

print-brokered:
	@printf '%s\n' $(GE_SRC_BROKERED)

# Restore the parent's default goal (see the goal-transparency note above
# the print targets). Empty before this include → stays empty, so the next
# real target the parent defines (Module.mk's `all:`) becomes the default.
.DEFAULT_GOAL := $(ge_sources_saved_goal)
