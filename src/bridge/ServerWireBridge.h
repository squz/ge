// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ServerWireBridge — RenderHost for the brokered (player) modality,
// server side.
//
// Owns a per-session WebSocket wire to a single attached player, the
// headless render target that captures the engine's draw output, and the
// H.264 encoder that streams frames over the wire. Receives SDL events from
// the wire and dispatches them to the engine.
//
// The sideband orchestration (listening for player_attached/detached, lazy
// global render init, per-session lifecycle) stays in SessionHost.mm — that's
// a multi-session concern outside the per-session contract.
//
// 🎯T34 (sokol port): this whole brokered/wire path is dormant and not built
// (GE_SRC_BROKERED is excluded). The render-target + GPU→CPU readback that fed
// the encoder was bgfx; it is stubbed here (the wire / encode / Context / event
// logic is intact and bgfx-free). The readback the encoder needs is exactly the
// headless sokol render+readback already built for 🎯T124
// (SokolContext::captureFrameRGBASync) — wiring that in makes this functional.
#pragma once

#include <ge/RenderHost.h>
#include <ge/VideoEncoder.h>
#include <ge/WebSocketClient.h>

#include <memory>
#include <string>

namespace ge {

class ServerWireBridge : public RenderHost {
public:
    // Constructor takes the bits needed to build a session Context once
    // dimensions arrive: app identity for the persistent DB path and
    // optional schema DDL. The bridge owns Context construction so the
    // run loop stays out of host-specific db setup.
    ServerWireBridge(std::string sessionId,
                     std::shared_ptr<WsConnection> wire,
                     const SessionHostConfig& config);
    ~ServerWireBridge() override;

    ServerWireBridge(const ServerWireBridge&) = delete;
    ServerWireBridge& operator=(const ServerWireBridge&) = delete;

    // Drain wire messages. On DeviceInfo arrival, dimensions become valid
    // (caller should then call initialize()). On later SDL events, invokes
    // the registered handler. Returns false if the wire is closed.
    bool pumpWire();

    // True after DeviceInfo arrives — width()/height() are valid; capture
    // can be initialized. False before that.
    bool hasDimensions() const;

    // Initialize the capture render target + encoder. Caller must have inited
    // the render system first. After this call, isReady() returns true.
    void initialize();

    // True after initialize() — ready to render.
    bool isReady() const;

    const std::string& id() const;

    // RenderHost
    int width() const override;
    int height() const override;
    DeviceClass deviceClass() const override;

    void send(const wire::SessionConfig&) override;
    void setEventHandler(std::function<void(const SDL_Event&)>) override;
    void pumpEvents() override;  // alias for pumpWire
    void beginFrame() override;
    void endFrame(uint32_t frameNumber) override;
    bool shouldQuit() const override;
    const Context& context() const override;

    // Final cleanup: destroy the render target, flush encoder.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> i_;
};

} // namespace ge
