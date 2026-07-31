// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 Default gesture-hint renderer — the SDF outline hand.
// 🎯T180 Pointing-hand articulation (finger/wrist vs bodily translate).
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

// Articulation thresholds as fractions of hand size.
// soft→hard: continuous blend from finger/wrist-dominated to bodily follow.
// fingerMax: residual reach before the hand pivots about the wrist.
struct PointingLayoutParams {
    float softReach = 0.10f;
    float hardReach = 0.55f;
    float fingerMax = 0.22f;
};

struct PointingLayout {
    std::vector<Capsule> capsules;
    la::float2           tip{};            // always equals the supplied tip
    la::float2           palmCentroid{};
    float                wristAngle = 0;   // radians; 0 = no pivot
};

// Place a pointing hand whose contact tip is exactly `tip` (same value
// InputDriver injects as PointerState.pos, after the game maps unit→pts).
//
// `bodyHome` is the world-space rest attachment (where the tip sits when
// the finger is at rest length). Pass the previous frame's updated home
// for continuous articulation; pass `tip` for a rigid first sample.
// When `outBodyHome` is non-null it receives the home for the next frame.
//
// Small |tip − bodyHome|: palm stays put, finger/wrist reach.
// Large offset: palm translates with the tip (bodily regime).
// Past fingerMax: wrist pivots rather than panning the silhouette.
PointingLayout layoutPointingHand(la::float2 tip,
                                  float handSize,
                                  float press,
                                  la::float2 bodyHome,
                                  la::float2* outBodyHome = nullptr,
                                  PointingLayoutParams params = {});

// Draw the default hand for `player` into the active render pass.
// No-op if sokol is not ready; GPU resources are created lazily on
// first use. The frame's swapchain (or offscreen) pass must be open.
void drawHand(const Context& ctx, const Player& player, const Rect& areaPts);

}  // namespace ge::hint
