// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::debug — an opt-in, flag-toggleable debug-render overlay (🎯T97).
//
// Accumulate diagnostic primitives anywhere in your frame, then flush once
// after your scene. While the overlay is disabled (the default off a fresh
// process unless the GE_DEBUG_OVERLAY env var is set), every accumulation call
// below is a cheap no-op — so call sites stay unconditional and toggling
// enabled() at runtime lights the whole layer up without touching your draw
// code. That is the "submit-mesh convention": hand mesh() the same buffers you
// already draw with, and the flag does the rest.
//
// The public surface deliberately names no rendering backend: coordinates are
// plain ge::la vectors, colours are ge::Color (straight-alpha RGBA float4), and
// flush() draws into the active render pass the same way ge::Sprite::draw does.
#pragma once

#include <ge/Linalg.h>
#include <ge/SessionHost.h>   // ge::Context

#include <cstdint>
#include <span>
#include <string_view>

namespace ge::debug {

// `ge::Color` defaults (straight-alpha RGBA in [0, 1]): opaque magenta for
// lines / wireframes, translucent magenta for fills ("sector tinting"), white
// for text. A colour with alpha 0 is "absent" — mesh() skips that layer.
inline constexpr Color kWireColor{1.0f, 0.0f, 1.0f, 1.0f};
inline constexpr Color kFillColor{1.0f, 0.0f, 1.0f, 0.25f};
inline constexpr Color kTextColor{1.0f, 1.0f, 1.0f, 1.0f};

// On-screen side length of a point() marker, in points (pt) — a fixed
// perceptual size, projected device-independently (pt, not px) like text.
inline constexpr float kPointSizePt = 4.0f;

// Runtime on/off. The first query latches the GE_DEBUG_OVERLAY env var
// (1/true/yes/on, case-insensitive → enabled), else disabled. setEnabled
// overrides it for the rest of the process.
bool enabled();
void setEnabled(bool on);

// ── ad-hoc primitives, world space (transformed by flush's worldToClip) ──
void line(la::float3 a, la::float3 b, Color color = kWireColor);
void line(la::float2 a, la::float2 b, Color color = kWireColor);
void tri(la::float3 a, la::float3 b, la::float3 c, Color color = kFillColor);
void tri(la::float2 a, la::float2 b, la::float2 c, Color color = kFillColor);

// ── submit-mesh convention ───────────────────────────────────────────────
// Hand the same indexed triangle mesh you draw with your own pipeline (any
// contiguous range — vector, array, C-array — converts to the span); while the
// overlay is enabled, ge draws it on top: a wireframe (every triangle edge) in
// `wireColor` and/or a translucent fill ("sector tinting") in `fillColor`. A
// colour with alpha 0 skips that layer, so the default is a magenta wireframe
// with no fill — pass a fillColor for a tint, or both for filled-and-outlined
// (the fill draws under the wire). No-op while disabled. indices.size() should
// be a multiple of 3; a degenerate tail and indices out of range for verts are
// skipped with a warning.
void mesh(std::span<const la::float2> verts, std::span<const uint16_t> indices,
          Color wireColor = kWireColor, Color fillColor = {});
void mesh(std::span<const la::float3> verts, std::span<const uint16_t> indices,
          Color wireColor = kWireColor, Color fillColor = {});

// ── convenience shapes ───────────────────────────────────────────────────
// Built from line() / tri(), same two-colour convention as mesh(): a colour
// with alpha 0 skips that layer (default = magenta outline, no fill). `box` is
// an axis-aligned ge::Rect in the z = 0 plane; `circle` is a segments-gon
// centred at `center` (triangle-fan fill, perimeter-segment outline).
void box(Rect rect, Color wireColor = kWireColor, Color fillColor = {});
void circle(la::float2 center, float radius,
            Color wireColor = kWireColor, Color fillColor = {}, int segments = 32);

// `point` marks an exact location — a contact point, a sampled position, a
// graph node — that box() / circle() only imply via their centre. Unlike those
// (which are world-space and so grow/shrink with zoom), a point is a *fixed
// perceptual size*: a small filled square `kPointSizePt` points on a side,
// centred on `pos` and held constant on screen however far worldToClip zooms
// out — so a cloud of points stays legible instead of collapsing to nothing.
// It is expanded to a screen-space quad at flush() (where the projection and
// surface size are known), the same way text() is. `pos` is projected through
// worldToClip; the 3D overload lets a point ride a perspective scene. Single
// layer (fill only) — one Color, default opaque magenta; alpha 0 is a no-op.
// For a *sized* dot that scales with the world, use circle() instead.
void point(la::float2 pos, Color color = kWireColor);
void point(la::float3 pos, Color color = kWireColor);

// ── text, screen space (top-left origin, +Y down, framebuffer pixels) ────
// Anchored at posPx; independent of worldToClip. Single line, monospace.
void text(la::float2 posPx, std::string_view str, Color color = kTextColor);

// ── flush ────────────────────────────────────────────────────────────────
// Draw everything accumulated since the last flush into the active render
// pass, then clear. `worldToClip` transforms line / tri / mesh coords to clip
// space; `ctx` supplies the surface size for projecting pixel-space text. Call
// once per frame after your scene. A no-op (bar clearing) while disabled or
// when nothing was queued — GPU resources are created lazily on first use.
void flush(const Context& ctx, const la::float4x4& worldToClip);

// Discard queued primitives without drawing (e.g. a frame you skip).
void clear();

// Test introspection — queued counts since the last flush / clear. Not part
// of the rendering contract; used by debug_test.cpp to verify accumulation
// without a GPU.
namespace testing {
int lineVertexCount();
int triVertexCount();
int textItemCount();
int pointItemCount();
} // namespace testing

} // namespace ge::debug
