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
// code. That is the "submit-mesh convention": hand wireMesh / fillMesh the same
// buffers you already draw with, and the flag does the rest.
//
// The public surface deliberately names no rendering backend: coordinates are
// plain ge::la vectors, colours are packed ABGR, and flush() draws into the
// active render pass the same way ge::Sprite::draw does.
#pragma once

#include <ge/Linalg.h>
#include <ge/SessionHost.h>   // ge::Context

#include <cstdint>
#include <span>
#include <string_view>

namespace ge::debug {

// Straight-alpha RGBA in [0, 1] (ge::la::float4, same as ge::rasterizeText).
// Defaults: opaque green for lines / wireframes, translucent green for fills
// ("sector tinting"), white for text.
inline constexpr la::float4 kLineColor{0.0f, 1.0f, 0.0f, 1.0f};
inline constexpr la::float4 kFillColor{0.0f, 1.0f, 0.0f, 0.25f};
inline constexpr la::float4 kTextColor{1.0f, 1.0f, 1.0f, 1.0f};

// Runtime on/off. The first query latches the GE_DEBUG_OVERLAY env var
// (1/true/yes/on, case-insensitive → enabled), else disabled. setEnabled
// overrides it for the rest of the process.
bool enabled();
void setEnabled(bool on);

// ── ad-hoc primitives, world space (transformed by flush's worldToClip) ──
void line(la::float3 a, la::float3 b, la::float4 color = kLineColor);
void line(la::float2 a, la::float2 b, la::float4 color = kLineColor);
void tri(la::float3 a, la::float3 b, la::float3 c, la::float4 color = kFillColor);
void tri(la::float2 a, la::float2 b, la::float2 c, la::float4 color = kFillColor);

// ── submit-mesh convention ───────────────────────────────────────────────
// Hand the same indexed triangle mesh you draw with your own pipeline (any
// contiguous range — vector, array, C-array — converts to the span); while the
// overlay is enabled, ge draws it on top as a wireframe (every triangle edge;
// the default) or, with fill = true, a translucent fill for "sector tinting".
// No-op while disabled. indices.size() should be a multiple of 3; a degenerate
// tail and indices out of range for verts are skipped with a warning. `color`
// defaults to {} (transparent), a sentinel meaning "pick by fill": opaque
// green for a wireframe, translucent green for a fill.
void mesh(std::span<const la::float2> verts, std::span<const uint16_t> indices,
          bool fill = false, la::float4 color = {});
void mesh(std::span<const la::float3> verts, std::span<const uint16_t> indices,
          bool fill = false, la::float4 color = {});

// ── text, screen space (top-left origin, +Y down, framebuffer pixels) ────
// Anchored at posPx; independent of worldToClip. Single line, monospace.
void text(la::float2 posPx, std::string_view str, la::float4 color = kTextColor);

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
} // namespace testing

} // namespace ge::debug
