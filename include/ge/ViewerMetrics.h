// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Viewer surface metrics for the server (wire) backend.
//
// Physical apps fill Context from the local OS each frame. Server apps fill
// the same Context fields from the attached player's DeviceInfo /
// SafeAreaUpdate. There is no public "select backend" API — the host path is
// chosen at compile time (mobile/desktop app vs server app).
//
// Content vs viewer (🎯T154):
//   fullRect / surface px  — CONTENT surface (server swapchain / encode size).
//                            Not the phone's native resolution.
//   drawSafe / uiSafe      — VIEWER chrome mapped into content space so games
//                            lay out against the glass without modality branches.
//   deviceClass / uiScale  — VIEWER form factor while a player is attached.
//   pixelsPerPt            — conversion for the content surface (stable fullRect);
//                            physical mm on glass uses deviceUiScale + viewer ratio.

#pragma once

#include <ge/SessionHost.h>  // SafeAreaInsets, DeviceClass

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ge {

// Snapshot of the remote viewing surface (player window).
struct ViewerWindow {
    int w = 0;              // window width, pixels
    int h = 0;              // window height, pixels
    float pixelRatio = 1.f; // display scale (e.g. 3 for @3x)
    DeviceClass deviceClass = DeviceClass::Unknown;
    // UI-safe rect in window pixels (cutouts + gestures). safeW==0 → full window.
    int safeX = 0, safeY = 0, safeW = 0, safeH = 0;
    // Draw-safe rect (cutouts only). drawSafeW==0 → same as ui-safe.
    int drawSafeX = 0, drawSafeY = 0, drawSafeW = 0, drawSafeH = 0;
    // 🎯T156.2: seat sensor authority — the glass declared a real
    // accelerometer (DeviceInfo kCapHasAccelerometer).
    bool hasAccelerometer = false;
};

// Aspect-fit content (contentW×contentH) into window (ww×wh) — same geometry
// as PlayerRender letterboxing.
inline void fitContentRect(int ww, int wh, float contentAspect,
                           float& outX, float& outY, float& outW, float& outH) {
    if (contentAspect <= 0.f || ww <= 0 || wh <= 0) {
        outX = 0;
        outY = 0;
        outW = float(std::max(ww, 0));
        outH = float(std::max(wh, 0));
        return;
    }
    const float winAspect = float(ww) / float(wh);
    if (contentAspect > winAspect) {
        outW = float(ww);
        outH = float(ww) / contentAspect;
        outX = 0.f;
        outY = (float(wh) - outH) * 0.5f;
    } else {
        outH = float(wh);
        outW = float(wh) * contentAspect;
        outX = (float(ww) - outW) * 0.5f;
        outY = 0.f;
    }
}

// Map one window-space safe rect into content-surface insets (pt).
inline SafeAreaInsets mapWindowRectToContentInsets(int winW, int winH,
                                                   int rectX, int rectY,
                                                   int rectW, int rectH,
                                                   int surfacePxW, int surfacePxH,
                                                   float contentPixelsPerPt) {
    SafeAreaInsets out{};
    if (surfacePxW <= 0 || surfacePxH <= 0 || winW <= 0 || winH <= 0) return out;

    const float ppt = contentPixelsPerPt > 0.f ? contentPixelsPerPt : 1.f;
    const float contentAspect = float(surfacePxW) / float(surfacePxH);

    float cx, cy, cw, ch;
    fitContentRect(winW, winH, contentAspect, cx, cy, cw, ch);
    if (cw < 1.f || ch < 1.f) return out;

    float sx = 0.f, sy = 0.f, sw = float(winW), sh = float(winH);
    if (rectW > 0 && rectH > 0) {
        sx = float(rectX);
        sy = float(rectY);
        sw = float(rectW);
        sh = float(rectH);
    }

    const float ix0 = std::max(sx, cx);
    const float iy0 = std::max(sy, cy);
    const float ix1 = std::min(sx + sw, cx + cw);
    const float iy1 = std::min(sy + sh, cy + ch);
    if (ix1 <= ix0 || iy1 <= iy0) {
        out.x0 = float(surfacePxW) / ppt;
        out.y0 = float(surfacePxH) / ppt;
        return out;
    }

    const float sx0 = (ix0 - cx) / cw * float(surfacePxW);
    const float sy0 = (iy0 - cy) / ch * float(surfacePxH);
    const float sx1 = (ix1 - cx) / cw * float(surfacePxW);
    const float sy1 = (iy1 - cy) / ch * float(surfacePxH);

    out.x0 = std::max(0.f, sx0) / ppt;
    out.y0 = std::max(0.f, sy0) / ppt;
    out.x1 = std::max(0.f, float(surfacePxW) - sx1) / ppt;
    out.y1 = std::max(0.f, float(surfacePxH) - sy1) / ppt;
    return out;
}

// Map UI-safe rect (legacy name). Prefer mapViewerDualSafeInsets for T154.
inline SafeAreaInsets mapViewerSafeInsetsToContent(const ViewerWindow& v,
                                                   int surfacePxW,
                                                   int surfacePxH,
                                                   float contentPixelsPerPt) {
    return mapWindowRectToContentInsets(v.w, v.h, v.safeX, v.safeY, v.safeW, v.safeH,
                                        surfacePxW, surfacePxH, contentPixelsPerPt);
}

// Dual draw/ui mapping. When drawSafeW==0, draw insets equal ui insets.
struct DualSafeInsets {
    SafeAreaInsets draw{};
    SafeAreaInsets ui{};
};

inline DualSafeInsets mapViewerDualSafeInsets(const ViewerWindow& v,
                                              int surfacePxW, int surfacePxH,
                                              float contentPixelsPerPt) {
    DualSafeInsets d;
    d.ui = mapWindowRectToContentInsets(v.w, v.h, v.safeX, v.safeY, v.safeW, v.safeH,
                                        surfacePxW, surfacePxH, contentPixelsPerPt);
    if (v.drawSafeW > 0 && v.drawSafeH > 0) {
        d.draw = mapWindowRectToContentInsets(v.w, v.h,
                                              v.drawSafeX, v.drawSafeY,
                                              v.drawSafeW, v.drawSafeH,
                                              surfacePxW, surfacePxH, contentPixelsPerPt);
    } else {
        d.draw = d.ui;
    }
    return d;
}

// Atomic snapshot written by ServerSession (input thread) and read by
// DirectRenderHost::refreshFrame (game thread) under stream.
struct ViewerMetricsStore {
    std::atomic<bool> valid{false};
    std::atomic<bool> hasAccel{false};
    std::atomic<int> w{0};
    std::atomic<int> h{0};
    std::atomic<int> pixelRatio{1};
    std::atomic<int> deviceClass{0};
    std::atomic<int> safeX{0};
    std::atomic<int> safeY{0};
    std::atomic<int> safeW{0};
    std::atomic<int> safeH{0};
    std::atomic<int> drawSafeX{0};
    std::atomic<int> drawSafeY{0};
    std::atomic<int> drawSafeW{0};
    std::atomic<int> drawSafeH{0};

    ViewerWindow snapshot() const {
        ViewerWindow v;
        if (!valid.load(std::memory_order_acquire)) return v;
        v.w = w.load(std::memory_order_relaxed);
        v.h = h.load(std::memory_order_relaxed);
        v.pixelRatio = float(std::max(1, pixelRatio.load(std::memory_order_relaxed)));
        const int dc = deviceClass.load(std::memory_order_relaxed);
        v.deviceClass = (dc >= 0 && dc <= 3)
            ? static_cast<DeviceClass>(dc)
            : DeviceClass::Unknown;
        v.safeX = safeX.load(std::memory_order_relaxed);
        v.safeY = safeY.load(std::memory_order_relaxed);
        v.safeW = safeW.load(std::memory_order_relaxed);
        v.safeH = safeH.load(std::memory_order_relaxed);
        v.drawSafeX = drawSafeX.load(std::memory_order_relaxed);
        v.drawSafeY = drawSafeY.load(std::memory_order_relaxed);
        v.drawSafeW = drawSafeW.load(std::memory_order_relaxed);
        v.drawSafeH = drawSafeH.load(std::memory_order_relaxed);
        v.hasAccelerometer = hasAccel.load(std::memory_order_relaxed);
        return v;
    }

    void applyDeviceInfo(int ww, int wh, int pr, int dc,
                         int sx, int sy, int sw, int sh,
                         int dx = 0, int dy = 0, int dw = 0, int dh = 0,
                         bool accel = false) {
        hasAccel.store(accel, std::memory_order_relaxed);
        w.store(ww, std::memory_order_relaxed);
        h.store(wh, std::memory_order_relaxed);
        pixelRatio.store(pr > 0 ? pr : 1, std::memory_order_relaxed);
        deviceClass.store(dc, std::memory_order_relaxed);
        safeX.store(sx, std::memory_order_relaxed);
        safeY.store(sy, std::memory_order_relaxed);
        safeW.store(sw, std::memory_order_relaxed);
        safeH.store(sh, std::memory_order_relaxed);
        drawSafeX.store(dx, std::memory_order_relaxed);
        drawSafeY.store(dy, std::memory_order_relaxed);
        drawSafeW.store(dw, std::memory_order_relaxed);
        drawSafeH.store(dh, std::memory_order_relaxed);
        valid.store(true, std::memory_order_release);
    }

    void applySafeArea(int sx, int sy, int sw, int sh,
                       int dx = 0, int dy = 0, int dw = 0, int dh = 0) {
        safeX.store(sx, std::memory_order_relaxed);
        safeY.store(sy, std::memory_order_relaxed);
        safeW.store(sw, std::memory_order_relaxed);
        safeH.store(sh, std::memory_order_relaxed);
        drawSafeX.store(dx, std::memory_order_relaxed);
        drawSafeY.store(dy, std::memory_order_relaxed);
        drawSafeW.store(dw, std::memory_order_relaxed);
        drawSafeH.store(dh, std::memory_order_relaxed);
    }
};

} // namespace ge
