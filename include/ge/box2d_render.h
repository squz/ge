// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T131.2 — box2d → render-on-demand trigger. Header-only on purpose: libge
// stays physics-agnostic (it never links box2d — box2d is APP_LIBS), so only
// apps that already use box2d pull this in. Companion to <ge/box2d_slice.h>.
//
// A physics game is the textbook render-on-demand consumer: render every frame
// while the simulation is moving, idle once it settles. box2d already computes
// exactly that signal — per-island sleep — so "render while the world is awake"
// is the right trigger. Crucially it's velocity-thresholded, so it's
// noise-immune: unlike diffing body positions (which would chase sub-pixel
// jitter forever), an asleep world reliably lets the loop idle.
#pragma once

#include <box2d/box2d.h>

#include <ge/SessionHost.h>  // ge::Context

namespace ge::box2d {

// Register a render trigger that keeps the run loop drawing while `world` has
// any awake body, and lets it idle once the whole simulation sleeps. Call once
// per session (e.g. in your factory) alongside opting into render-on-demand:
//
//     ctx.setContinuousRendering(false);
//     ge::box2d::renderWhileAwake(ctx, state.worldId);
//
// A tilt / touch that wakes a body resumes rendering on the next frame; when the
// buggy settles and every island sleeps, the loop idles at ~0% GPU. For "only
// while a certain screen is active", register your own predicate via
// Context::addRenderTrigger gating on both — e.g.
// `[&]{ return playing && b2World_GetAwakeBodyCount(world) > 0; }`.
inline void renderWhileAwake(const ge::Context& ctx, b2WorldId world) {
    ctx.addRenderTrigger([world] { return b2World_GetAwakeBodyCount(world) > 0; });
}

}  // namespace ge::box2d
