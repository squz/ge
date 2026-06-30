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
#include <ge/png.h>
#include <spdlog/spdlog.h>

#include "render/ScreenshotBridge.h"

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

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
// for window + sokol + input. The host owns the session Context (db
// setup, dimensions, safe-area); the run loop just relays it.
static void runDirect(Factory factory, const SessionHostConfig& config) {
    DirectRenderHost host(config);
    applyImmersive(config.immersive);

    // 🎯T136 Crash diagnostics: gate the callback guards (below) on the same
    // flag, and install the fatal-signal last-gasp handlers. Done before the
    // factory so a crash in the consumer's construction is also surfaced.
    ge::setCrashDiagnosticsEnabled(config.crashDiagnostics);
    if (config.crashDiagnostics) ge::installCrashHandlers();

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
    float lastReportedFps = 0.0f;  // 🎯T111 onMetrics deviation gate state

    while (!host.shouldQuit()) {
        // 🎯T114 Apple: drain autoreleased objects every frame. sokol's Metal
        // backend autoreleases per-pass objects (MTLRenderPassDescriptor,
        // attachment arrays, command encoders/buffers, CAMetalDrawable
        // wrappers); ge runs under SDL_main rather than sokol_app, so nothing
        // else pops a pool — without this, every per-frame object accumulates
        // for process lifetime, retained drawables pin their IOSurfaces, and
        // multi-pass consumers jetsam in minutes (multimaze2 🎯T47: ~65
        // passes/frame ⇒ ~31 MB/s leaked on an iPhone 13). On Android the
        // .mm is compiled `-x c++` (no __OBJC__), leaving a plain block.
#if defined(__OBJC__)
        @autoreleasepool
#endif
        {
            host.pumpEvents();
            // 🎯T92.5 Run any state-registry tasks the app-channel marshalled
            // onto the game thread (state_query / save_state / restore_state),
            // so they see a consistent snapshot. No-op when no channel is
            // active.
            ge::appchannel::pumpMainThreadTasks();

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

            // 🎯T132/T131.1 Render-on-demand: render this frame iff continuous
            // mode is on, OR a one-shot redraw is pending (input / requestRedraw,
            // the *edge* signal), OR any registered render trigger is active (the
            // *level* signal — a physics sim still awake, an animation still
            // moving). Otherwise skip the whole draw+present: pumpEvents has
            // already idled the thread (SDL_WaitEventTimeout, ~0% CPU), so this
            // just avoids GPU/encode/present work on a static screen. takeRedraw()
            // runs unconditionally so the one-shot is always consumed. Reset
            // `last` so the next rendered frame doesn't see a multi-tick dt.
            {
                const auto& ctx      = host.context();
                const bool  redraw   = ctx.takeRedraw();
                const bool  wantFrame = ctx.continuousRendering() || redraw ||
                                        ctx.anyRenderTriggerActive();
                if (!wantFrame) {
                    last = SDL_GetPerformanceCounter();
                    continue;
                }
            }

            // 🎯T92.4 Feed the real (pre-time-control) frame time to the perf
            // push accumulator; it emits a {frame_ms, counters} sample ~once
            // per second when an app-channel is live (no-op otherwise).
            ge::appchannel::perfTick(dt * 1000.0f);

            // 🎯T92.2 Apply dev time-control (app_pause/resume/step/speed). A
            // no-op pass-through unless an app-channel is driving it; returns 0
            // while paused (render + input below still run), kStepDt per
            // stepped frame, or dt×speed otherwise.
            if (rc.onUpdate) {
                ge::guardCallback("onUpdate", [&] {  // 🎯T136
                    rc.onUpdate(ge::appchannel::applyTimeControl(dt));
                });
            }

            // 🎯T101 Refresh per-frame Context state (resize, insets, parallax,
            // tilt) before onRender. The game opens this frame's swapchain pass
            // itself via ctx.swapchainPass() at the top of onRender; all
            // sg_begin/end_pass + commit/present (direct) or encode/transmit
            // (wire) live inside that ge::Pass's lifetime, so the loop no
            // longer brackets the frame.
            host.refreshFrame(dt);

            // 🎯T111 Emit a perf-metrics report when smoothed fps has moved
            // enough since the last report (or every frame at threshold 0).
            // Opt-in: nothing happens unless the game set rc.onMetrics. ge
            // reports; the app decides — no quality stepping here. Feeds the
            // real (pre-time-control) fps so the reading is the true render
            // rate even when an app-channel slows game time, and runs after the
            // paused() continue above so paused frames don't pollute the
            // average.
            if (rc.onMetrics) {
                const float f = host.context().fps();
                if (shouldReportMetrics(f, lastReportedFps,
                                        config.metricsReportThreshold)) {
                    lastReportedFps = f;
                    rc.onMetrics(Metrics{.fps = f,
                                         .frameTime = host.context().frameTime()});
                }
            }

            if (rc.onRender) {
                ge::guardCallback("onRender", [&] {  // 🎯T136
                    rc.onRender(host.context());
                });
            }
        }
    }

    if (rc.onShutdown) rc.onShutdown();
}

// ── ge::run — dispatch by modality ────────────────────────────────

void run(Factory factory, const SessionHostConfig& config) {
    // Install the cross-platform spdlog sink (🎯T66) before any other
    // engine logging — SDL/sokol/IAP startup all emit SPDLOG_INFO and
    // we want those visible on iOS/Android logs without per-app
    // sink wiring. Idempotent if a consumer has already installed.
    ge::log::install();

    // 🎯T92 Dev-only bidirectional RPC channel to spyder (app_channel_*),
    // activated when SPYDER_APP_CHANNEL is "host:port" (🎯T119). No-op otherwise
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

// ── renderBatch / renderToPng — 🎯T124 headless render ─────────────
int renderBatch(Factory factory, SessionHostConfig config,
                const std::vector<RenderItem>& items) {
    ge::log::install();
    config.headless = false;   // direct host, no ged / wire
    config.hidden   = true;    // unmapped window — nothing shown
    DirectRenderHost host(config);
    applyImmersive(false);
    RunConfig rc = factory(host.context());
    host.setEventHandler(rc.onEvent);

    int ok = 0;
    for (const auto& item : items) {
        try {
            if (item.prepare) item.prepare();
            std::vector<std::uint8_t> rgba;
            int w = 0, h = 0;
            const bool captured = ge::detail::captureFrameRGBASync(
                [&] {
                    host.refreshFrame(0.0f);
                    if (rc.onRender) rc.onRender(host.context());
                },
                rgba, w, h);
            if (captured && w > 0 && h > 0 &&
                ge::writePng(item.outPath, rgba.data(), w, h)) {
                ++ok;
                SPDLOG_INFO("renderBatch: {}x{} -> {}", w, h, item.outPath);
            } else {
                SPDLOG_ERROR("renderBatch: no frame for '{}'", item.outPath);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("renderBatch: '{}' threw: {}", item.outPath, e.what());
        }
    }
    if (rc.onShutdown) rc.onShutdown();
    return ok;
}

bool renderToPng(Factory factory, SessionHostConfig config,
                 const std::function<void()>& prepare,
                 const std::string& outPath) {
    return renderBatch(std::move(factory), std::move(config),
                       {{prepare, outPath}}) == 1;
}

} // namespace ge
