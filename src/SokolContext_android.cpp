// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38: sokol_gfx backend for Android using OpenGL ES 3 via SDL3 / EGL.
//
// Mirrors src/SokolContext.mm (Apple Metal) for the Android target.
// sokol_gfx has no Vulkan backend that we can target (sokol's Vulkan
// branch is experimental and not part of the shipping single-header
// release), so Android is GLES-only here. On real devices that matches
// the bgfx era (we always shipped OpenGL ES on Adreno/Mali). On the
// Apple-Silicon AVD emulator GLES is what we get out of EGL too (the
// host translator only exposes GLES 3.0/3.1 cleanly); we do not branch
// to a separate emulator backend the way BgfxContext.mm did.
//
// T39 carry-over: the Android-emulator Vulkan path used to need RGBA8
// to dodge the host translator's byte-order swap. The same translator
// is in play here for GLES, so we keep the swap-chain pixel format at
// SG_PIXELFORMAT_RGBA8 (rather than BGRA8) to avoid the channel-swap
// risk on Apple-Silicon AVDs. RGBA8 is universally supported across
// real Android GPUs as well, so there's no real-device cost.
//
// Per-frame model: GLES has no Metal-drawable equivalent. The "swap-
// chain" is the default EGL framebuffer (FBO 0). We hand sokol a
// zeroed `gl.framebuffer` field per pass and call SDL_GL_SwapWindow
// after sg_commit() to present.

#if defined(__ANDROID__)

#include <ge/SokolContext.h>
#include <ge/Signal.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <spdlog/spdlog.h>

#include <GLES3/gl3.h>  // 🎯T92.6 glReadPixels for screenshot readback

#include <cstring>
#include <vector>

#define SOKOL_IMPL
#define SOKOL_GLES3
#include "sokol_gfx.h"
#include "sokol_log.h"

namespace ge {

struct SokolContext::M {
    int           width  = 0;
    int           height = 0;
    SDL_Window*   window = nullptr;
    SDL_GLContext gl     = nullptr;
    bool          paused = false;

    // 🎯T92.6 One-shot screenshot sink, fired in endFrame after sg_commit.
    SokolContext::FrameCaptureSink captureSink;

    ~M() {
        if (gl) {
            // sg_shutdown must run while the GL context is still
            // current so sokol's GL resources are released against
            // the right context.
            SDL_GL_MakeCurrent(window, gl);
            sg_shutdown();
            SDL_GL_DestroyContext(gl);
        } else {
            sg_shutdown();
        }
        if (window) SDL_DestroyWindow(window);
        SPDLOG_INFO("SokolContext destroyed");
    }
};

namespace {

// sokol-gfx callbacks bridged to spdlog so messages land in the same
// sink everything else uses (T66 — logcat via Android log sink).
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

SokolContext::SokolContext(const SokolConfig& config)
    : m(std::make_unique<M>()) {
    m->width  = config.width;
    m->height = config.height;

    ge::installSignalHandlers();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_SENSOR)) {
        SPDLOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return;
    }

