// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// runServer — multi-session server modality of ge::run() (restored).
//
// Each player_attached creates an independent game instance:
//   factory(ctx) → RunConfig, per-session wire + H.264 encode (ServerSession).
// No broadcast: desktop + Pixel each get their own sim/render/stream.
//
// One DirectRenderHost (GPU/window); sessions render sequentially each frame
// (same constraint as the historical bgfx multi-session path: one context,
// N sessions → shared frame budget). Capture is routed via
// ServerSession::setCaptureTarget before each session's onRender.
//
// Input: wire events do NOT go through DirectRenderHost::pumpEvents (that
// only drains local SDL). Per-session AccelSynth converts raw Shift+drag
// from desktop players into SDL_EVENT_SENSOR_UPDATE before onEvent —
// same engine-side contract as single-session direct mode.

#include "../RunDirect.h"
#include "../render/AccelSynth.h"
#include "../render/DirectRenderHost.h"
#include "ServerSession.h"

#include <ge/Linalg.h>
#include <ge/Protocol.h>
#include <ge/SessionHost.h>
#include <ge/Signal.h>
#include <ge/appchannel.h>
#include <ge/log.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace ge {

namespace {

// Per-player game + its own AccelSynth (Shift+drag state is not shared).
struct SessionGame {
    RunConfig rc;
    std::optional<AccelSynth> synth;
};

// Match DirectRenderHost's presentation-tilt deadzone / axis convention.
la::float2 presentationTiltFromSynth(const AccelSynth& synth) {
    const Tilt t = synth.current();
    if (std::sqrt(t.x * t.x + t.y * t.y) <= 0.7f) return {0.f, 0.f};
    // Y negated: SDL mouse-y grows downward; presentation tilt is y-up.
    return la::float2{t.x, -t.y} * kTiltRadPerPixel;
}

} // namespace

