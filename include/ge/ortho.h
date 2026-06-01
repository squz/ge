// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Orthographic projection builders for 2D layouts.
//
// `letterbox(content, screen)` builds a projection matrix that maps a
// content rect (0..content.x, 0..content.y) onto clip space such that
// content's aspect ratio is preserved and the dominant-axis fits the
// screen. The opposite axis is *extended* to fill the rest — drawing
// outside (0..content.x, 0..content.y) lands in the letterbox /
// pillarbox bars (useful for backdrop art that bleeds past the
// nominal content rect; consumers that want hard black bars simply
// don't draw outside the nominal rect).
//
// `pixelOrtho(w, h)` is the no-aspect-correction variant: maps
// (0..w, 0..h) directly to clip space, top-left origin, +Y down.
// Suitable for chrome (bars, dialogs) that should be specified in
// physical screen pixels.
//
// `screenToContent(p, content, screen)` is the inverse of
// `letterbox` — converts a screen-space point (e.g. an SDL_Event
// coordinate) back into content-space for hit-testing under a
// letterboxed projection.
//
// All three are constexpr. Top-left origin / +Y down convention
// matches bgfx clip-space when the renderer is HLSL / Metal / Vulkan
// (the default for ge's targets).

#pragma once

#include <ge/Linalg.h>
#include <ge/SessionHost.h>  // ge::Rect

#include <algorithm>
#include <cmath>

