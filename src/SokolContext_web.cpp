// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T157: sokol_gfx backend glue for the web build (Emscripten/wasm).
//
// WebGL2 via SOKOL_GLES3 with SOKOL_IMPL inline — the Apple single-backend
// model, not Android's dispatch-shim/.so split (🎯T107): the browser has
// exactly one backend, so there is nothing to select at runtime. A future
// WebGPU backend (🎯T157.1) would revisit this the T107 way.
//
// This TU is deliberately self-contained (window/canvas + GL context +
// swapchain pass + readback) so a future SP2S command-stream replayer (browser
// player, post-🎯T128.2) can sit on it without the DirectRenderHost loop.
//
// Lifecycle: onBackground/onForeground are no-ops like Apple's — the WebGL
// context survives a hidden tab, and requestAnimationFrame stops firing while
// hidden, so the loop pauses without any surface teardown. (WebGL context-loss
// recovery is a follow-on; sokol has no context-restore path today.)

#if defined(__EMSCRIPTEN__)

#include <ge/SokolContext.h>
#include <ge/Signal.h>

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <GLES3/gl3.h>  // 🎯T92.6 glReadPixels for screenshot readback

#include <cstring>
#include <vector>

#define SOKOL_GLES3
#define SOKOL_IMPL
#include "sokol_gfx.h"
#include "sokol_log.h"

namespace ge {

namespace {

// sokol-gfx callbacks bridged to spdlog (T66 — stderr → browser console).
void sokolLog(const char* tag, uint32_t log_level, uint32_t log_item,
              const char* message, uint32_t line_nr, const char* filename,
              void* /*user*/) {
    SPDLOG_LOGGER_CALL(spdlog::default_logger().get(),
        log_level <= 1 ? spdlog::level::err : spdlog::level::warn,
        "sokol[{}] {}({},{}): {}",
        tag, filename ? filename : "?", line_nr, log_item,
        message ? message : "");
}

} // namespace

struct SokolContext::M {
    int           width  = 0;
    int           height = 0;
    SDL_Window*   window = nullptr;
    SDL_GLContext gl     = nullptr;
    SokolContext::FrameCaptureSink captureSink;  // 🎯T92.6 one-shot
};

SokolContext::SokolContext(const SokolConfig& config) : m(new M) {
    ge::installSignalHandlers();  // no-op on web; kept for cross-platform symmetry

    // ge paces frames itself (RAF-aligned Asyncify await in SessionHost.mm).
    // Without this hint SDL's SwapWindow calls emscripten_sleep(0) on every
    // swap — a second, redundant suspend that fires beneath the frame's C++
    // try scopes (invoke trampolines), where Asyncify aborts ("import
    // invoke_* changed the state").
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SPDLOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return;
    }

    m->width  = config.width;
    m->height = config.height;

    // WebGL2 == GLES 3.0 profile. Attributes must precede SDL_CreateWindow.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);  // 🎯T133: swapchain depth buffer
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    const char* title = (config.title && *config.title) ? config.title : "ge";
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.hidden) flags |= SDL_WINDOW_HIDDEN;  // 🎯T124 headless render
    m->window = SDL_CreateWindow(title, config.width, config.height, flags);
    if (!m->window) {
        SPDLOG_ERROR("SDL_CreateWindow(WebGL2) failed: {}", SDL_GetError());
        return;
    }

    m->gl = SDL_GL_CreateContext(m->window);
    if (!m->gl) {
        SPDLOG_ERROR("SDL_GL_CreateContext failed: {}", SDL_GetError());
        return;
    }
    if (!SDL_GL_MakeCurrent(m->window, m->gl)) {
        SPDLOG_ERROR("SDL_GL_MakeCurrent failed: {}", SDL_GetError());
        return;
    }
    // Swap interval 0: ge's run loop is already RAF-paced (Asyncify await in
    // SessionHost.mm), and a non-zero interval makes SDL's SwapWindow call
    // emscripten_sleep — a suspend beneath the frame's C++ try scopes, which
    // Asyncify aborts on ("import invoke_* changed the state").
    SDL_GL_SetSwapInterval(0);

    int pxW = 0, pxH = 0;
    if (SDL_GetWindowSizeInPixels(m->window, &pxW, &pxH) && pxW > 0 && pxH > 0) {
        m->width = pxW;
        m->height = pxH;
    }

    sg_desc desc{};
    desc.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH;  // 🎯T133
    desc.environment.defaults.sample_count = 1;
    desc.logger.func = sokolLog;
    desc.image_pool_size = 2048; // 🎯T168.2 room for a full tile pyramid (~300 tiles)
    desc.view_pool_size = 4096;
    sg_setup(&desc);
    if (!sg_isvalid()) {
        SPDLOG_ERROR("sg_setup (WebGL2) failed");
        return;
    }

    SPDLOG_INFO("SokolContext: {}x{} WebGL2 (Emscripten)", m->width, m->height);
}

