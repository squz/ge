// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ge/SessionHost.h>

#include <functional>
#include <memory>

namespace tiltbuggy {

class Scene;

// 🎯T137 Renders the TiltBuggy scene from the original textures: a tiled
// asphalt arena with rectangular ice/dirt patches and the buggy sprite on top.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Called once after the render backend is initialized. `shaderDir` is
    // unused (the renderer draws via ge::Sprite/SpriteBatch, not a bespoke
    // pipeline) but kept for call-site compatibility.
    void init(const char* shaderDir);

    // Clear + draw the scene for this frame, inside the swapchain pass.
    void drawFrame(const Scene& scene, const ge::Context& c,
                   float tiltX = 0.f, float tiltY = 0.f);

    // Title / heading banner in full-surface point space (y-down), same
    // layout as drawFrame. Empty if chrome is unavailable. Used as the
    // T109 hit-target test case (tap heading → reset buggy).
    static ge::Rect titleBannerRectInPts(const ge::Context& c);

    // 🎯T170 Optional overlay drawn last, inside the frame's pass — used
    // for gesture-hint hands / tag ripples. The pass is still open when
    // it runs, so overlay code just issues draws.
    std::function<void(const ge::Context&)> overlay;

private:
    struct Impl;
    std::unique_ptr<Impl> i_;
};

} // namespace tiltbuggy