namespace ge::ortho {

namespace detail {

// The content-space rect that maps to the *full* clip-space [-1, 1]²
// under `letterbox(content, screen)`. The visible nominal content
// rect (0..content.x, 0..content.y) sits centered inside this; the
// extension beyond (negative coords or beyond content's width/height)
// is the letterbox / pillarbox region.
constexpr Rect contentVisible(la::float2 content, la::float2 screen) {
    // Cross-multiply to avoid the divisions in aspect ratios.
    if (screen.x * content.y > screen.y * content.x) {
        // Screen is wider than content → pillarbox (extend horizontally).
        const float extra = (screen.x * content.y - screen.y * content.x)
                          / (2.0f * screen.y);
        return Rect{-extra, 0.0f, content.x + 2.0f * extra, content.y};
    }
    // Screen is taller than content → letterbox (extend vertically).
    const float extra = (screen.y * content.x - screen.x * content.y)
                      / (2.0f * screen.x);
    return Rect{0.0f, -extra, content.x, content.y + 2.0f * extra};
}

} // namespace detail

constexpr la::float4x4 letterbox(la::float2 content, la::float2 screen) {
    const Rect v = detail::contentVisible(content, screen);
    const float l = v.x, t = v.y, r = v.x + v.w, b = v.y + v.h;
    return {{ 2.0f / (r - l), 0.0f, 0.0f, 0.0f },
            { 0.0f, 2.0f / (t - b), 0.0f, 0.0f },
            { 0.0f, 0.0f,           1.0f, 0.0f },
            { -(r + l) / (r - l), -(t + b) / (t - b), 0.0f, 1.0f }};
}

constexpr la::float4x4 pixelOrtho(float w, float h) {
    return {{  2.0f / w, 0.0f,     0.0f, 0.0f },
            {  0.0f,    -2.0f / h, 0.0f, 0.0f },
            {  0.0f,     0.0f,     1.0f, 0.0f },
            { -1.0f,     1.0f,     0.0f, 1.0f }};
}

constexpr la::float2 screenToContent(la::float2 screenPt,
                                      la::float2 content,
                                      la::float2 screen) {
    const Rect v = detail::contentVisible(content, screen);
    return {v.x + (screenPt.x / screen.x) * v.w,
            v.y + (screenPt.y / screen.y) * v.h};
}

// 🎯T94 — tilt-perspective projection.
//
// `tilt(aspect, tiltRad)` returns a clip→clip transform you *compose* with
// your normal projection (`mul(ortho::tilt(aspect, t), yourProj)`): it rotates
// the [-1,1]² clip quad about an axis perpendicular to `tiltRad`, applies a
// perspective foreshortening (camera at z = D = 2, fovY = 2·atan(1/D)), and
// folds in a bounding-box fit so the rotated quad stays inside the screen.
// The GPU's own w-divide produces the perspective; at zero tilt the result is
// the identity (un-tilted) so the same call is correct on every platform.
//
// `aspect` is the screen aspect (w/h) — it un-stretches the clip square before
// rotating so the tilt isn't sheared, matching it back afterward.
//
// `tiltRad` is a *presentation-tilt* vector: its direction is the tilt
// direction and its magnitude is the tilt angle in radians. Feed it from
// `Context::presentationTilt()`, which is the AccelSynth contribution and is
// {0,0} whenever a real accelerometer is present — so the view tilts only on
// synth platforms (desktop / sim / emu), never doubling up on a device whose
// screen is already physically tilted. **Do not** pass real gameplay gravity
// here; that would tilt the UI on real hardware.
//
// Convention (ported from the pre-T38 PlayerRender composite, kept here so the
// two never drift): axis = (−tiltRad.y, tiltRad.x)/|tiltRad|, angle = |tiltRad|.
// Not constexpr (sin/cos + a 4-corner fit pass), same caveat as frameRotated.
inline la::float4x4 tilt(float aspect, la::float2 tiltRad) {
    const float angle = std::sqrt(tiltRad.x * tiltRad.x + tiltRad.y * tiltRad.y);
    if (angle < 1e-4f || aspect <= 0.0f) {
        // No tilt → identity (base projection passes through untouched).
        return {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    }
    const float axX = -tiltRad.y / angle;
    const float axY =  tiltRad.x / angle;

    // Rodrigues rotation about the in-plane axis (axX, axY, 0).
    const float c = std::cos(angle), s = std::sin(angle), C = 1.0f - c;
    const float R00 = c + axX * axX * C;
    const float R01 = axX * axY * C;          // == R10
    const float R11 = c + axY * axY * C;
    const float R20 = -axY * s;               // rz coefficient of local-x
    const float R21 =  axX * s;               // rz coefficient of local-y

    constexpr float D = 2.0f;                 // camera distance (matches fovY)

    // Pre-fit clip coefficients. Local space has halfW = aspect, halfH = 1, so
    // local-x = aspect·x. clipX = (D/aspect)·rx, clipY = D·ry, clipW = D − rz.
    const float Xx = D * R00,            Xy = (D / aspect) * R01;
    const float Yx = D * aspect * R01,   Yy = D * R11;
    const float Wx = -aspect * R20,      Wy = -R21,           Wc = D;

    // Bounding-box fit: project the four clip-quad corners (±1, ±1), measure
    // the NDC bbox, then scale+translate so it fills [-1,1]² (ndc' = ndc·fitS +
    // fitT). Folded into the matrix below: scaling clip.xy and adding fitT·clip.w
    // before the w-divide is algebraically the post-divide ndc·fitS + fitT.
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& p : corners) {
        const float cw = Wx * p[0] + Wy * p[1] + Wc;
        if (cw < 0.01f) continue;             // corner behind camera — skip
        const float nx = (Xx * p[0] + Xy * p[1]) / cw;
        const float ny = (Yx * p[0] + Yy * p[1]) / cw;
        minX = std::min(minX, nx); maxX = std::max(maxX, nx);
        minY = std::min(minY, ny); maxY = std::max(maxY, ny);
    }
    float fitS = 1.0f, fitTx = 0.0f, fitTy = 0.0f;
    if (maxX > minX && maxY > minY) {
        fitS  = std::min(2.0f / (maxX - minX), 2.0f / (maxY - minY));
        fitTx = -fitS * (minX + maxX) * 0.5f;
        fitTy = -fitS * (minY + maxY) * 0.5f;
    }

    // Column-major (linalg): each brace is a column; result[row] = Σ col[c][row]·v[c].
    // clip.z is dropped (this is a 2D tilt; ge's swap chain is depth-NONE).
    return {{ fitS * Xx + fitTx * Wx, fitS * Yx + fitTy * Wx, 0.0f, Wx },
            { fitS * Xy + fitTx * Wy, fitS * Yy + fitTy * Wy, 0.0f, Wy },
            { 0.0f,                   0.0f,                   0.0f, 0.0f },
            { fitTx * Wc,             fitTy * Wc,             0.0f, Wc }};
}

} // namespace ge::ortho
