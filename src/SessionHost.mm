// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::run() dispatcher + runDirect() (distribution modality).
//
// The brokered implementation (runBrokered) lives in SessionHost_brokered.mm
// which is only compiled when the parent build wants brokered support (i.e.
// not mobile distribution). Define GE_DIRECT_ONLY to compile just runDirect
// without pulling in the bridge subsystem, WebSocketClient, ServerWireBridge
// etc.

#include <ge/SessionHost.h>
#include <ge/Signal.h>
#include <ge/Protocol.h>

#include "Immersive.h"
#include "render/DirectRenderHost.h"

#include <SDL3/SDL.h>
#include <ge/appchannel.h>
#include <ge/log.h>
#include <spdlog/spdlog.h>

#include <string>

namespace ge {

// Brokered entry point — defined in SessionHost_brokered.mm when included.
// For GE_DIRECT_ONLY builds, we stub it out with a runtime error.
#ifdef GE_DIRECT_ONLY
static void runBrokered(Factory, const SessionHostConfig&) {
    SPDLOG_ERROR("ge::run: headless/brokered modality requested but this build "
                 "was compiled with GE_DIRECT_ONLY — relink with the bridge "
                 "subsystem enabled.");
}
#else
void runBrokered(Factory factory, const SessionHostConfig& config);
#endif

// ── runDirect: standalone / distribution modality ─────────────────
//
// Render + engine in one process, no ged, no wire. Uses DirectRenderHost
// for window + bgfx + input. The host owns the session Context (db
// setup, dimensions, safe-area); the run loop just relays it.
static void runDirect(Factory factory, const SessionHostConfig& config) {
    DirectRenderHost host(config);
    applyImmersive(config.immersive);
    RunConfig rc = factory(host.context());
    host.setEventHandler(rc.onEvent);
    host.setBackPressedHandler(rc.onBackPressed);
    host.setMemoryWarningHandler(rc.onMemoryWarning);

    wire::SessionConfig sc{};
    sc.sensors = config.sensors;
    sc.orientation = config.orientation;
    host.send(sc);

    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();

    while (!host.shouldQuit()) {
        host.pumpEvents();

        uint64_t now = SDL_GetPerformanceCounter();
        float dt = float(now - last) / float(freq);
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        // While the host is paused, skip the entire render bracket so we
        // never touch a backgrounded surface. Android: the swap chain is
        // torn down and SDL blocks the loop anyway. iOS (🎯T88): a
        // backgrounded scene can't get a Metal command buffer, so
        // beginFrame would wedge and trip the scene-update watchdog;
        // pumpEvents has already blocked on SDL_WaitEventTimeout (idle,
        // not spinning) until the next OS event. onUpdate is skipped while
        // paused; reset `last` so the first foreground frame doesn't see a
        // multi-second dt.
        if (host.paused()) {
            last = SDL_GetPerformanceCounter();
            continue;
        }

        // 🎯T92.4 Feed the real (pre-time-control) frame time to the perf push
        // accumulator; it emits a {frame_ms, counters} sample ~once per second
        // when an app-channel is live (no-op otherwise).
        ge::appchannel::perfTick(dt * 1000.0f);

        // 🎯T92.2 Apply dev time-control (app_pause/resume/step/speed). A
        // no-op pass-through unless an app-channel is driving it; returns 0
        // while paused (render + input below still run), kStepDt per stepped
        // frame, or dt×speed otherwise.
        if (rc.onUpdate) rc.onUpdate(ge::appchannel::applyTimeControl(dt));

        host.beginFrame();
        if (rc.onRender) rc.onRender(host.context());
        // Self-incrementing frame counter (T38: bgfx::frame() removed; sokol
        // commits inside SokolContext::endFrame and has no equivalent return).
        // ServerWireBridge used the bgfx frame number to correlate async
        // readbacks; not needed for the direct-render path.
        static uint32_t frameNum = 0;
        host.endFrame(++frameNum);
    }

    if (rc.onShutdown) rc.onShutdown();
}

// ── ge::run — dispatch by modality ────────────────────────────────

void run(Factory factory, const SessionHostConfig& config) {
    // Install the cross-platform spdlog sink (🎯T66) before any other
    // engine logging — SDL/bgfx/IAP startup all emit SPDLOG_INFO and
    // we want those visible on iOS/Android logs without per-app
    // sink wiring. Idempotent if a consumer has already installed.
    ge::log::install();

    // 🎯T92 Dev-only bidirectional RPC channel to spyder (app_channel_*),
    // activated when LOG_TARGET is "appchannel://host:port". No-op otherwise
    // and compiled out in release. ge-internal method handlers (ping today;
    // lifecycle/time/input in later leaves) are registered inside this call
    // before the hello handshake advertises them.
    ge::appchannel::installFromEnv(config.appName ? config.appName : "ge", "dev");

    if (config.headless) {
        runBrokered(factory, config);
    } else {
        runDirect(factory, config);
    }
}

} // namespace ge
