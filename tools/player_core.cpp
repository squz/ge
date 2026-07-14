// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Thin glue: owns the main loop and wires PlayerWireBridge ↔ PlayerRender.
// All wire concerns live in PlayerWireBridge (bridge/); all SDL / rendering
// concerns live in PlayerRender (render/). This file has no knowledge of
// H.264, sockets, or SDL windowing.

#include "player_core.h"

#include <ge/FrameLog.h>
#include <ge/PlayerRender.h>
#include <ge/PlayerWireBridge.h>
#include <ge/Protocol.h>
#include <ge/Signal.h>
#include <ge/StreamHostPolicy.h>
#include <sqlpipe.h>

// Immersive is engine-internal (src/); player applies SessionConfig.immersive
// on the viewer the same way DirectRenderHost does on a direct app.
#include "../src/Immersive.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <fstream>
#include <vector>

int playerCore(const std::string& host, int port, const std::string& serverName) {
    ge::installSignalHandlers();
    SPDLOG_INFO("ge player starting (H.264 + GE2S cmdstream)");

    // No synthetic mouse/touch events — each input source stays native.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_SENSOR)) {
        SPDLOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return 1;
    }

    // ── Handshake (🎯T154) — init is seed; discovery is derived ──
    //
    //   SessionHostConfig in ge::run   fixed constants at process start
    //   server → SessionConfig         first app payload on the wire
    //   player applies that policy     orientation / immersive / sensors / …
    //   player → DeviceInfo            measured once from the configured surface
    //
    // Safe rects are not known before SessionConfig, and are not “updated”
    // after it. They are computed for the first time after policy is on glass.
    ge::PlayerWireBridge wire({host, port, serverName});
    wire::SessionConfig cfg{};
    if (!wire.connect(cfg)) {
        SDL_Quit();
        return 1;
    }
    SPDLOG_INFO("SessionConfig: sensors={:#x} orientation={} transport={} flags={:#x}",
                cfg.sensors, cfg.orientation, cfg.transport, cfg.flags);

    const bool immersive = (cfg.flags & wire::kSessionFlagImmersive) != 0;
    const bool noSaver   = (cfg.flags & wire::kSessionFlagNoScreenSaver) != 0;

    // Orientation hint before window creation (iOS/Android launch lock).
    if (cfg.orientation != 0) {
        const char* hint = nullptr;
        switch (cfg.orientation) {
        case wire::kOrientationLandscape:        hint = "LandscapeLeft"; break;
        case wire::kOrientationLandscapeFlipped: hint = "LandscapeRight"; break;
        case wire::kOrientationPortrait:         hint = "Portrait"; break;
        case wire::kOrientationPortraitFlipped:  hint = "PortraitUpsideDown"; break;
        }
        if (hint) SDL_SetHint(SDL_HINT_ORIENTATIONS, hint);
    }

    ge::PlayerRender::Config rc;
#ifndef GE_DESKTOP
    rc.borderless = true;
