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

#include "../RunDirect.h"
#include "../render/DirectRenderHost.h"
#include "ServerSession.h"

#include <ge/Protocol.h>
#include <ge/SessionHost.h>
#include <ge/Signal.h>
#include <ge/appchannel.h>
#include <ge/log.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>

namespace ge {

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

    // Per-player game instances (factory called once per attach).
    std::unordered_map<std::string, RunConfig> games;

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
                    games[id] = factory(renderHost.context());
                },
                [&](const std::string& id) {
                    auto it = games.find(id);
                    if (it == games.end()) return;
                    if (it->second.onShutdown) it->second.onShutdown();
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
                        if (it->second.onShutdown) it->second.onShutdown();
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

            // Per-session input (not global SDL queue — no cross-talk).
            for (auto& [id, rc] : games) {
                server->drainInput(id, [&](const SDL_Event& e) {
                    if (rc.onEvent) {
                        ge::guardCallback("onEvent", [&] { rc.onEvent(e); });
                    }
                });
            }

            // Update all instances.
            for (auto& [id, rc] : games) {
                if (rc.onUpdate) {
                    ge::guardCallback("onUpdate", [&] {
                        rc.onUpdate(ge::appchannel::applyTimeControl(dt));
                    });
                }
            }

            // Render + capture + encode each instance independently.
            for (auto& [id, rc] : games) {
                server->setCaptureTarget(id);
                renderHost.refreshFrame(dt);
                if (rc.onRender) {
                    ge::guardCallback("onRender", [&] {
                        rc.onRender(renderHost.context());
                    });
                }
            }
        }
    }

    for (auto& [id, rc] : games) {
        if (rc.onShutdown) rc.onShutdown();
    }
    games.clear();
    server->stop();
}

} // namespace ge
