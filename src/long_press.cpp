// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/long_press.h>

namespace ge {

bool LongPressWatcher::handleEvent(const PointerEvent& ev) {
    const bool inside = region.contains(ev.pos);

    switch (ev.kind) {
    case PointerEvent::Down:
        if (tracking) return false;       // single-touch lock
        if (!inside)  return false;
        tracking = true;
        activeId = ev.id;
        elapsed  = 0.0f;
        fired    = false;
        return true;

    case PointerEvent::Move:
        if (!tracking)              return false;
        if (ev.id != activeId)      return false;
        if (!inside) {
            // Finger drifted out of the region — cancel without firing.
            cancel();
            return true;
        }
        return true;

    case PointerEvent::Up:
        if (!tracking)              return false;
        if (ev.id != activeId)      return false;
        // Up before the threshold = not a long press. Don't fire.
        cancel();
        return true;
    }
    return false;
}

void LongPressWatcher::update(float dt) {
    if (!tracking || fired) return;
    elapsed += dt;
    if (elapsed >= thresholdSec) {
        fired = true;
        if (onFire) onFire();
    }
}

void LongPressWatcher::cancel() {
    tracking = false;
    activeId = 0;
    elapsed  = 0.0f;
    fired    = false;
}

} // namespace ge
