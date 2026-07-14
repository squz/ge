// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ServerSession — the server side of the canonical brokered wire, as a
// sink/pump the direct run loop drives (🎯T92.2.2). It is NOT a RenderHost:
// server mode is a hidden-window variant of DirectRenderHost that streams ge's
// canonical H.264 wire, so the render host stays DirectRenderHost and this
// object only owns the relay sockets, the encoder, and the input pump.
//
// Lifecycle: constructed from the relay's host/port/name + the game's
// wire::SessionConfig, start() spins the sideband thread that waits for a
// player to attach (/ws/server?name=), openWire() dials the per-session wire
// (/ws/server/wire/<id>), and while a player is attached active() is true so
// DirectRenderHost arms per-frame capture. Each captured frame is handed to
// onCapturedFrame() on the game thread, H.264-encoded, and sent as a
// kVideoStreamMagic packet. Relayed SDL events arrive on the input thread and
// are pushed into the SDL event queue (DirectRenderHost's pumpEvents dispatches
// them). Single player. LAN/dev only.
#pragma once

#include <ge/Protocol.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ge {

class ServerSession {
public:
    // host:port is the relay (spyder's HTTP addr); name identifies the server
    // in the relay catalogue (/stream/servers). cfg is the game-wide session
    // requirements (sensors, orientation) sent to the player on attach.
    ServerSession(std::string host, int port, std::string name,
                  const wire::SessionConfig& cfg);
    ~ServerSession();

    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    // Spawn the relay connection thread (returns immediately).
    void start();
    // Tear down: stop capture, close sockets, join threads.
    void stop();

    // True while a player is attached — DirectRenderHost gates its per-frame
    // capture arming on this (via activeFlag()).
    bool active() const;

    // The atomic DirectRenderHost polls each frame to decide whether to arm
    // capture. Lives as long as this ServerSession.
    std::atomic<bool>* activeFlag();

    // Game thread: a just-presented frame's pixels (as delivered by
    // SokolContext::captureNextFrame). Encodes + sends when a player is
    // attached; a no-op otherwise. The encode dimensions are the CAPTURED
    // frame's actual w×h — NOT any DeviceInfo the player advertised — so the
    // player receives frames at the server's own render resolution and
    // letterboxes/scales as it sees fit. No-op under transport=cmdstream
    // (sprite LiveCapture path instead).
    void onCapturedFrame(const std::uint8_t* px, int w, int h);

    // 🎯T128 command-stream: arm/flush LiveCapture around onRender.
    // contentW/H = server swapchain pixels (for player aspect-fit letterbox).
    void onFrameBegin(int contentW, int contentH);
    void onFrameEnd();

    // When false, DirectRenderHost skips GPU framebuffer readback.
    std::atomic<bool>* capturePixelsFlag();

private:
    struct Impl;
    std::unique_ptr<Impl> i_;
};

} // namespace ge
