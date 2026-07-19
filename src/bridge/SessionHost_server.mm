// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// runServer — the SERVER BUILD entry for ge::run() (🎯T92.2.2 / 🎯T154.1).
//
// Desktop game and stream server are **totally different builds**:
//   make              → bin/<app>         (windowed DirectRenderHost)
//   make GE_SERVER=1  → bin/<app>-server  (-DGE_SERVER_BUILD, this TU linked)
//
// The server is a **console stream host** (stdio + relay), not a Mac game app.
// Product story (🎯T154.1): distinct `make server` binary, no Dock/foreground
// GUI identity (Accessory policy). Implementation still uses a hidden SDL
// window as the Metal offscreen drawable for encode/cmdstream — that is host
// machinery, not a user-facing window.
//
// Kept in a separate TU so mobile distribution (GE_DIRECT_ONLY) omits wire.

#include "../RunDirect.h"
#include "ServerSession.h"

#include <ge/Protocol.h>
#include <ge/SessionHost.h>
#include <sqlpipe.h>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif
#endif

namespace ge {

namespace {

// Console process identity on macOS: no Dock icon, do not steal focus.
// Call after SDL has brought up NSApp (first video init). Safe to call early
// if NSApp already exists; no-op if not yet.
void adoptConsoleProcessIdentity() {
#if defined(__APPLE__) && TARGET_OS_OSX
    @autoreleasepool {
        if (NSApp == nil) {
            // Ensure AppKit is up enough to set policy (SDL may not have yet).
            [NSApplication sharedApplication];
        }
        // Accessory: no Dock tile; not a normal foreground app.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        // Do not activate / order front anything.
    }
#endif
}

} // namespace

void runServer(Factory factory, const SessionHostConfig& config) {
    adoptConsoleProcessIdentity();
    std::string name =
        config.appName && *config.appName ? config.appName : "server";
    // Optional catalogue override for multi-server proof / factory renames
    // (🎯T100.5): GE_SERVER_NAME=multimaze2 bin/tiltbuggy-server.
    if (const char* n = std::getenv("GE_SERVER_NAME"); n && *n) {
        name = n;
    }

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

    // The wire::SessionConfig the player receives on attach — same session
    // policy the direct host would apply locally (sensors, orientation,
    // immersive chrome, screen-saver). 🎯T154: viewer glass applies these.
    wire::SessionConfig sc{};
    sc.sensors = config.sensors;
    sc.orientation = config.orientation;
    if (config.immersive) sc.flags |= wire::kSessionFlagImmersive;
    if (config.disableScreenSaver) sc.flags |= wire::kSessionFlagNoScreenSaver;

    auto server = std::make_shared<ServerSession>(host, port, name, sc);
    server->start();
    SPDLOG_INFO("runServer: '{}' streaming to relay {}:{}", name, host, port);

    // Offscreen drawable only — player is the glass. Not a user-facing window.
    SessionHostConfig cfg = config;
    cfg.hidden = true;

    ServerHook hook;
    hook.active = server->activeFlag();
    hook.capturePixels = server->capturePixelsFlag();
    hook.viewer = server->viewerMetrics();
    hook.sink = [server](const std::uint8_t* px, int w, int h) {
        server->onCapturedFrame(px, w, h);
    };
    hook.beforeRender = [server](int w, int h) { server->onFrameBegin(w, h); };
    hook.afterRender = [server] {
        server->onFrameEnd();
        // 🎯T154 SP2T: continuous fire-and-forget durable push (~0.5s).
        server->maybePushSp2t();
    };
    hook.onStop = [server] { server->stop(); };
    // 🎯T158: AccelSynth arm transitions → SP2A to the primary glass.
    hook.armSink = [server](bool armed) { server->sendArmState(armed); };
    // Capture schema so setWorkingDb can retain it for post-seed re-apply.
    const std::string schema = config.schemaDdl;
    hook.bindDb = [server, schema](std::shared_ptr<sqlpipe::Database> db) {
        server->setWorkingDb(std::move(db), schema);
    };

    SPDLOG_INFO("runServer: console stream host (not a desktop game window)");
    // runDirectHosted constructs DirectRenderHost (SDL_Init / NSApp); console
    // policy is re-applied at the end of host construction via ServerHook path.
    runDirectHosted(std::move(factory), std::move(cfg), &hook);
}

} // namespace ge