#endif
    rc.orientation = cfg.orientation;
    rc.immersive = immersive;  // discovery: ui-safe after bars are applied
    ge::PlayerRender render(rc);

    // Apply the rest of SessionConfig now that a window/activity exists.
    // Blocking applyImmersive waits for the OS layout/insets pass so the
    // subsequent DeviceInfo read sees the configured surface.
    if (immersive) ge::applyImmersive(true);
    if (noSaver) SDL_DisableScreenSaver();
    if (cfg.sensors & wire::kSensorAccelerometer) render.enableAccelerometer();

    {
        wire::DeviceInfo di{};
        render.fillDeviceInfo(di);
        wire.sendDeviceInfo(di);
        SPDLOG_INFO("DeviceInfo {}x{} @{}x class={} safe=({},{} {}x{}) "
                    "draw=({},{} {}x{})",
                    di.width, di.height, di.pixelRatio, di.deviceClass,
                    di.safeX, di.safeY, di.safeW, di.safeH,
                    di.drawSafeX, di.drawSafeY, di.drawSafeW, di.drawSafeH);
    }

    // 🎯T154 GE2T: player-authoritative durable db. Open the glass-side store
    // and seed the server's :memory: working set so reconnect restores state.
    std::string playerDbPath = ":memory:";
    if (char* pref = SDL_GetPrefPath("squz", "ge-player")) {
        playerDbPath = ge::durableDbPathForPlayer("squz", "ge-player", pref);
        SDL_free(pref);
    }
    sqlpipe::Database playerDb(playerDbPath);
    {
        std::vector<uint8_t> dump;
        if (ge::dumpSqliteMain(playerDb.handle(), dump) && !dump.empty()) {
            wire.sendGe2tSnapshot(dump);
            SPDLOG_INFO("GE2T: seeded server from player durable db {} ({} bytes)",
                        playerDbPath, dump.size());
        } else {
            SPDLOG_INFO("GE2T: player durable db empty or dump failed ({})",
                        playerDbPath);
        }
    }

    // Mid-session orientation/resize only — re-measure the live surface.
    auto sendViewerDiscovery = [&] {
        wire::DeviceInfo di{};
        render.fillDeviceInfo(di);
        wire.sendDeviceInfo(di);
    };
    struct PlayerFrame { uint64_t timestamp; int decoded; uint32_t lastSeq; float drainMs; float renderMs; float pumpMs; float evMs; float upMs; };
    static FrameLog<PlayerFrame> playerLog(
        [](const std::vector<PlayerFrame>& frames, uint64_t freq) {
            int total = 0, empty = 0, gaps = 0;
            uint32_t prev = 0, minSeq = UINT32_MAX, maxSeq = 0;
            float maxDrain = 0, maxRender = 0, maxGap = 0, maxPump = 0, sumPump = 0;
            float maxEv = 0, sumEv = 0, maxUp = 0, sumUp = 0;
            for (size_t i = 0; i < frames.size(); i++) {
                auto& f = frames[i];
                total += f.decoded;
                if (f.decoded == 0) empty++;
                maxDrain  = std::max(maxDrain,  f.drainMs);
                maxRender = std::max(maxRender, f.renderMs);
                maxPump   = std::max(maxPump,   f.pumpMs);
                sumPump  += f.pumpMs;
                maxEv     = std::max(maxEv,     f.evMs);
                sumEv    += f.evMs;
                maxUp     = std::max(maxUp,     f.upMs);
                sumUp    += f.upMs;
                if (i > 0) {
                    float g = float(frames[i].timestamp - frames[i-1].timestamp)
                            * 1000.f / float(freq);
                    maxGap = std::max(maxGap, g);
                }
                if (f.decoded > 0) {
                    if (prev && f.lastSeq > prev + 1) gaps += f.lastSeq - prev - 1;
                    prev = f.lastSeq;
                    minSeq = std::min(minSeq, f.lastSeq);
                    maxSeq = std::max(maxSeq, f.lastSeq);
                }
            }
            SPDLOG_INFO("PlayerLog: {} ticks, {} decoded ({} empty), seq {}-{} ({} gaps), "
                        "maxDrain={:.1f}ms maxRender={:.1f}ms maxGap={:.1f}ms "
                        "pump avg={:.1f}/max={:.1f}ms ev avg={:.1f}/max={:.1f}ms "
                        "upload avg={:.1f}/max={:.1f}ms",
                        frames.size(), total, empty, minSeq, maxSeq, gaps,
                        maxDrain, maxRender, maxGap,
                        frames.empty() ? 0.f : sumPump / frames.size(), maxPump,
                        frames.empty() ? 0.f : sumEv / frames.size(), maxEv,
                        frames.empty() ? 0.f : sumUp / frames.size(), maxUp);
        });

    uint64_t frameCount = 0;
    ge::PlayerWireBridge::DecodedFrame decodedFrame;
    ge::PlayerWireBridge::CmdDisplayFrame cmdFrame;
    // Present-rate meter for the ≥55 fps gate (cmdstream + video).
    uint64_t fpsWindowStart = SDL_GetPerformanceCounter();
    uint64_t fpsWindowFrames = 0;
    const uint64_t freq = SDL_GetPerformanceFrequency();

    while (!ge::shouldQuit()) {
        const uint64_t tEv0 = SDL_GetPerformanceCounter();
        auto pump = render.pumpEvents();
        if (pump.quit) break;
        if (pump.surfaceChanged) sendViewerDiscovery();
        if (pump.lifecycleKind != 0) {
            wire::ViewerLifecycle life{};
            life.kind = pump.lifecycleKind;
            life.memoryLevel = pump.lifecycleMemoryLevel;
            wire.sendLifecycle(life);
        }
        for (auto& e : pump.upstreamEvents) wire.sendEvent(e);

        const uint64_t tPump0 = SDL_GetPerformanceCounter();
        if (!wire.pump()) break;
        // 🎯T154 GE2T: server→player durable push (detach / refresh).
        {
            std::vector<uint8_t> push;
            if (wire.pollGe2tSnapshot(push) && !push.empty() &&
                playerDbPath != ":memory:") {
                std::ofstream out(playerDbPath, std::ios::binary | std::ios::trunc);
                if (out) {
                    out.write(reinterpret_cast<const char*>(push.data()),
                              static_cast<std::streamsize>(push.size()));
                    SPDLOG_INFO("GE2T: wrote durable snapshot {} ({} bytes)",
                                playerDbPath, push.size());
                }
            }
        }
        const uint64_t tPump1 = SDL_GetPerformanceCounter();

        bool got = false;
        if (wire.pollCmdFrame(cmdFrame)) {
            render.beginCmdFrame(cmdFrame.contentW, cmdFrame.contentH);
            for (const auto& img : cmdFrame.images) {
                ge::PlayerRender::CmdImageUpload u;
                u.id = img.id;
                u.w = img.w;
                u.h = img.h;
                u.rgba = img.rgba.data();
                u.rgbaBytes = img.rgba.size();
                render.uploadCmdImage(u);
            }
            for (const auto& run : cmdFrame.runs) {
                ge::PlayerRender::CmdSpriteRunDraw d;
                d.imageId = run.imageId;
                d.nVerts = run.nVerts;
                d.verts = run.verts.data();
                d.mvp = run.mvp;
                render.drawCmdSpriteRun(d);
            }
            render.endCmdFrame();
            frameCount++;
            got = true;
            fpsWindowFrames++;
        } else if (wire.pollFrame(decodedFrame)) {
            render.updateVideoTexture(decodedFrame.view());
            frameCount++;
            got = true;
            fpsWindowFrames++;
        }
        (void)got;
        const uint64_t tUp1 = SDL_GetPerformanceCounter();

        auto rs = render.render();
        auto stats = wire.lastPumpStats();
        const float tickHz = float(freq);
        playerLog.record({SDL_GetPerformanceCounter(),
                          stats.framesThisTick, stats.lastSeq,
                          rs.drainMs, rs.renderMs,
                          float(tPump1 - tPump0) * 1000.f / tickHz,
                          float(tPump0 - tEv0) * 1000.f / tickHz,
                          float(tUp1 - tPump1) * 1000.f / tickHz});

        // Log measured present rate every ~1 s (cmdstream / video).
        const uint64_t now = SDL_GetPerformanceCounter();
        const double winSec = double(now - fpsWindowStart) / double(freq);
        if (winSec >= 1.0) {
            const double fps = double(fpsWindowFrames) / winSec;
            SPDLOG_INFO("PlayerFPS: {:.1f} fps over {:.2f}s ({} frames) "
                        "cmdstream={} last_wire={}B last_seq={}",
                        fps, winSec, fpsWindowFrames,
                        stats.cmdstream ? 1 : 0,
                        stats.lastWireBytes, stats.lastSeq);
            fpsWindowStart = now;
            fpsWindowFrames = 0;
        }
    }

    wire.close();
    SDL_Quit();
    SPDLOG_INFO("Player exited ({} frames decoded)", frameCount);
    return 0;
}
