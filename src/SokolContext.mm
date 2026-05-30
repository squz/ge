// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38 spike: sokol_gfx Metal backend on macOS via SDL3.

#include <ge/SokolContext.h>
#include <ge/Signal.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <spdlog/spdlog.h>

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define SOKOL_IMPL
#define SOKOL_METAL
#include "sokol_gfx.h"
#include "sokol_log.h"

#include <TargetConditionals.h>
#if !TARGET_OS_OSX
#  error "SokolContext spike: macOS only for now"
#endif

namespace ge {

struct SokolContext::M {
    int             width  = 0;
    int             height = 0;
    SDL_Window*     window = nullptr;
    SDL_MetalView   metalView = nullptr;
    CAMetalLayer*   layer = nil;
    id<MTLDevice>   device = nil;
    id<MTLCommandQueue> queue = nil;

    // Per-frame state, valid only between beginFrame/endFrame.
    id<CAMetalDrawable>    currentDrawable = nil;
    id<MTLCommandBuffer>   currentCmdBuf   = nil;

    ~M() {
        sg_shutdown();
        if (metalView) SDL_Metal_DestroyView(metalView);
        if (window)    SDL_DestroyWindow(window);
        SPDLOG_INFO("SokolContext destroyed");
    }
};

namespace {

// sokol-gfx callbacks bridged to spdlog so messages land in the same
// sink everything else uses (T66 — Apple os_log / Android logcat).
void sokolLog(const char* tag, uint32_t log_level, uint32_t log_item,
              const char* message, uint32_t line_nr, const char* filename,
              void* /*user*/) {
    SPDLOG_LOGGER_CALL(spdlog::default_logger().get(),
        log_level <= 1 ? spdlog::level::err : spdlog::level::warn,
        "sokol[{}] {}({},{}): {}",
        tag, filename ? filename : "?", line_nr, log_item,
        message ? message : "");
}

// Apple Metal command-queue / drawable pointers handed to sokol_gfx
// via the per-frame swapchain block must outlive the sg_commit() call,
// which is guaranteed because sokol takes a strong reference internally
// when it enqueues the encoder work. We just hold them on M for the
// duration of the frame and release them in endFrame.
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

    const char* title = (config.title && *config.title) ? config.title : "ge";
    m->window = SDL_CreateWindow(title,
        config.width, config.height,
        SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m->window) {
        SPDLOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return;
    }

    m->metalView = SDL_Metal_CreateView(m->window);
    m->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m->metalView);

    m->device = MTLCreateSystemDefaultDevice();
    m->layer.device = m->device;
    m->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    m->layer.framebufferOnly = YES;

    // Pick up the real pixel dimensions; Retina blows up the requested
    // 820×1180 to the actual backbuffer size.
    int pxW = 0, pxH = 0;
    if (SDL_GetWindowSizeInPixels(m->window, &pxW, &pxH) && pxW > 0 && pxH > 0) {
        m->width  = pxW;
        m->height = pxH;
    }
    m->layer.drawableSize = CGSizeMake(m->width, m->height);

    m->queue = [m->device newCommandQueue];

    sg_desc desc{};
    desc.environment.defaults.color_format = SG_PIXELFORMAT_BGRA8;
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_NONE;
    desc.environment.defaults.sample_count = 1;
    desc.environment.metal.device = (__bridge const void*)m->device;
    desc.logger.func = sokolLog;
    sg_setup(&desc);
    if (!sg_isvalid()) {
        SPDLOG_ERROR("sg_setup failed");
        return;
    }

    SPDLOG_INFO("SokolContext: {}x{} Metal ({})",
                m->width, m->height,
                [[m->device name] UTF8String]);
}

SokolContext::~SokolContext() = default;

int  SokolContext::width()  const { return m->width;  }
int  SokolContext::height() const { return m->height; }
bool SokolContext::shouldQuit() const { return ge::shouldQuit(); }
SDL_Window* SokolContext::window() const { return m->window; }

void SokolContext::beginFrame(const float clearColor[4]) {
    // Handle window resize each frame; CAMetalLayer drawableSize must
    // be kept in sync or nextDrawable returns stretched / black frames.
    int pxW = 0, pxH = 0;
    if (SDL_GetWindowSizeInPixels(m->window, &pxW, &pxH)
        && pxW > 0 && pxH > 0
        && (pxW != m->width || pxH != m->height)) {
        m->width  = pxW;
        m->height = pxH;
        m->layer.drawableSize = CGSizeMake(pxW, pxH);
    }

    m->currentDrawable = [m->layer nextDrawable];
    if (!m->currentDrawable) {
        SPDLOG_WARN("nextDrawable returned nil; skipping frame");
        return;
    }
    m->currentCmdBuf = [m->queue commandBuffer];

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
    sc.color_format = SG_PIXELFORMAT_BGRA8;
    sc.depth_format = SG_PIXELFORMAT_NONE;
    sc.metal.current_drawable = (__bridge const void*)m->currentDrawable;

    // sokol needs the active MTLCommandBuffer for the frame; it picks
    // one up from environment.metal.* callbacks normally, but in our
    // pull-mode wrapper we pass it through every frame via the swapchain.
    // Actually: sokol-Metal pulls the cmd buffer from desc.environment
    // at init; per-frame we just hand it the drawable. The command
    // buffer it uses internally is created from the device's default
    // queue. We keep our own cmd buffer for explicit present.
    // (Confirmed: sokol_gfx Metal creates an internal cmd buffer per
    // frame from the device; we don't have to thread ours through.)
    (void)m->currentCmdBuf;

    sg_begin_pass(&pass);
}

void SokolContext::endFrame() {
    if (!sg_isvalid()) return;
    if (!m->currentDrawable) return;

    sg_end_pass();
    sg_commit();

    // sokol_gfx-Metal handles the [commandBuffer presentDrawable:...]
    // internally when given a drawable in the swapchain config. So we
    // just drop our refs.
    m->currentDrawable = nil;
    m->currentCmdBuf   = nil;
}

} // namespace ge
