// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// RefreshRateBoost — engine-internal press-tracking counter that
// requests max display refresh rate while any touch / mouse-button
// press is in flight.
//
// Design (🎯T63):
//   The engine maintains a reference count of "in-flight presses".
//   Down event → engagePress()  — counter++; if 0→1, request boost.
//   Up / Cancel → releasePress() — counter--; if 1→0, release boost.
//   If counter would underflow, clamp to 0 and log a warning.
//
// Platform behaviour:
//   iOS/iPadOS — holds a CADisplayLink at high preferredFrameRateRange
//                (min=80, preferred=max) while boost is engaged. This
//                keeps the display in its ProMotion refresh mode and
//                ensures the rendered frame produced by the Down event
//                is delivered within ≤1 vsync.
//   Android    — calls GeActivity.setFrameRateBoost(true/false) via JNI
//                which forwards to Surface.setFrameRate(max, …) (API 30+;
//                no-op on older devices).
//   Desktop    — no-op. Desktop displays do not VRR-throttle on idle.
//
// API is intentionally engine-internal (not in the public ge:: surface).
// DirectRenderHost owns one RefreshRateBoost and calls engage/release from
// its SDL event handling path — no consumer code change needed.
#pragma once

#include <memory>

namespace ge {

class RefreshRateBoost {
public:
    RefreshRateBoost();
    ~RefreshRateBoost();

    RefreshRateBoost(const RefreshRateBoost&) = delete;
    RefreshRateBoost& operator=(const RefreshRateBoost&) = delete;

    // Call on every SDL pointer Down event.
    // If the counter transitions 0→1, the platform boost is engaged.
    void engagePress();

    // Call on every SDL pointer Up / Cancel event.
    // If the counter transitions 1→0, the platform boost is released.
    // Spurious extra releases (counter at 0) are clamped and logged once.
    void releasePress();

    // Drain the press counter to zero and release boost.
    // Called on session end / app background to avoid leaving the display
    // pinned at max refresh with no fingers down.
    void drainPresses();

    // Counter value — for diagnostics and tests.
    int pressCount() const noexcept;

private:
    // Platform-specific implementation struct (pImpl).
    struct M;
    std::unique_ptr<M> m_;
};

} // namespace ge