SokolContext::~SokolContext() {
    if (m->gl) {
        SDL_GL_MakeCurrent(m->window, m->gl);  // shut down sokol against the right context
        sg_shutdown();
        SDL_GL_DestroyContext(m->gl);
    } else if (sg_isvalid()) {
        sg_shutdown();
    }
    if (m->window) SDL_DestroyWindow(m->window);
    SPDLOG_INFO("SokolContext destroyed (WebGL2)");
}

int         SokolContext::width() const      { return m->width; }
int         SokolContext::height() const     { return m->height; }
bool        SokolContext::shouldQuit() const { return ge::shouldQuit(); }
SDL_Window* SokolContext::window() const     { return m->window; }

void SokolContext::beginFrame(const float clearColor[4]) {
    if (!sg_isvalid()) return;

    // Adopt canvas resizes (CSS layout change, devicePixelRatio change).
    int pxW = 0, pxH = 0;
    if (SDL_GetWindowSizeInPixels(m->window, &pxW, &pxH)
        && pxW > 0 && pxH > 0 && (pxW != m->width || pxH != m->height)) {
        m->width = pxW;
        m->height = pxH;
    }

    sg_pass pass{};
    auto& act = pass.action;
    act.colors[0].load_action  = SG_LOADACTION_CLEAR;
    act.colors[0].store_action = SG_STOREACTION_STORE;
    act.colors[0].clear_value  = clearColor
        ? sg_color{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}
        : sg_color{0.f, 0.f, 0.f, 1.f};
    act.depth.load_action  = SG_LOADACTION_CLEAR;   // 🎯T133: clear depth to far
    act.depth.store_action = SG_STOREACTION_DONTCARE;
    act.depth.clear_value  = 1.0f;

    auto& sc = pass.swapchain;
    sc.width        = m->width;
    sc.height       = m->height;
    sc.sample_count = 1;
    sc.color_format = SG_PIXELFORMAT_RGBA8;
    sc.depth_format = SG_PIXELFORMAT_DEPTH;
    sc.gl.framebuffer = 0;  // FBO 0 == the WebGL canvas default framebuffer
    sg_begin_pass(&pass);
}

void SokolContext::endFrame() {
    if (!sg_isvalid()) return;
    sg_end_pass();
    sg_commit();

    // 🎯T92.6 readback before SDL_GL_SwapWindow. glReadPixels is bottom-up;
    // flip rows to top-down RGBA8 (same as the Android GLES path).
    if (m->captureSink) {
        auto sink = std::move(m->captureSink);
        m->captureSink = nullptr;
        const int w = m->width, h = m->height;
        if (w > 0 && h > 0) {
            const std::size_t row = static_cast<std::size_t>(w) * 4;
            std::vector<std::uint8_t> raw(row * h), rgba(row * h);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
            for (int y = 0; y < h; ++y)
                std::memcpy(&rgba[static_cast<std::size_t>(h - 1 - y) * row],
                            &raw[static_cast<std::size_t>(y) * row], row);
            sink(rgba.data(), w, h);
        }
    }
    SDL_GL_SwapWindow(m->window);
}

void SokolContext::captureNextFrame(FrameCaptureSink sink) {
    m->captureSink = std::move(sink);
}

void SokolContext::onBackground() {}
void SokolContext::onForeground() {}

} // namespace ge

#endif // __EMSCRIPTEN__
