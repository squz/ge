// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/SessionHost.h>

#include <SDL3/SDL_events.h>

#include <atomic>

namespace ge {

// Rect::intersect / bbox are now defined inline as constexpr in
// SessionHost.h (T52). Out-of-line definitions removed.

struct Context::M {
    // Render-surface size in pixels (the GPU-facing unit passed to sokol).
    // Stored in pixels because that is the unit sokol/SDL work in; all
    // outward-facing rect accessors convert to pt via pixelsPerPt.
    int surfacePxW = 0;
    int surfacePxH = 0;
    DeviceClass deviceClass = DeviceClass::Unknown;
    // Insets in pt (the consumer-facing unit). Render hosts convert from
    // pixel insets at the boundary before calling setDrawSafeInsets /
    // setUiSafeInsets; consumers never see raw pixel insets.
    SafeAreaInsets drawInsetsPt{};  // display cutouts only
    SafeAreaInsets uiInsetsPt{};    // cutouts + gesture / tappable zones
    float pixelsPerPt = 1.0f;
    float deviceUiScale = 1.0f;
    la::float2 parallax{0.0f, 0.0f};
    la::float2 presentationTilt{0.0f, 0.0f};
    float frameTime = 0.0f;  // 🎯T111 EMA of run-loop dt, seconds
    std::shared_ptr<sqlpipe::Database> db;
    std::function<Pass()> swapchainPassFn;  // 🎯T101 installed by the host
    // 🎯T132 render-on-demand. `continuous` is touched only on the game thread
    // (consumer toggles it in onUpdate/onRender; the loop reads it). `redraw` is
    // the pending one-shot, set from any thread via requestRedraw().
    bool continuous = true;
    std::atomic<bool> redraw{false};
};

Context::Context(int surfaceWidth, int surfaceHeight, DeviceClass deviceClass,
                 const std::string& dbPath,
                 const std::string& schemaDdl)
    : m(std::make_shared<M>()) {
    // 🎯T132: M holds a std::atomic (non-movable), so build it in place rather
    // than moving a designated-initializer temporary in.
    m->surfacePxW = surfaceWidth;
    m->surfacePxH = surfaceHeight;
    m->deviceClass = deviceClass;
    m->db = std::make_shared<sqlpipe::Database>(dbPath, schemaDdl);
}

Rect Context::drawSafeRectInPts() const {
    const auto& s = m->drawInsetsPt;
    return fullRectInPts().adjusted({{.a = {s.x0, s.y0}, .b = {-s.x1, -s.y1}}});
}
Rect Context::uiSafeRectInPts() const {
    const auto& s = m->uiInsetsPt;
    return fullRectInPts().adjusted({{.a = {s.x0, s.y0}, .b = {-s.x1, -s.y1}}});
}
Rect Context::fullRectInPts() const {
    const float ppt = m->pixelsPerPt > 0.0f ? m->pixelsPerPt : 1.0f;
    return Rect{0, 0,
                float(m->surfacePxW) / ppt,
                float(m->surfacePxH) / ppt};
}

SafeAreaInsets Context::drawSafeInsetsInPts() const { return m->drawInsetsPt; }
SafeAreaInsets Context::uiSafeInsetsInPts()   const { return m->uiInsetsPt;   }

DeviceClass Context::deviceClass() const { return m->deviceClass; }
float Context::pixelsPerPt() const  { return m->pixelsPerPt; }
float Context::ptsPerPixel() const  { return 1.0f / m->pixelsPerPt; }
float Context::deviceUiScale() const { return m->deviceUiScale; }
la::float2 Context::parallax() const { return m->parallax; }
la::float2 Context::presentationTilt() const { return m->presentationTilt; }
float Context::frameTime() const { return m->frameTime; }
float Context::fps() const { return m->frameTime > 0.0f ? 1.0f / m->frameTime : 0.0f; }
std::shared_ptr<sqlpipe::Database> Context::db() const { return m->db; }

void Context::setSurfaceDimensions(int surfacePxW, int surfacePxH) {
    m->surfacePxW = surfacePxW;
    m->surfacePxH = surfacePxH;
}
// Insets are accepted in pt — render hosts must convert pixel insets to pt
// (divide by pixelsPerPt) before calling these setters.
void Context::setDrawSafeInsets(SafeAreaInsets saPt) { m->drawInsetsPt = saPt; }
void Context::setUiSafeInsets(SafeAreaInsets saPt)   { m->uiInsetsPt   = saPt; }
void Context::setPixelsPerPt(float v)                { m->pixelsPerPt = v; }
void Context::setDeviceUiScale(float v)              { m->deviceUiScale = v; }
void Context::setParallax(la::float2 p)              { m->parallax = p; }
void Context::setPresentationTilt(la::float2 t)      { m->presentationTilt = t; }
// 🎯T111 Single-source frame-time EMA (same 0.9/0.1 smoothing the debug overlay
// used). dt is the run-loop wall-clock delta in seconds; ignore 0 and
// multi-frame stalls (>= 1s) so a hitch can't blow up the average. fps() /
// frameTime() and the overlay readout all read this one value.
void Context::recordFrameTime(float dt) {
    if (dt > 0.0f && dt < 1.0f)
        m->frameTime = m->frameTime <= 0.0f ? dt : m->frameTime * 0.9f + dt * 0.1f;
}
void Context::setSwapchainPassFn(std::function<Pass()> fn) {
    m->swapchainPassFn = std::move(fn);
}

// 🎯T101 Delegate to the host's installed factory. The fallback (no factory
// wired) yields a no-op Pass so a stray call can't crash — though every real
// session installs one at start.
Pass Context::swapchainPass() const {
    if (m->swapchainPassFn) return m->swapchainPassFn();
    return makePass(nullptr);
}

// 🎯T132 Render-on-demand. const methods that mutate the shared per-session M
// (not the Context handle), so a consumer can call them on the const Context&
// passed to onRender / onUpdate.
void Context::setContinuousRendering(bool on) const {
    m->continuous = on;
    if (on) requestRedraw();  // resuming continuous: guarantee the next frame draws
}
bool Context::continuousRendering() const { return m->continuous; }

void Context::requestRedraw() const {
    m->redraw.store(true, std::memory_order_relaxed);
    // Wake a loop idling in pumpEvents' SDL_WaitEventTimeout. SDL's event queue
    // is thread-safe, so this is safe from any thread; the host filters this
    // sentinel out before delivering to onEvent.
    SDL_Event e{};
    e.type = SDL_EVENT_USER;
    e.user.code = kRedrawEventCode;
    SDL_PushEvent(&e);
}
void Context::markRedraw() const { m->redraw.store(true, std::memory_order_relaxed); }
bool Context::redrawPending() const { return m->redraw.load(std::memory_order_relaxed); }
bool Context::takeRedraw() const { return m->redraw.exchange(false, std::memory_order_relaxed); }

} // namespace ge
