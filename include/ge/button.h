// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Standard touch/click button interactor and grouping primitive.
//
// `ge::Button` implements iOS-style press semantics: tap down inside
// the hit region highlights, drag outside un-highlights, drag back in
// re-highlights, release inside fires, release outside or external
// cancel doesn't. A Move that lands inside while the button is idle
// captures the touch too (drift-in capture, 🎯T62) — same effect a
// Down inside would have had, so the user can roll a finger onto a
// tight button from outside. Rendering-agnostic — the consumer supplies
// a hit-test predicate (rect, SVG element, polygon, anything) and
// queries `highlighted()` from the render loop, or wires an
// `onHighlightChange` callback for one-shot side effects.
//
// `ge::ButtonGroup` collects several buttons and routes pointer
// events through them with "first match wins, subsequent fingers
// ignored" semantics: once a button starts tracking a touch, all
// pointer events go to it until it returns to idle.
//
// Single-touch by design — once a press is in flight, additional
// fingers are dropped on the floor. Adequate for game UI today; if
// multi-touch ever matters, revisit then.

#pragma once

#include <ge/Linalg.h>
#include <ge/SessionHost.h>  // ge::Rect

#include <SDL3/SDL_touch.h>  // SDL_FingerID

#include <functional>
#include <vector>

namespace ge {

// Pre-converted pointer event in the consumer's coordinate space.
// Consumer converts from SDL_Event once at the dispatch site (mouse
// = pixel coords directly, touch = normalized × surface size); the
// button interactor operates on the result.
struct PointerEvent {
    enum Kind { Down, Move, Up };

    Kind         kind;
    la::float2   pos;
    // Touch ID. For touch events, copy from SDL_TouchFingerEvent::fingerID.
    // For mouse events, use `kMouseId` so the state machine treats
    // mouse as a single virtual finger.
    SDL_FingerID id;
};

// Sentinel finger ID for synthetic "mouse is a single finger" events.
// SDL touch IDs are device-specific and never collide with this value.
inline constexpr SDL_FingerID kMouseId = SDL_FingerID(~0ULL);

// Standard press-button interactor.
struct Button {
    using HitTest           = std::function<bool(la::float2)>;
    using FireCallback      = std::function<void()>;
    using HighlightCallback = std::function<void(bool highlighted)>;

    HitTest             hitTest           = {};
    FireCallback        onFire            = {};
    HighlightCallback   onHighlightChange = {};

    // Internal state. Public so designated init still works; the
    // consumer shouldn't reach in directly.
    enum Phase { Idle, PressedInside, PressedOutside };
    Phase        phase    = Idle;
    SDL_FingerID activeId = 0;  // valid only when phase != Idle

    // Pump a pointer event through the state machine. Returns true
    // if the event was consumed by this button (caller stops
    // dispatching to other buttons / handlers).
    bool handleEvent(const PointerEvent& ev);

    // For the render loop — true iff the user is pressing and the
    // pointer is currently inside the hit region.
    bool highlighted() const { return phase == PressedInside; }

    // True iff a press is in flight (inside or outside). Use to
    // maintain an "active button" lock at the dispatcher level.
    bool tracking() const { return phase != Idle; }

    // Force back to idle. Use for screen transitions or removing
    // the button mid-touch. Fires `onHighlightChange(false)` if
    // currently highlighted; does not fire `onFire`.
    void cancel();
};

// Routes pointer events through a list of buttons, maintaining a
// single-button lock for the duration of any press. Once one button
// claims a touch, all subsequent events for that touch (and other
// fingers) go to it until it returns to idle. Borrows its buttons —
// each `Button*` typically points at a struct member of the screen
// that owns the group.
struct ButtonGroup {
    std::vector<Button*>  buttons;
    Button*               active = nullptr;

    bool handleEvent(const PointerEvent& ev);
};

// Convenience: rect-based hit test. Captures the rect by value, so
// the hit region is frozen at the time of the call. For buttons
// whose geometry changes per-frame, reassign `btn.hitTest =
// ge::rectHitTest(currentRect)` each frame, or write the lambda
// inline to close over a live source.
inline Button::HitTest rectHitTest(Rect r) {
    return [r](la::float2 p) { return r.contains(p); };
}

} // namespace ge
