// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/SessionHost.h>

namespace ge {

// Rect::intersect / bbox are now defined inline as constexpr in
// SessionHost.h (T52). Out-of-line definitions removed.

struct Context::M {
    int surfaceWidth;
    int surfaceHeight;
    DeviceClass deviceClass;
    SafeAreaInsets drawInsets;  // display cutouts only
    SafeAreaInsets uiInsets;    // cutouts + gesture / tappable zones
    float pixelsPerPt = 1.0f;
    float deviceUiScale = 1.0f;
    la::float2 parallax{0.0f, 0.0f};
    std::shared_ptr<sqlpipe::Database> db;
};

Context::Context(int surfaceWidth, int surfaceHeight, DeviceClass deviceClass,
                 const std::string& dbPath,
                 const std::string& schemaDdl)
    : m(std::make_shared<M>(M{
        .surfaceWidth = surfaceWidth,
        .surfaceHeight = surfaceHeight,
        .deviceClass = deviceClass,
        .db = std::make_shared<sqlpipe::Database>(dbPath, schemaDdl),
    })) {}

Rect Context::drawSafeRect() const {
    const auto& s = m->drawInsets;
    return fullRect().adjusted({{.a = {s.x0, s.y0}, .b = {-s.x1, -s.y1}}});
}
Rect Context::uiSafeRect() const {
    const auto& s = m->uiInsets;
    return fullRect().adjusted({{.a = {s.x0, s.y0}, .b = {-s.x1, -s.y1}}});
}
Rect Context::fullRect()     const {
    return Rect{0, 0, float(m->surfaceWidth), float(m->surfaceHeight)};
}

SafeAreaInsets Context::drawSafeInsets() const { return m->drawInsets; }
SafeAreaInsets Context::uiSafeInsets()   const { return m->uiInsets;   }

DeviceClass Context::deviceClass() const { return m->deviceClass; }
float Context::pixelsPerPt() const  { return m->pixelsPerPt; }
float Context::ptsPerPixel() const  { return 1.0f / m->pixelsPerPt; }
float Context::deviceUiScale() const { return m->deviceUiScale; }
la::float2 Context::parallax() const { return m->parallax; }
std::shared_ptr<sqlpipe::Database> Context::db() const { return m->db; }

void Context::setDimensions(int surfaceWidth, int surfaceHeight) {
    m->surfaceWidth  = surfaceWidth;
    m->surfaceHeight = surfaceHeight;
}
void Context::setDrawSafeInsets(SafeAreaInsets sa) { m->drawInsets = sa; }
void Context::setUiSafeInsets(SafeAreaInsets sa)   { m->uiInsets   = sa; }
void Context::setPixelsPerPt(float v)              { m->pixelsPerPt = v; }
void Context::setDeviceUiScale(float v)            { m->deviceUiScale = v; }
void Context::setParallax(la::float2 p)            { m->parallax = p; }

} // namespace ge
