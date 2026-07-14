// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// runServer — the server modality of ge::run() (🎯T92.2.2).
//
// Server mode is a hidden-window variant of the direct run loop that streams
// ge's canonical H.264 wire to a player attached via spyder's relay. There is
// ONE render host (DirectRenderHost) and ONE wire (ServerSession) — no parallel
// StreamClient hack, no dormant ServerWireBridge. This TU builds the
// ServerSession, forces a hidden window, wires the session's frame sink + active
// flag into the shared direct loop via ServerHook, and hands off to
// runDirectHosted (SessionHost.mm).
//
// Kept in a separate TU from SessionHost.mm so mobile distribution builds
// (GE_DIRECT_ONLY) can omit it and avoid pulling in the bridge subsystem
// (ServerSession, WebSocketClient, VideoEncoder). Under GE_DIRECT_ONLY,
// SessionHost.mm stubs runServer with a runtime error instead.

#include "../RunDirect.h"
#include "ServerSession.h"

#include <ge/Protocol.h>
#include <ge/SessionHost.h>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace ge {

void runServer(Factory factory, const SessionHostConfig& config) {
    const std::string name =
        config.appName && *config.appName ? config.appName : "server";

    // The relay address is a RUNTIME parameter of a server build (an address,
    // not a mode): GE_SERVER=<host:port>, set by whatever spawns the instance
    // (spyder's factory). Defaults to the daemon's conventional local address.
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

    // The wire::SessionConfig the player receives on attach — game-wide sensor /
    // orientation requirements, mirroring runDirect's send() to a local host.
    wire::SessionConfig sc{};
    sc.sensors = config.sensors;
    sc.orientation = config.orientation;

    auto server = std::make_shared<ServerSession>(host, port, name, sc);
    server->start();
    SPDLOG_INFO("runServer: '{}' streaming to relay {}:{}", name, host, port);

    // Force a hidden window — a server-mode instance has no visible surface (the
    // player displays the stream). Render dims stay config.width/height.
    SessionHostConfig cfg = config;
    cfg.hidden = true;

    ServerHook hook;
    hook.active = server->activeFlag();
    hook.capturePixels = server->capturePixelsFlag();
    hook.sink = [server](const std::uint8_t* px, int w, int h) {
        server->onCapturedFrame(px, w, h);
    };
    hook.beforeRender = [server](int w, int h) { server->onFrameBegin(w, h); };
    hook.afterRender = [server] { server->onFrameEnd(); };
    hook.onStop = [server] { server->stop(); };

    runDirectHosted(std::move(factory), std::move(cfg), &hook);
}

} // namespace ge
