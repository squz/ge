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
#include <string_view>

namespace ge::debug {

// Packed 0xAABBGGRR (matches ge::SpriteVertex::abgr). Defaults: opaque green
// for lines / wireframes, translucent green for fills ("sector tinting"),
// white for text.
inline constexpr uint32_t kLineColor = 0xFF00FF00u;
inline constexpr uint32_t kFillColor = 0x4000FF00u;
inline constexpr uint32_t kTextColor = 0xFFFFFFFFu;

// Runtime on/off. The first query latches the GE_DEBUG_OVERLAY env var
// (1/true/yes/on, case-insensitive → enabled), else disabled. setEnabled
// overrides it for the rest of the process.
bool enabled();
void setEnabled(bool on);

// ── ad-hoc primitives, world space (transformed by flush's worldToClip) ──
void line(la::float3 a, la::float3 b, uint32_t abgr = kLineColor);
void line(la::float2 a, la::float2 b, uint32_t abgr = kLineColor);
void tri(la::float3 a, la::float3 b, la::float3 c, uint32_t abgr = kFillColor);
void tri(la::float2 a, la::float2 b, la::float2 c, uint32_t abgr = kFillColor);

// ── submit-mesh convention ───────────────────────────────────────────────
// Hand the same indexed triangle mesh you draw with your own pipeline; while
// the overlay is enabled, ge draws it on top — as a wireframe (every triangle
// edge; the default) or, with fill = true, a translucent fill for "sector
// tinting". No-op while disabled — adopting the convention is one extra call,
// and the flag does the rest. indexCount should be a multiple of 3; a
// degenerate tail and indices out of range for vertCount are skipped with a
// warning. `abgr` defaults to 0, a sentinel meaning "pick by fill": opaque
// green for a wireframe, translucent green for a fill.
void mesh(const la::float2* verts, int vertCount,
          const uint16_t* indices, int indexCount,
          bool fill = false, uint32_t abgr = 0);
void mesh(const la::float3* verts, int vertCount,
          const uint16_t* indices, int indexCount,
          bool fill = false, uint32_t abgr = 0);

// ── text, screen space (top-left origin, +Y down, framebuffer pixels) ────
// Anchored at posPx; independent of worldToClip. Single line, monospace.
void text(la::float2 posPx, std::string_view str, uint32_t abgr = kTextColor);

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
