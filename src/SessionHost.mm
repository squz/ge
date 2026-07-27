// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::run() dispatcher + runDirectHosted() (direct + server modality).
//
// 🎯T92.2.2 One render host, one wire. The windowed path and server mode share
// runDirectHosted() (a hidden-window DirectRenderHost streaming ge's canonical
// wire); server mode is grafted on via a ServerHook (RunDirect.h). runServer —
// which builds the ServerSession and calls runDirectHosted — lives in the wire
// TU (src/bridge/SessionHost_server.mm), compiled only when the parent build
// wants wire support (i.e. not mobile distribution). Define GE_DIRECT_ONLY to
// compile just the windowed path without pulling in the bridge subsystem
// (ServerSession, WebSocketClient, VideoEncoder); runServer is then stubbed.

#include <ge/SessionHost.h>
#include <ge/Signal.h>
#include <ge/Protocol.h>

#include "Immersive.h"
#include "RunDirect.h"
#include "render/DirectRenderHost.h"
#include "../tools/player_orientation.h"

#include <SDL3/SDL.h>
#include <ge/appchannel.h>
#include <ge/debug.h>  // 🎯T174 internal::bindContext
#include <ge/log.h>
#include <ge/png.h>
#include <spdlog/spdlog.h>

#include "render/ScreenshotBridge.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

// 🎯T157 Suspend the wasm stack until the browser's next animation frame.
// Requires -sASYNCIFY (see cmake/web-wasm.cmake): Asyncify saves the live
// frames (main → ge::run → runDirectHosted) and rewinds them on resume, so
// the blocking loop below runs unmodified — locals stay alive, destructors
// never run spuriously, and a hidden tab (RAF stops) pauses the loop.
//
// Why not emscripten_set_main_loop(simulate_infinite_loop)? That "never
// returns" by throwing a JS exception through the wasm stack, and with
// -fwasm-exceptions every C++ cleanup pad on the way out CATCHES the
// foreign exception and runs its destructors — host/rc/the consumer's
// pre-ge::run state were all destroyed with the RAF callback still
// registered (memory fault on the first tick). Asyncify's unwind is a
// code transform, not an exception, so no cleanup pads fire.
EM_ASYNC_JS(void, ge_web_await_frame, (), {
    await new Promise(resolve => requestAnimationFrame(resolve));
});
#endif

namespace ge {

// 🎯T92.2.2 Server entry point — a hidden-window DirectRenderHost that streams
// ge's canonical wire, defined in the wire TU (src/bridge/SessionHost_server.mm),
// compiled ONLY into a server build. ge::run() dispatches here at COMPILE time
// via GE_SERVER_BUILD; a desktop/mobile build never references runServer, so the
// static linker never pulls the ServerSession / WebSocketClient / VideoEncoder
// objects (smaller, and a shipped direct build can't be coerced into a server).
#if defined(GE_SERVER_BUILD)
void runServer(Factory factory, const SessionHostConfig& config);
#endif

// ── runDirectHosted: standalone / distribution / server modality ──
//
// Render + engine in one process, no stream broker. Uses DirectRenderHost for window +
// sokol + input. The host owns the session Context (db setup, dimensions,
// safe-area); the run loop just relays it. When `server` is non-null the same
// loop drives server mode: the host is hidden, capture is armed each frame, and
// the render-on-demand gate is skipped so the player gets a continuous stream.
// (Declared in RunDirect.h so runServer in the wire TU can call it.)
//
// 🎯T157 The per-iteration body lives in DirectLoop::step() so a host that
// cannot block the thread (the web build's requestAnimationFrame callback)
// drives the same body one tick at a time; native platforms keep the
// blocking while-loop below.

namespace {

struct DirectLoop {
    DirectRenderHost& host;
    RunConfig& rc;
    const SessionHostConfig& config;
    const ServerHook* server;

    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();
    float lastReportedFps = 0.0f;  // 🎯T111 onMetrics deviation gate state

