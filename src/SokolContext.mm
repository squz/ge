// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38 spike: sokol_gfx Metal backend on macOS + iOS via SDL3.

#include <ge/SokolContext.h>
#include <ge/Signal.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define SOKOL_IMPL
#define SOKOL_METAL
#include "sokol_gfx.h"
#include "sokol_log.h"

#include <TargetConditionals.h>
#if !TARGET_OS_OSX
#import <UIKit/UIKit.h>
#endif

namespace ge {

struct SokolContext::M {
    int             width  = 0;
    int             height = 0;
    SDL_Window*     window = nullptr;
    SDL_MetalView   metalView = nullptr;
    CAMetalLayer*   layer = nil;
    id<MTLDevice>   device = nil;

    // Per-frame state, valid only between beginFrame/endFrame.
    id<CAMetalDrawable>    currentDrawable = nil;

    // 🎯T92.6 One-shot screenshot sink, armed via captureNextFrame, fired and
    // cleared inside endFrame after sg_commit. Dedicated readback texture +
    // command queue, lazily created on first capture.
    SokolContext::FrameCaptureSink captureSink;
    id<MTLCommandQueue>            captureQueue = nil;

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

// The CAMetalDrawable handed to sokol_gfx via the per-frame swapchain
// block must outlive the sg_commit() call, which is guaranteed because
// sokol takes a strong reference internally when it enqueues the encoder
// work. We just hold it on M for the duration of the frame and release
// it in endFrame.
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
#if TARGET_OS_OSX
    const Uint64 windowFlags =
        SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    // iOS windows are always fullscreen; SDL_WINDOW_RESIZABLE doesn't apply.
    // SDL_WINDOW_HIGH_PIXEL_DENSITY is required (🎯T82): SDL3 does NOT default
    // it on for iOS — it only raises the metalview's contentScaleFactor to the
    // device nativeScale when the flag is present. Without it,
    // SDL_GetWindowSizeInPixels returns points, not pixels, and ge::Context's
    // divide-by-pixelsPerPt math reports a phantom ~1/3-size surface, so taps
    // land in the upper-left third of the screen. Pre-sokol BgfxContext.mm set
    // this flag on the same path.
    const Uint64 windowFlags = SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    Uint64 flags = windowFlags;
    if (config.hidden) flags |= SDL_WINDOW_HIDDEN;  // 🎯T124 headless render
    m->window = SDL_CreateWindow(title, config.width, config.height, flags);
    if (!m->window) {
        SPDLOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return;
    }

    m->metalView = SDL_Metal_CreateView(m->window);
    m->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m->metalView);

    m->device = MTLCreateSystemDefaultDevice();
    m->layer.device = m->device;
    m->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
#ifndef NDEBUG
    // 🎯T92.6 Dev builds keep the drawable readable so the app-channel
    // screenshot path can blit it back to the CPU. framebufferOnly = YES
    // (the release default just below) blocks using the drawable as a blit
    // source. The dev-only cost is a minor GPU optimisation the simulator /
    // a debug build won't miss.
    m->layer.framebufferOnly = NO;
#else
    m->layer.framebufferOnly = YES;
#endif

#if !TARGET_OS_OSX
    // Paint the Metal layer and every UIView/UIWindow above it opaque
    // black. During the iOS orientation-change animation the system
    // snapshots the view hierarchy and cross-fades — if any parent
    // view's backgroundColor is nil/undefined it shows as a pink
    // flash. Walk up the hierarchy setting black on everything.
    {
        CGFloat black[] = {0.f, 0.f, 0.f, 1.f};
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGColorRef blackCG = CGColorCreate(cs, black);
        m->layer.backgroundColor = blackCG;
        m->layer.opaque = YES;
        CGColorRelease(blackCG);
        CGColorSpaceRelease(cs);
        UIView* uiView = (__bridge UIView*)m->metalView;
        for (UIView* v = uiView; v != nil; v = v.superview) {
            v.backgroundColor = [UIColor blackColor];
            v.opaque = YES;
        }
        UIWindow* uiWindow = uiView.window;
        if (uiWindow) {
            uiWindow.backgroundColor = [UIColor blackColor];
        }
    }
#endif