void runServer(Factory factory, const SessionHostConfig& config) {
    const std::string name =
        config.appName && *config.appName ? config.appName : "server";

    std::string host = "127.0.0.1";
    int port = 3030;
    if (const char* s = std::getenv("GE_SERVER")) {
        const std::string addr = s;
        const auto colon = addr.rfind(':');
        if (colon != std::string::npos) {
            host = addr.substr(0, colon);
            port = std::atoi(addr.c_str() + colon + 1);
        } else if (!addr.empty()) {
            host = addr;
        }
    }

    wire::SessionConfig sc{};
    sc.sensors = config.sensors;
    sc.orientation = config.orientation;

    auto server = std::make_shared<ServerSession>(host, port, name, sc);
    server->start();
    SPDLOG_INFO(
        "runServer: '{}' multi-session streaming to relay {}:{} "
        "(independent game per player)",
        name, host, port);

    SessionHostConfig cfg = config;
    cfg.hidden = true;

    DirectRenderHost renderHost(cfg);
    renderHost.setServerFrameSink(
        [server](const std::uint8_t* px, int w, int h) {
            server->onCapturedFrame(px, w, h);
        },
        server->activeFlag());

    ge::setCrashDiagnosticsEnabled(config.crashDiagnostics);
    if (config.crashDiagnostics) ge::installCrashHandlers();

    const bool wantAccelSynth =
        (config.sensors & wire::kSensorAccelerometer) != 0;

    // Per-player game instances (factory called once per attach).
    std::unordered_map<std::string, SessionGame> games;

    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();
    constexpr uint64_t kServerFps = 60;
    const uint64_t paceStart = last;
    uint64_t paceIndex = 0;

    while (!renderHost.shouldQuit()) {
        // Pace to ~60 fps (hidden window has no vsync).
        {
            const uint64_t target = paceStart + paceIndex * freq / kServerFps;
            uint64_t now = SDL_GetPerformanceCounter();
            if (now < target) {
                const int64_t sleepTicks =
                    int64_t(target - now) - int64_t(freq / 1000);
                if (sleepTicks > 0)
                    SDL_Delay(uint32_t(sleepTicks * 1000 / freq));
                while (SDL_GetPerformanceCounter() < target) {
                }
            }
            ++paceIndex;
        }

#if defined(__OBJC__)
        @autoreleasepool
#endif
        {
            renderHost.pumpEvents();
            ge::appchannel::pumpMainThreadTasks();

            // Lifecycle: factory per attach (independent game state).
            server->pollLifecycle(
                [&](const std::string& id) {
                    if (games.count(id)) return;
                    SPDLOG_INFO("runServer: creating game instance for session {}",
                                id);
                    SessionGame g;
                    g.rc = factory(renderHost.context());
                    if (wantAccelSynth) {
                        // Engine-side synth: players forward raw Shift+drag;
                        // no setWindow — relative mouse mode is the player's job.
                        g.synth.emplace();
                        auto onEvent = g.rc.onEvent;
                        g.synth->setEmit([onEvent](const SDL_Event& e) {
                            if (onEvent) {
                                ge::guardCallback("onEvent",
                                                  [&] { onEvent(e); });
                            }
                        });
                        SPDLOG_INFO(
                            "runServer: AccelSynth enabled for session {}", id);
                    }
                    games.emplace(id, std::move(g));
                },
                [&](const std::string& id) {
                    auto it = games.find(id);
                    if (it == games.end()) return;
                    if (it->second.rc.onShutdown) it->second.rc.onShutdown();
                    games.erase(it);
                    SPDLOG_INFO("runServer: destroyed game instance {}", id);
                });

            // Drop game state if wire vanished without detach msg.
            {
                const auto live = server->sessionIds();
                std::unordered_map<std::string, bool> liveSet;
                for (const auto& id : live) liveSet[id] = true;
                for (auto it = games.begin(); it != games.end();) {
                    if (!liveSet.count(it->first)) {
                        if (it->second.rc.onShutdown)
                            it->second.rc.onShutdown();
                        it = games.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            uint64_t now = SDL_GetPerformanceCounter();
            float dt = float(now - last) / float(freq);
            last = now;
            if (dt > 0.1f) dt = 0.1f;

            if (renderHost.paused()) {
                last = SDL_GetPerformanceCounter();
                continue;
            }

            if (games.empty()) {
                last = SDL_GetPerformanceCounter();
                continue;
            }

            ge::appchannel::perfTick(dt * 1000.0f);

            // Per-session input through AccelSynth, then onEvent.
            // Wire events never enter DirectRenderHost::pumpEvents, so this
            // is the only path that restores Shift+drag → SENSOR_UPDATE.
            for (auto& [id, g] : games) {
                server->drainInput(id, [&](const SDL_Event& e) {
                    if (g.synth && g.synth->handle(e)) return;
                    if (g.rc.onEvent) {
                        ge::guardCallback("onEvent",
                                          [&] { g.rc.onEvent(e); });
                    }
                });
                // Ease-back after Shift release (emits SENSOR_UPDATE via setEmit).
                if (g.synth) g.synth->update();
            }

            // Update all instances.
            for (auto& [id, g] : games) {
                if (g.rc.onUpdate) {
                    ge::guardCallback("onUpdate", [&] {
                        g.rc.onUpdate(ge::appchannel::applyTimeControl(dt));
                    });
                }
            }

            // Render + capture + encode each instance independently.
            for (auto& [id, g] : games) {
                server->setCaptureTarget(id);
                renderHost.refreshFrame(dt);
                // Host synth is local-SDL only and unused for wire players;
                // override presentation tilt from this session's AccelSynth.
                if (g.synth) {
                    const_cast<Context&>(renderHost.context())
                        .setPresentationTilt(presentationTiltFromSynth(*g.synth));
                }
                if (g.rc.onRender) {
                    ge::guardCallback("onRender", [&] {
                        g.rc.onRender(renderHost.context());
                    });
                }
            }
        }
    }

    for (auto& [id, g] : games) {
        if (g.rc.onShutdown) g.rc.onShutdown();
    }
    games.clear();
    server->stop();
}

} // namespace ge
