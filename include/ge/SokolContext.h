// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38 spike: sokol_gfx replacement for BgfxContext.
// macOS Metal only. iOS / Android come later if the spike continues.
//
// Owns the SDL window, the CAMetalLayer, the MTLDevice, and sg_setup.
// Per frame: caller acquires a drawable via beginFrame(), submits draws
// between beginFrame() and endFrame(), and endFrame() commits + presents.
#pragma once

#include <ge/Linalg.h>

#include <cstdint>
#include <functional>
#include <memory>

struct SDL_Window;

namespace ge {

struct SokolConfig {
    int         width  = 820;
    int         height = 1180;
    const char* title  = nullptr;
};

class SokolContext {
public:
    explicit SokolContext(const SokolConfig& config);
    ~SokolContext();

    int         width() const;
    int         height() const;
    bool        shouldQuit() const;
    SDL_Window* window() const;

    // Per-frame swap-chain pass setup. beginFrame() acquires the next
    // Metal drawable and opens a sokol pass against it; the caller's
    // draws go between begin/end; endFrame() submits + presents.
    //
    // clearColor is RGBA in [0,1]. Pass nullptr for the default
    // (opaque black). To use don't-care / load instead of clear, call
    // beginFramePassAction() with an explicit pass-action setup.
    void beginFrame(const float clearColor[4] = nullptr);
    void endFrame();

    // 🎯T92.6 One-shot framebuffer readback for the app-channel screenshot
    // path. Arms a capture of the frame about to be presented; the next
    // endFrame() — after the GPU has rendered it — invokes `sink` with the
    // pixels as RGBA8, top-down, tightly packed (w*h*4 bytes), then disarms.
    // Both this call and the sink run on the game thread. Capturing the
    // swapchain requires a readable framebuffer, so this only does real work
    // in dev builds (the layer is framebufferOnly in release).
    using FrameCaptureSink = std::function<void(const std::uint8_t* rgba, int w, int h)>;
    void captureNextFrame(FrameCaptureSink sink);

    // App-lifecycle hooks. On Apple these are no-ops — the CAMetalLayer
    // survives backgrounding and rendering resumes without a swap-chain
    // rebuild. On Android the EGL surface is destroyed on background
    // and recreated on foreground; the implementation pauses frame
    // submission between the two events and re-binds the GL context on
    // resume. Wire from SDL_EVENT_DID_ENTER_{BACKGROUND,FOREGROUND}.
    void onBackground();
    void onForeground();

    // 🎯T107: opaque pImpl, defined per platform in the matching .cpp/.mm. Public
    // only so the Android backend implementations (GlesM / VkM) can derive from
    // it as an abstract base; it stays an incomplete type to consumers.
    struct M;

private:
    std::unique_ptr<M> m;
};

} // namespace ge