    // 🎯T92.2.2 Server pacing. A hidden window gets no vsync back-pressure
    // from present, so the loop spins as fast as encode allows (observed
    // ~108 fps) — overworking the encoder and flooding the player faster
    // than it can decode. Pace server mode to the encoder's 60 fps with
    // absolute timestamps (no drift). Windowed mode keeps vsync pacing.
    static constexpr uint64_t kServerFps = 60;
    const uint64_t paceStart = last;
    uint64_t paceIndex = 0;

    void step();
};

void DirectLoop::step() {
    if (server) {
        const uint64_t target = paceStart + paceIndex * freq / kServerFps;
        uint64_t now = SDL_GetPerformanceCounter();
        if (now < target) {
            const int64_t sleepTicks =
                int64_t(target - now) - int64_t(freq / 1000);
            if (sleepTicks > 0)
                SDL_Delay(uint32_t(sleepTicks * 1000 / freq));
            while (SDL_GetPerformanceCounter() < target) {}
        }
        ++paceIndex;
    }
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
        // 🎯T109 Full-surface pts for hit_targets center_norm / bbox_norm.
        {
            const ge::Rect full = host.context().fullRectInPts();
            ge::appchannel::setHitTargetSurfacePts(full.w, full.h);
        }

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
            return;
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
            // 🎯T92.2.2 In server mode a remote player needs a continuous
            // frame stream, so bypass the render-on-demand gate entirely —
            // render every frame regardless of the app's on-demand opt-in.
            const bool  wantFrame = (server && server->active) ||
                                    ctx.continuousRendering() || redraw ||
                                    ctx.anyRenderTriggerActive();
            if (!wantFrame) {
                last = SDL_GetPerformanceCounter();
                return;
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

        // 🎯T128 cmdstream: arm LiveCapture around onRender so SpriteBatch
        // serialises SP2S for the wire (server modality only).
        if (server && server->beforeRender)
            server->beforeRender(host.width(), host.height());
        if (rc.onRender) {
            ge::guardCallback("onRender", [&] {  // 🎯T136
                rc.onRender(host.context());
            });
        }
        if (server && server->afterRender) server->afterRender();
    }
}

} // namespace

void runDirectHosted(Factory factory, SessionHostConfig config,
                     const ServerHook* server) noexcept {
    // iPadOS 26: arm supported-orientation mask *before* SDL_CreateWindow so
    // the glass measures landscape (not a transient portrait letterbox). The
    // freeze (TN3192) is applied later inside playerForceOrientation after
    // geometry settles — freezing too early blocks the rotate (see
    // tools/player_orientation_ios.mm). DirectRenderHost::send re-applies
    // after the window/scene exist.
    if (config.orientation != 0) {
        const char* hint = nullptr;
        switch (config.orientation) {
        case wire::kOrientationLandscape:        hint = "LandscapeLeft"; break;
        case wire::kOrientationLandscapeFlipped: hint = "LandscapeRight"; break;
        case wire::kOrientationPortrait:         hint = "Portrait"; break;
        case wire::kOrientationPortraitFlipped:  hint = "PortraitUpsideDown"; break;
        case wire::kOrientationAnyLandscape:     hint = "LandscapeLeft LandscapeRight"; break;
        }
        if (hint) SDL_SetHint(SDL_HINT_ORIENTATIONS, hint);
        playerForceOrientation(config.orientation);
    }

    DirectRenderHost host(config);
    applyImmersive(config.immersive);

    if (server && server->active) {
        host.setServerFrameSink(server->sink, server->active,
                                server->capturePixels);
        host.setViewerMetricsStore(server->viewer);
        if (server->armSink) host.setArmStateSink(server->armSink);
        if (server->bindDb) server->bindDb(host.context().db());
    }

    // 🎯T136 Crash diagnostics: gate the callback guards (below) on the same
    // flag, and install the fatal-signal last-gasp handlers. Done before the
    // factory so a crash in the consumer's construction is also surfaced.
    ge::setCrashDiagnosticsEnabled(config.crashDiagnostics);
    if (config.crashDiagnostics) ge::installCrashHandlers();

    // 🎯T174 Bind this session's ge::debug scope before the factory runs, so
    // HUD providers / debug primitives registered during construction land in
    // the session, not the process default.
    debug::internal::bindContext(host.context());

    RunConfig rc = factory(host.context());
    host.setEventHandler(rc.onEvent);
    host.setBackPressedHandler(rc.onBackPressed);
    host.setMemoryWarningHandler(rc.onMemoryWarning);

    wire::SessionConfig sc{};
    sc.sensors = config.sensors;
    sc.orientation = config.orientation;
    if (config.immersive) sc.flags |= wire::kSessionFlagImmersive;
    if (config.disableScreenSaver) sc.flags |= wire::kSessionFlagNoScreenSaver;
    host.send(sc);

    DirectLoop loop{host, rc, config, server};

    while (!host.shouldQuit()) {
        loop.step();
#if defined(__EMSCRIPTEN__)
        // 🎯T157 Yield to the browser between frames (Asyncify suspend —
        // see ge_web_await_frame above). The loop, its locals, and the
        // caller's pre-ge::run state survive verbatim; effectively the
        // browser's requestAnimationFrame paces this while-loop.
        //
        // Asyncify cannot suspend beneath a JS-exception invoke trampoline,
        // so on web this TU compiles -fno-exceptions (tools/prebuild.sh):
        // every call on the suspend chain — including this import — is a
        // plain wasm `call`, and guardCallback/renderBatch degrade via
        // __cpp_exceptions.
        ge_web_await_frame();
#endif
    }

    if (rc.onShutdown) rc.onShutdown();

    // 🎯T92.2.2 Stop the ServerSession (close sockets, join threads) after the
    // loop exits — runServer installed onStop for this.
    if (server && server->onStop) server->onStop();
}

// ── ge::run — dispatch by modality ────────────────────────────────

void run(Factory factory, const SessionHostConfig& config) noexcept {
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

    // 🎯T92.2.2 Server mode is a BUILD VARIANT, not a runtime switch: ge::run
    // knows at compile time which build it belongs to. A server build
    // (GE_SERVER_BUILD) is a hidden-window DirectRenderHost that streams the
    // canonical wire to a player via spyder's relay (the relay address is read
    // from the GE_SERVER env inside runServer); every other build is the
    // direct/windowed path. The app calls ge::run() identically in both — the
    // mode is the build, not the call.
#if defined(GE_SERVER_BUILD)
    runServer(factory, config);
#else
    runDirectHosted(factory, config, nullptr);
#endif
}

// ── renderBatch / renderToPng — 🎯T124 headless render ─────────────
int renderBatch(Factory factory, SessionHostConfig config,
                const std::vector<RenderItem>& items) {
    ge::log::install();
    config.headless = false;   // direct host, no stream broker / wire
    config.hidden   = true;    // unmapped window — nothing shown
    DirectRenderHost host(config);
    applyImmersive(false);
    debug::internal::bindContext(host.context());  // 🎯T174
    RunConfig rc = factory(host.context());
    host.setEventHandler(rc.onEvent);

    int ok = 0;
    for (const auto& item : items) {
        // Per-item error capture (one bad fixture doesn't sink the run) —
        // except in a no-EH TU (🎯T157 web: this file is -fno-exceptions so
        // the Asyncify suspend chain has no invoke trampolines; the render
        // verb is a desktop tool, so nothing is lost there).
#if defined(__cpp_exceptions)
        try {
#endif
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
#if defined(__cpp_exceptions)
        } catch (const std::exception& e) {
            SPDLOG_ERROR("renderBatch: '{}' threw: {}", item.outPath, e.what());
        }
#endif
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
