// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Long-press gesture detector (🎯T65.6).
//
// Watches `ge::PointerEvent`s in a hit region; fires `onFire` when a
// single finger has been held inside the region for at least
// `thresholdSec` seconds without lifting and without drifting outside.
//
// Designed for "secret" / debug triggers — e.g. "long-press the
// top-right corner of a debug build to open the IAP entitlement panel".
// Single-touch by design; additional fingers are ignored while a press
// is in flight, mirroring the `ge::Button` discipline.
//
// Wiring (consumer-side):
//
//     ge::LongPressWatcher trigger{
//         .region = topRightCorner,
//         .onFire = [&]{ debugPanel.toggle(); },
//     };
//     // In onEvent(SDL_Event):
//     if (auto pe = toPointerEvent(ev)) trigger.handleEvent(*pe);
//     // In onUpdate(dt):
//     trigger.update(dt);
//
// The watcher is rect-based for simplicity. For non-rectangular hit
// regions (e.g. lunasvg-rendered chrome), set `region` to a bounding
// rect and gate `onFire` with a more precise test if needed.

#pragma once

#include <ge/button.h>          // PointerEvent, kMouseId
#include <ge/SessionHost.h>     // Rect

#include <functional>

namespace ge {

struct LongPressWatcher {
    Rect                       region        = {};
    float                      thresholdSec  = 1.0f;
    std::function<void()>      onFire        = {};

    // Internal state (public so designated init still works; consumer
    // shouldn't reach in directly).
    bool         tracking = false;
    SDL_FingerID activeId = 0;
    float        elapsed  = 0.0f;
    bool         fired    = false;  // latched after onFire to suppress refires

    // Pump a pointer event through the watcher. Returns true if the
    // event was consumed (a press is in flight inside the region).
    // Idle events outside the region return false so other handlers
    // can claim them.
    bool handleEvent(const PointerEvent& ev);

    // Advance the timer. Must be called each frame with the frame's
    // delta time. Fires `onFire` exactly once per press if the
    // tracked finger has been held for ≥ `thresholdSec`. Subsequent
    // update() calls during the same press are no-ops until the
    // finger lifts.
    void update(float dt);

    // Force back to idle (e.g. on a panel-close action that should
    // drop the trigger). Does not fire `onFire`.
    void cancel();
};

} // namespace ge
