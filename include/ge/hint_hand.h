// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 Default gesture-hint renderer — the SDF outline hand.
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
// min blend, interior-stroke fade) are tweak-tunable under "hint.*".
// One pinch-style hand (index + thumb) is drawn when the player has
// two pointers; the pointing hand otherwise. Draws nothing while the
// hint is fully faded (loop gap), so render-on-demand apps can gate
// redraws on `player.active()`.

#pragma once

#include <ge/SessionHost.h>  // ge::Context, ge::Rect
#include <ge/hint.h>

namespace ge::hint {

// Draw the default hand for `player` into the active render pass.
// No-op if sokol is not ready; GPU resources are created lazily on
// first use. The frame's swapchain (or offscreen) pass must be open.
void drawHand(const Context& ctx, const Player& player, const Rect& areaPts);

}  // namespace ge::hint