    // Pick up the real pixel dimensions; Retina blows up the requested
    // 820×1180 to the actual backbuffer size.
    int pxW = 0, pxH = 0;
    if (SDL_GetWindowSizeInPixels(m->window, &pxW, &pxH) && pxW > 0 && pxH > 0) {
        m->width  = pxW;
        m->height = pxH;
    }
    m->layer.drawableSize = CGSizeMake(m->width, m->height);

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

    // sokol_gfx-Metal creates AND commits its own MTLCommandBuffer each
    // frame (from the device handed to sg_setup) and presents the drawable
    // inside sg_commit(). ge must NOT create its own command buffer here:
    // an uncommitted [queue commandBuffer] every frame exhausts the
    // MTLCommandQueue's in-flight limit (~64), after which commandBuffer
    // blocks and beginFrame wedges the main thread — fatal on the iOS
    // Simulator's MTLSimDriver (renders ~1s then freezes). 🎯T89.

    sg_begin_pass(&pass);
}

void SokolContext::captureNextFrame(FrameCaptureSink sink) {
    m->captureSink = std::move(sink);
}

void SokolContext::endFrame() {
    if (!sg_isvalid()) return;
    if (!m->currentDrawable) return;

    sg_end_pass();
    sg_commit();

    // 🎯T92.6 Screenshot readback. sokol's sg_commit has committed the render
    // (and present) to its own command queue but not waited for it; we blit
    // the drawable's texture on that SAME queue (sg_mtl_command_queue) so the
    // copy is ordered strictly after the render, then wait and read back. The
    // drawable is still alive here (dropped below).
    if (m->captureSink) {
        auto sink = std::move(m->captureSink);
        m->captureSink = nullptr;
        id<MTLTexture> src = m->currentDrawable.texture;
        const NSUInteger w = src.width, h = src.height;
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:w height:h mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;  // CPU-readable (unified memory)
        desc.usage       = MTLTextureUsageShaderRead;
        id<MTLTexture> dst = [m->device newTextureWithDescriptor:desc];
        if (!m->captureQueue) m->captureQueue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();
        id<MTLCommandBuffer>     cb   = [m->captureQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:dst destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4);
        [dst getBytes:bgra.data() bytesPerRow:w * 4
           fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        // newTextureWithDescriptor is +1 under MRR — release or leak 12.6 MB
        // per capture. Invisible for one-shot screenshots; at the server's
        // per-frame capture rate it exhausted system RAM (~1.3 GB/s observed).
        [dst release];
        // BGRA8 → RGBA8 so the sink's contract is uniform across backends.
        std::vector<std::uint8_t> rgba(bgra.size());
        for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
            rgba[i * 4 + 0] = bgra[i * 4 + 2];
            rgba[i * 4 + 1] = bgra[i * 4 + 1];
            rgba[i * 4 + 2] = bgra[i * 4 + 0];
            rgba[i * 4 + 3] = bgra[i * 4 + 3];
        }
        sink(rgba.data(), static_cast<int>(w), static_cast<int>(h));
    }

    // sokol_gfx-Metal handles the [commandBuffer presentDrawable:...]
    // internally when given a drawable in the swapchain config. So we
    // just drop our drawable ref.
    m->currentDrawable = nil;
}

// Apple lifecycle: the CAMetalLayer survives backgrounding, but a drawable
// reference (m->currentDrawable) acquired before the background transition
// can be stale by the time we foreground — sokol_gfx's swapchain pass
// then renders into an invalid Metal texture and the first foreground
// frame comes back black (multimaze2 🎯T29 / ge 🎯T108).
//
// onBackground stays a no-op because endFrame already drops the in-flight
// drawable; onForeground defensively clears it so the next beginFrame's
// [layer nextDrawable] returns a fresh in-sync drawable rather than reusing
// a stale ref. The Android branch (SokolContext_android.cpp) needs real
// teardown because the SurfaceView's ANativeWindow is destroyed on
// background — different mechanism, same shape.
void SokolContext::onBackground() {}
void SokolContext::onForeground() {
    m->currentDrawable = nil;
}

} // namespace ge
