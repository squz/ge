// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/button.h>

namespace ge {

bool Button::handleEvent(const PointerEvent& ev) {
    // 🎯T131.3 Wrap the state machine so any visible-state (phase) transition
    // fires onRedraw exactly once — the single point that guarantees a
    // render-on-demand frame for the depress / release. The IIFE gives the
    // existing switch a single exit without changing its logic.
    const Phase before = phase;
    const bool consumed = [&]() -> bool {
        const bool inside = hitTest ? hitTest(ev.pos) : false;

        switch (ev.kind) {
        case PointerEvent::Down:
            // Drop subsequent presses while already tracking — single-touch
            // by design (see header docs).
            if (phase != Idle) return false;
            if (!inside) return false;
            activeId = ev.id;
            phase = PressedInside;
            if (onHighlightChange) onHighlightChange(true);
            return true;

        case PointerEvent::Move:
            // Drift-in capture (🎯T62): a Move arriving inside while idle
            // captures the touch — same effect as a Down inside. Lets the
            // user roll a finger onto a button from outside, matching
            // UIButton behaviour on iOS.
            if (phase == Idle) {
                if (!inside) return false;
                activeId = ev.id;
                phase = PressedInside;
                if (onHighlightChange) onHighlightChange(true);
                return true;
            }
            // Only the tracked finger drives state transitions; other
            // fingers' motion is ignored.
            if (ev.id != activeId) return false;
            if (inside && phase == PressedOutside) {
                phase = PressedInside;
                if (onHighlightChange) onHighlightChange(true);
            } else if (!inside && phase == PressedInside) {
                phase = PressedOutside;
                if (onHighlightChange) onHighlightChange(false);
            }
            return true;

        case PointerEvent::Up:
            if (phase == Idle) return false;
            if (ev.id != activeId) return false;
            {
                const bool fired = (phase == PressedInside);
                if (phase == PressedInside && onHighlightChange) {
                    onHighlightChange(false);
                }
                phase = Idle;
                activeId = 0;
                if (fired && onFire) onFire();
            }
            return true;
        }
        return false;
    }();

    if (onRedraw && phase != before) onRedraw();
    return consumed;
}

void Button::cancel() {
    if (phase == Idle) return;
    const bool wasHighlighted = (phase == PressedInside);
    phase = Idle;
    activeId = 0;
    if (wasHighlighted && onHighlightChange) onHighlightChange(false);
    if (onRedraw) onRedraw();  // 🎯T131.3 phase left Idle → draw the un-pressed frame
}

bool ButtonGroup::handleEvent(const PointerEvent& ev) {
    // 🎯T131.3 Fire the group's redraw sink on the routed button's visible-state
    // transition. handleEvent returns false only on no-op paths (no phase
    // change), so a change is possible exactly on the consumed path — snapshot
    // that one button's phase, no per-button scan.
    if (active) {
        Button*     b      = active;
        const auto  before = b->phase;
        b->handleEvent(ev);
        if (!active->tracking()) active = nullptr;
        if (onRedraw && b->phase != before) onRedraw();
        return true;  // while locked, the group claims every event
    }
    for (auto* b : buttons) {
        const auto before = b->phase;
        if (b->handleEvent(ev)) {
            if (b->tracking()) active = b;
            if (onRedraw && b->phase != before) onRedraw();
            return true;
        }
    }
    return false;
}

} // namespace ge
