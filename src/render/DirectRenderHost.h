// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// DirectRenderHost — RenderHost for in-process windowed rendering.
//
// Owns the SDL window, the sokol swap-chain target (via SokolContext), and
// the local input loop (SDL events + ⌥-mouse → synthetic accelerometer).
// Used by distribution / standalone builds; no ged, no wire.
#pragma once

#include <ge/RenderHost.h>
#include <ge/SokolContext.h>

#include <memory>

namespace ge {

class DirectRenderHost : public RenderHost {
public:
    explicit DirectRenderHost(const SessionHostConfig&);
    ~DirectRenderHost() override;

    DirectRenderHost(const DirectRenderHost&) = delete;
    DirectRenderHost& operator=(const DirectRenderHost&) = delete;

    int width() const override;
    int height() const override;
    DeviceClass deviceClass() const override;
    bool paused() const override;
    SafeAreaInsets drawSafeInsetsInPts() const override;
    SafeAreaInsets uiSafeInsetsInPts() const override;
    const Context& context() const override;

    void send(const wire::SessionConfig&) override;
    void setEventHandler(std::function<void(const SDL_Event&)>) override;
    void pumpEvents() override;
    void beginFrame() override;
    void endFrame(uint32_t frameNumber) override;
    bool shouldQuit() const override;

    // Wire 🎯T44 / 🎯T45 callbacks. Called once after the game's factory
    // returns its RunConfig (in runDirect). Pass the RunConfig fields
    // straight in; the host stores them and dispatches drained events
    // on the game thread inside pumpEvents.
    void setBackPressedHandler(std::function<void()>);
    void setMemoryWarningHandler(std::function<void(MemoryPressureLevel)>);

private:
    la::float2 updateParallax();

    struct Impl;
    std::unique_ptr<Impl> i_;
};

} // namespace ge
