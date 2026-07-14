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

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <cstdint>

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

    // 1. Connect to stream relay and wait for SessionConfig (orientation hint must
    //    be applied BEFORE window creation, which happens in PlayerRender).
    ge::PlayerWireBridge wire({host, port, serverName});
    wire::SessionConfig cfg{};
    if (!wire.connect(cfg)) {
        SDL_Quit();
        return 1;
    }
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

    // 2. Create the renderer (SDL window + accelerometer).
    ge::PlayerRender::Config rc;
#ifndef GE_DESKTOP
    rc.borderless = true;
#endif
    rc.orientation = cfg.orientation;
    ge::PlayerRender render(rc);
    if (cfg.sensors & wire::kSensorAccelerometer) render.enableAccelerometer();

    // 3. Send DeviceInfo upstream.
    {
        int w, h, pr;
        render.getDeviceDimensions(w, h, pr);
        wire::DeviceInfo di{};
        di.width = w;
        di.height = h;
        di.pixelRatio = pr;
        di.deviceClass = 3;
        wire.sendDeviceInfo(di);
    }

    // 4. Main loop.
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
        for (auto& e : pump.upstreamEvents) wire.sendEvent(e);

        const uint64_t tPump0 = SDL_GetPerformanceCounter();
        if (!wire.pump()) break;
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