    // Request an EGL context that matches what sokol_gfx's GLES3 path
    // assumes. SDL3 reads these attributes when it creates the GL
    // context inside SDL_CreateWindow (with SDL_WINDOW_OPENGL) on
    // Android — they must be set before window creation.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    const char* title = (config.title && *config.title) ? config.title : "ge";
    // Android: ask for a fullscreen surface (matches BgfxContext.mm).
    // Passing non-fullscreen config dimensions here would make SDL pin
    // the underlying SurfaceView buffer to those dimensions, producing
    // a small surface stretched across the physical screen.
    // SDL_WINDOW_OPENGL triggers SDL's EGL setup path.
    m->window = SDL_CreateWindow(title,
        config.width, config.height,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
    if (!m->window) {
        SPDLOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
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
    SDL_GL_SetSwapInterval(1); // vsync

    // Pick up the real (native) dimensions SDL gave us so sokol and
    // callers both see the actual backbuffer size rather than the
    // config hint.
    int pxW = 0, pxH = 0;
    if (SDL_GetWindowSizeInPixels(m->window, &pxW, &pxH) && pxW > 0 && pxH > 0) {
        m->width  = pxW;
        m->height = pxH;
    }

    sg_desc desc{};
    // T39: keep RGBA8 on Android. The Apple-Silicon AVD's host
    // translator reads the framebuffer bytes as RGBA regardless of the
    // GL format tag (same root cause as the bgfx-Vulkan BGRA8 swap on
    // emulator); RGBA8 is also universally supported across real
    // Android GPUs, so there is no real-device cost.
    desc.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_NONE;
    desc.environment.defaults.sample_count = 1;
    desc.logger.func = sokolLog;
    sg_setup(&desc);
    if (!sg_isvalid()) {
        SPDLOG_ERROR("sg_setup failed");
        return;
    }

    SPDLOG_INFO("SokolContext: {}x{} GLES3 (Android)", m->width, m->height);
    // Also surface to logcat for first-launch visibility before the
    // spdlog Android sink is necessarily wired.
    SDL_Log("ge: SokolContext %dx%d GLES3 (Android)", m->width, m->height);
}

SokolContext::~SokolContext() = default;

int         SokolContext::width()      const { return m->width;  }
int         SokolContext::height()     const { return m->height; }
bool        SokolContext::shouldQuit() const { return ge::shouldQuit(); }
SDL_Window* SokolContext::window()     const { return m->window; }

void SokolContext::beginFrame(const float clearColor[4]) {
    if (m->paused) return;
    if (!sg_isvalid()) return;

    // Re-pick the surface size each frame. On Android the system can
    // resize the SurfaceView (rotation, multi-window resume) and the
    // backbuffer dimensions need to track it for the GL viewport that
    // sokol derives from the swapchain to be right.
    int pxW = 0, pxH = 0;
    if (SDL_GetWindowSizeInPixels(m->window, &pxW, &pxH)
        && pxW > 0 && pxH > 0
        && (pxW != m->width || pxH != m->height)) {
        m->width  = pxW;
        m->height = pxH;
    }

    sg_pass pass{};
    auto& act = pass.action;
    act.colors[0].load_action  = SG_LOADACTION_CLEAR;
    act.colors[0].store_action = SG_STOREACTION_STORE;
    if (clearColor) {
        act.colors[0].clear_value = { clearColor[0], clearColor[1],
                                       clearColor[2], clearColor[3] };
    } else {
        act.colors[0].clear_value = { 0.f, 0.f, 0.f, 1.f };
    }

    auto& sc = pass.swapchain;
    sc.width        = m->width;
    sc.height       = m->height;
    sc.sample_count = 1;
    sc.color_format = SG_PIXELFORMAT_RGBA8;
    sc.depth_format = SG_PIXELFORMAT_NONE;
    // FBO 0 == default (EGL window) framebuffer. sokol validates that
    // this stays at zero for swapchain passes on GL backends.
    sc.gl.framebuffer = 0;

    sg_begin_pass(&pass);
}

void SokolContext::captureNextFrame(FrameCaptureSink sink) {
    m->captureSink = std::move(sink);
}

void SokolContext::endFrame() {
    if (m->paused) return;
    if (!sg_isvalid()) return;

    sg_end_pass();
    sg_commit();

    // 🎯T92.6 Screenshot readback. sg_commit has flushed the GL render into
    // the default framebuffer; read it back BEFORE SDL_GL_SwapWindow discards
    // the back buffer. glReadPixels is bottom-up (GL origin bottom-left), so
    // flip rows to deliver top-down RGBA8 — the sink's uniform contract.
    if (m->captureSink) {
        auto sink = std::move(m->captureSink);
        m->captureSink = nullptr;
        const int w = m->width, h = m->height;
        if (w > 0 && h > 0) {
            const std::size_t row = static_cast<std::size_t>(w) * 4;
            std::vector<std::uint8_t> raw(row * h);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
            std::vector<std::uint8_t> rgba(row * h);
            for (int y = 0; y < h; ++y)
                std::memcpy(&rgba[static_cast<std::size_t>(h - 1 - y) * row],
                            &raw[static_cast<std::size_t>(y) * row], row);
            sink(rgba.data(), w, h);
        }
    }

    // GLES present: swap the EGL backbuffer to the screen.
    SDL_GL_SwapWindow(m->window);
}

void SokolContext::onBackground() {
    // Android destroys the SurfaceView's underlying ANativeWindow on
    // background; SDL3 invalidates the EGL surface to match. Skip
    // begin/endFrame while paused — the main loop is expected to stop
    // pumping frames once SDL3 reports the window is hidden anyway.
    m->paused = true;
    SPDLOG_INFO("SokolContext: backgrounded (paused)");
}

void SokolContext::onForeground() {
    // SDL3 recreates the EGL surface for us during the foreground
    // transition (the window's ANativeWindow* changes; SDL rebinds it
    // before redelivering events). The EGL context itself is preserved,
    // so sokol_gfx's GLES state survives without an sg_shutdown /
    // sg_setup cycle — it picks up the new default FBO transparently on
    // the next bind.
    //
    // Re-make the context current explicitly: SDL3 has been observed
    // (on some devices) to lose the current binding across the surface
    // recreate, which would otherwise leave subsequent GL calls hitting
    // a null context.
    if (m->window && m->gl) {
        if (!SDL_GL_MakeCurrent(m->window, m->gl)) {
            SPDLOG_WARN("SDL_GL_MakeCurrent on foreground failed: {}",
                        SDL_GetError());
        }
        int w = 0, h = 0;
        if (SDL_GetWindowSizeInPixels(m->window, &w, &h) && w > 0 && h > 0) {
            m->width  = w;
            m->height = h;
        }
        SPDLOG_INFO("SokolContext: foregrounded, surface {}x{}",
                    m->width, m->height);
    }
    m->paused = false;
}

} // namespace ge

#endif // __ANDROID__
