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
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace ge::debug {

// `ge::Color` defaults (straight-alpha RGBA in [0, 1]): opaque magenta for
// lines / wireframes, translucent magenta for fills ("sector tinting"), white
// for text. A colour with alpha 0 is "absent" — mesh() skips that layer.
inline constexpr Color kWireColor{1.0f, 0.0f, 1.0f, 1.0f};
inline constexpr Color kFillColor{1.0f, 0.0f, 1.0f, 0.25f};
inline constexpr Color kTextColor{1.0f, 1.0f, 1.0f, 1.0f};

// On-screen diameter of a point() marker, in points (pt) — a fixed perceptual
// size, projected device-independently (pt, not px) like text.
inline constexpr float kPointSizePt = 2.0f;

// Default circle() quality: the largest on-screen gap, in points, allowed
// between the drawn polygon and the true circle. Smaller = smoother + costlier.
inline constexpr float kCircleQualityPt = 0.5f;

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
// an axis-aligned ge::Rect in the z = 0 plane.
void box(Rect rect, Color wireColor = kWireColor, Color fillColor = {});

// `circle` is an n-gon centred at `center` (triangle-fan fill, perimeter-
// segment outline), but you don't pick n — you pick a `quality`: the maximum
// on-screen distance, in points, between the polygon and the true circle. The
// engine then chooses just enough vertices to honour it. Because that error is
// perceptual, the vertex count is resolved at flush() against worldToClip, so a
// circle stays smooth as you zoom in and cheap as you zoom out — without the
// caller tracking scale. The polygon is *balanced*: vertices sit ~quality
// outside the circle and edge midpoints ~quality inside, halving the count a
// purely-inscribed polygon would need for the same worst-case error. `minVerts`
// floors the count (e.g. 4 to keep a recognisable diamond when tiny); the hard
// floor is 3. See segmentsForQuality() for the exact policy.
void circle(la::float2 center, float radius, Color wireColor = kWireColor,
            Color fillColor = {}, float quality = kCircleQualityPt,
            int minVerts = 0);

// The tessellation policy circle() applies at flush, exposed pure for tests and
// for callers who tessellate their own arcs. `radiusPt` is the circle's *on-
// screen* radius in points; `qualityPt` the max polygon↔circle deviation in
// points. Returns the vertex count: ceil((π/2)·√(radiusPt/qualityPt)), floored
// at max(3, minVerts) and capped so a pathological zoom can't explode it.
int segmentsForQuality(float radiusPt, float qualityPt, int minVerts = 0);

// `point` marks an exact location — a contact point, a sampled position, a
// graph node — that box() / circle() only imply via their centre. Unlike those
// (which are world-space and so grow/shrink with zoom), a point is a *fixed
// perceptual size*: a small filled disc `kPointSizePt` points across, centred
// on `pos` and held constant on screen however far worldToClip zooms out — so a
// cloud of points stays legible instead of collapsing to nothing. Under the
// hood it's a tiny circle tessellated with at least 8 vertices, so it reads
// round rather than blocky even at its default size. It is expanded to a
// screen-space disc at flush() (where the projection and surface size are
// known), the same way text() is. `pos` is projected through
// worldToClip; the 3D overload lets a point ride a perspective scene. Single
// layer (fill only) — one Color, default opaque magenta; alpha 0 is a no-op.
// For a *sized* dot that scales with the world, use circle() instead.
void point(la::float2 pos, Color color = kWireColor);
void point(la::float3 pos, Color color = kWireColor);

// ── text, screen space (top-left origin, +Y down, framebuffer pixels) ────
// Anchored at posPx; independent of worldToClip. Single line, monospace.
void text(la::float2 posPx, std::string_view str, Color color = kTextColor);

// ── perf HUD strip (🎯T173) ──────────────────────────────────────────────
// A one-line performance strip — engine-owned dt/fps plus optional
// game-supplied segments — drawn by flush() over a translucent backing
// box so it stays legible on bright content. Independent of the debug-
// draw layer: hudEnabled() latches the GE_PERF_HUD env var on first
// query, setHudEnabled() overrides at runtime, and the strip renders
// even while the rest of the overlay is disabled. (While the overlay is
// enabled and the HUD is not, the legacy bare FPS readout still draws.)
//
// Placement is imperative and cheap — plain stores read at the next
// flush(), so a game may reposition every frame as its chrome evolves:
//
//     ge::debug::setHudPlacement(ge::debug::HudAnchor::TopLeft, {8, 8});
//     // or pin it precisely, in point space:
//     ge::debug::setHudPlacement(ge::debug::HudAnchor::Custom,
//                                {panel.x, panel.y1() + 4});
//
// Custom anchors interpret offsetPt as the absolute top-left of the
// strip in point space; corner anchors inset from that corner.
//
// Game segments come from a provider queried once per flush:
//
//     ge::debug::setHudProvider([&] {
//         return fmt::format("zoom {:.2f}  mag {}", cam.zoom, magnet ? "on" : "off");
//     });

enum class HudAnchor { TopLeft, TopRight, BottomLeft, BottomRight, Custom };

bool hudEnabled();
void setHudEnabled(bool on);
void setHudPlacement(HudAnchor anchor, la::float2 offsetPt = {8.0f, 8.0f});
// Empty function clears. Called on the game thread from flush(); keep it
// allocation-light — its string is appended after the engine dt/fps text.
void setHudProvider(std::function<std::string()> provider);

// Pure label composition (exposed for tests): "dt 16.7 ms  60.0 fps",
// plus "  " + extra when extra is non-empty. dtSeconds <= 0 or fps <= 0
// render as "--".
std::string hudLabel(float dtSeconds, float fps, std::string_view extra);

// ── flush ────────────────────────────────────────────────────────────────
// Draw everything accumulated since the last flush into the active render
// pass, then clear. When the overlay is enabled, flush also draws a small
// smoothed FPS readout in the top-right corner. `worldToClip` transforms line /
// tri / mesh coords to clip space; `ctx` supplies the surface size for
// projecting pixel-space text. Call once per frame after your scene. A no-op
// while disabled; while enabled, it still draws FPS even when no other debug
// primitives were queued. GPU resources are created lazily on first use.
void flush(const Context& ctx, const la::float4x4& worldToClip);

// Discard queued primitives without drawing (e.g. a frame you skip).
void clear();

// ── per-session scoping (🎯T174) ─────────────────────────────────────────
// All of the above — the primitive queues AND the perf HUD config — is
// per-session state, carried by the Context. The render host binds the
// session before dispatching game callbacks (and flush(ctx, …) re-binds
// from its Context), so in a multi-session server each stream accumulates
// and renders its own debug content, and free functions need no Context
// parameter. Direct-mode games see no change: one session, bound at
// startup. Calls made before any session exists (unit tests, early init)
// land in a process-default session.
namespace internal {
// Bind `ctx`'s session as the target of the free functions above,
// creating its state lazily. Engine-internal; called by the hosts.
void bindContext(const Context& ctx);
} // namespace internal

// Test introspection — queued counts since the last flush / clear, for the
// currently bound session. Not part of the rendering contract; used by
// debug_test.cpp to verify accumulation without a GPU.
namespace testing {
int lineVertexCount();
int triVertexCount();
int textItemCount();
int pointItemCount();
int circleItemCount();
// 🎯T174 The bound session's HUD provider output ("" if none) — lets a
// multi-session test verify provider isolation without a GPU.
std::string hudProviderText();
// 🎯T174 Re-bind the process-default session (test hygiene after binding
// throwaway Contexts).
void bindProcessDefault();
} // namespace testing

} // namespace ge::debug
