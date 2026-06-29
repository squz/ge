// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ge/SessionHost.h>

#include <memory>

namespace tiltbuggy {

class Scene;  // from Scene.h — written by a parallel agent

// Point-space rect of the "BUY PRO" button. Returned in pt matching
// ge::input::fromSdl's output (🎯T60), so main.cpp's onEvent hit-test
// and Renderer's draw position share one source of truth.
ge::Rect proButtonRect(const ge::Context& c);

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Called once after the render backend is initialized. `shaderDir` is the directory
    // containing compiled `.bin` files (e.g. "build/shaders").
    void init(const char* shaderDir);

    // Clear + draw the scene. Call per frame in the swapchain Pass
    // covering the full surface. The playfield is placed at
    // `c.drawSafeRectInPts()` (display-cutout-safe — visuals here aren't
    // physically obscured) and the renderer is free to draw anywhere
    // on `c.fullRectInPts()` for effects that bleed past it.
    // `tiltX` / `tiltY` are normalized (~[-1, +1]) device tilt; the
    // host's composite pass applies viewport tilt when synthesized
    // tilt is non-zero. Pass (0, 0) for a flat top-down view.
    void drawFrame(const Scene& scene, const ge::Context& c,
                   float tiltX = 0.f, float tiltY = 0.f);

    // 🎯T124 The 🎯T89 render-liveness spin advances the buggy a hair each frame
    // — great for spotting a stalled loop, fatal for deterministic snapshots.
    // Turn it off for headless render-to-PNG so the same state yields the same
    // pixels. On by default.
    void setDiagnosticSpin(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> i_;
};

} // namespace tiltbuggy
