// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ge/Linalg.h>
#include <ge/SessionHost.h>  // ge::Rect

#include <cmath>

namespace ge {

// A `Rect` as a 2D coordinate frame: origin at (rect.x, rect.y), x-basis
// = (rect.w, 0), y-basis = (0, rect.h). Maps the unit square (0..1)² in
// local space to the rect in parent space.
//
// Use as a model-to-parent transform — the float4x4 feeds a draw's MVP uniform.
// Apply `la::inverse(frame(r))` to map parent points back into local
// (unit-square) space; useful for hit testing.
//
// Mapping between two arbitrary rects is just composition. Use
// `la::mul` for matrix multiplication — linalg deprecates `operator*`
// between matrices (it does component-wise multiplication, not what
// you want):
//
//     auto m_a_to_b = la::mul(frame(b), la::inverse(frame(a)));
//
// `Rect.w` and `Rect.h` may be negative — that is how a caller in a y-up
// parent space flips the y basis so the local y=0 edge lines up with the
// rect's top in the parent's orientation. No separate y-up / y-down API.
//
// The z basis is identity (z passes through unchanged). The 2D-rect view
// is recovered by the common case where local-space z = 0; the identity
// z keeps the matrix invertible (det = w * h) so `la::inverse(frame(r))`
// is well-defined for hit-testing.
constexpr la::float4x4 frame(Rect r) {
    return {
        { r.w, 0.f, 0.f, 0.f },   // col 0: x basis
        { 0.f, r.h, 0.f, 0.f },   // col 1: y basis
        { 0.f, 0.f, 1.f, 0.f },   // col 2: z basis (identity)
        { r.x, r.y, 0.f, 1.f },   // col 3: origin
    };
}

// Sister to `frame(Rect)` parameterised by center + size instead of
// origin + size. Equivalent to `frame(Rect::centered(center, size))`.
// Use when call sites have a logical center and a physical size in
// hand (sprites placed by their geometric centre rather than their
// top-left corner).
constexpr la::float4x4 frameCentered(la::float2 center, la::float2 size) {
    return frame(Rect::centered(center, size));
}

// Frame for a rect of `size` centered at `center`, rotated by `angle`
// radians around `center`. Positive angle = standard 2D-math
// convention (CCW when y-axis points up; CW when y-axis points down,
// which is the on-screen case for top-left-origin renderers).
//
// At angle = 0 this matches `frameCentered(center, size)` exactly.
// At angle != 0 the matrix's basis vectors are rotated; the local
// (0..1)² unit square maps to a rotated rectangle in world space.
//
// Non-constexpr because `std::sin` / `std::cos` aren't constexpr
// before C++26 — same caveat as `DampedRotation::matrix()`.
inline la::float4x4 frameRotated(la::float2 center, la::float2 size, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float w = size.x;
    const float h = size.y;
    // Composed: T(center) * R(angle) * S(w, h) * T(-0.5, -0.5).
    // Local point (lx, ly) maps to:
    //   world = center + R · ((lx - 0.5) · w, (ly - 0.5) · h)
    return {
        {  c * w,  s * w, 0.f, 0.f },                                     // x basis
        { -s * h,  c * h, 0.f, 0.f },                                     // y basis
        {  0.f,    0.f,   1.f, 0.f },                                     // z basis
        { center.x - 0.5f * (c * w - s * h),
          center.y - 0.5f * (s * w + c * h),
          0.f, 1.f },                                                      // origin
    };
}

} // namespace ge
