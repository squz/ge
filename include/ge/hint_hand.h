// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 Default gesture-hint renderer — the SDF outline hand.
// 🎯T180 Pointing-hand articulation — wrist pivot only (no bodily pan).
//
// One call per frame draws ge's built-in white-fill / black-outline /
// sticker-halo cartoon hand for a playing `ge::hint::Player`. The hand
// is re-derived from the gesture skeleton every frame in a fragment
// shader (smooth-min capsule union), so fingers blend organically into
// the palm, motion is continuous at any display rate, and the art is
// resolution-independent — no textures, no atlas.
//
// Games that want their own aesthetic skip this header entirely and
// consume `Player::pointers()` + tags directly (the data-only tier).
//
// Usage (inside onRender, after `auto p = c.swapchainPass();`):
//
//     hintPlayer.update(dt);                       // in onUpdate
//     ge::hint::drawHand(c, hintPlayer, c.uiSafeRectInPts());
//
// `areaPts` maps the clip's unit gesture space onto a screen rect in
// point space; pass the rect your gameplay targets live in so
// Params.from / Params.to line up with real UI.
//
// Look parameters (hand size, outline / halo / stroke widths, smooth-
// min blend, interior-stroke fade) and articulation knobs
// (`hint.artic_*`) are tweak-tunable under "hint.*".
// One pinch-style hand (index + thumb) is drawn when the player has
// two pointers; the pointing hand otherwise. Draws nothing while the
// hint is fully faded (loop gap), so render-on-demand apps can gate
// redraws on `player.active()`.

#pragma once

#include <ge/SessionHost.h>  // ge::Context, ge::Rect
#include <ge/hint.h>

#include <vector>

namespace ge::hint {

// ── 🎯T180 Pure pointing-hand layout (testable without GPU) ─────────

struct Capsule {
    la::float2 a{}, b{};
    float      radius = 0;
    int        chain  = 0;  // 0 = palm, 1..5 = digit
};

// Articulation knobs as fractions of hand size.
//
// All positions are **screen points** (UI pts) — the same space as
// real touches after unit→areaPts mapping. Touches never live in
// 3D/world space; the hand pivots on the glass.
//
// Palm anchors at the rest wrist (from `restTip`); tip residual aims
// the silhouette by wrist pivot only (no pan of the whole hand).
// `fingerMax` is the soft-start radius (fraction of S). softReach /
// hardReach are kept for ABI/tweak compatibility but ignored.
struct PointingLayoutParams {
    float softReach = 0.08f;   // unused (legacy; no bodily follow)
    float hardReach = 0.65f;   // unused (legacy; no bodily follow)
    // Soft-start radius: tip residual below this is mostly finger stretch
    // (palm stays); above it the wrist pivot ramps to full aim.
    float fingerMax = 0.22f;
};

struct PointingLayout {
    std::vector<Capsule> capsules;
    la::float2           tip{};            // always equals the supplied tip (pts)
    la::float2           palmCentroid{};   // pts
    float                wristAngle = 0;   // radians; 0 = no pivot
};

// Place a pointing hand whose contact tip is exactly `tip` in **screen
// pts** (same value InputDriver injects after unit→areaPts mapping).
//
// `restTip` is the screen-space rest contact (where the tip sat when
// the finger was idle). It does **not** drift with the pointer — the
// wrist stays put on the glass; motion is pivot only. Pass the previous
// frame's rest for continuous play; pass `tip` for a rigid first sample.
// When `outRestTip` is non-null it receives the same rest (unchanged).
//
// |tip − restTip| aims the silhouette about the fixed screen wrist so
// the gesture reads as a finger on glass, never a panning cursor.
PointingLayout layoutPointingHand(la::float2 tip,
                                  float handSize,
                                  float press,
                                  la::float2 restTip,
                                  la::float2* outRestTip = nullptr,
                                  PointingLayoutParams params = {});

// Draw the default hand for `player` into the active render pass.
// No-op if sokol is not ready; GPU resources are created lazily on
// first use. The frame's swapchain (or offscreen) pass must be open.
void drawHand(const Context& ctx, const Player& player, const Rect& areaPts);

}  // namespace ge::hint
