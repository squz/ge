// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/button.h>

namespace ge {

bool Button::handleEvent(const PointerEvent& ev) {
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
}

void Button::cancel() {
    if (phase == Idle) return;
    const bool wasHighlighted = (phase == PressedInside);
    phase = Idle;
    activeId = 0;
    if (wasHighlighted && onHighlightChange) onHighlightChange(false);
}

bool ButtonGroup::handleEvent(const PointerEvent& ev) {
    if (active) {
        active->handleEvent(ev);
        if (!active->tracking()) active = nullptr;
        return true;  // while locked, the group claims every event
    }
    for (auto* b : buttons) {
        if (b->handleEvent(ev)) {
            if (b->tracking()) active = b;
            return true;
        }
    }
    return false;
}

} // namespace ge
