// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/SessionHost.h>

namespace ge {

// Rect::intersect / bbox are now defined inline as constexpr in
// SessionHost.h (T52). Out-of-line definitions removed.

struct Context::M {
    // Render-surface size in pixels (the GPU-facing unit passed to bgfx).
    // Stored in pixels because that is the unit bgfx/SDL work in; all
    // outward-facing rect accessors convert to pt via pixelsPerPt.
    int surfacePxW;
    int surfacePxH;
    DeviceClass deviceClass;
    // Insets in pt (the consumer-facing unit). Render hosts convert from
    // pixel insets at the boundary before calling setDrawSafeInsets /
    // setUiSafeInsets; consumers never see raw pixel insets.
    SafeAreaInsets drawInsetsPt;  // display cutouts only
    SafeAreaInsets uiInsetsPt;    // cutouts + gesture / tappable zones
    float pixelsPerPt = 1.0f;
    float deviceUiScale = 1.0f;
    la::float2 parallax{0.0f, 0.0f};
    std::shared_ptr<sqlpipe::Database> db;
};

Context::Context(int surfaceWidth, int surfaceHeight, DeviceClass deviceClass,
                 const std::string& dbPath,
                 const std::string& schemaDdl)
    : m(std::make_shared<M>(M{
        .surfacePxW = surfaceWidth,
        .surfacePxH = surfaceHeight,
        .deviceClass = deviceClass,
        .db = std::make_shared<sqlpipe::Database>(dbPath, schemaDdl),
    })) {}

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

} // namespace ge
