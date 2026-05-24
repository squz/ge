// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/sdl_input.h>

#include <SDL3/SDL_video.h>

namespace ge::input {

// PointerEvent.pos is in point space (🎯T60).
//
// Mouse events: SDL delivers mouse coords in window-point space already.
// No pt→pixel conversion — PointerEvent.pos uses them as-is.
//
// Finger events: SDL delivers (0..1) normalized coords; we denormalize
// against `surfaceSizePts` (ctx.fullRectInPts().size()) to get pt coords.

std::optional<PointerEvent> fromSdl(const SDL_Event& ev, la::float2 surfaceSizePts) {
    switch (ev.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // Drop touch-synthetic mouse events — the consumer already
        // receives the corresponding SDL_EVENT_FINGER_* event.
        if (ev.button.which == SDL_TOUCH_MOUSEID) return std::nullopt;
        // SDL mouse coords are already in window points — use directly.
        return PointerEvent{
            .kind = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                    ? PointerEvent::Down : PointerEvent::Up,
            .pos  = {ev.button.x, ev.button.y},
            .id   = kMouseId,
        };

    case SDL_EVENT_MOUSE_MOTION:
        if (ev.motion.which == SDL_TOUCH_MOUSEID) return std::nullopt;
        return PointerEvent{
            .kind = PointerEvent::Move,
            .pos  = {ev.motion.x, ev.motion.y},
            .id   = kMouseId,
        };

    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
        // Denormalize (0..1) finger coords against surface size in pts.
        return PointerEvent{
            .kind = ev.type == SDL_EVENT_FINGER_DOWN ? PointerEvent::Down
                  : ev.type == SDL_EVENT_FINGER_UP   ? PointerEvent::Up
                                                    : PointerEvent::Move,
            .pos  = {ev.tfinger.x * surfaceSizePts.x,
                     ev.tfinger.y * surfaceSizePts.y},
            .id   = ev.tfinger.fingerID,
        };
    }
    return std::nullopt;
}

} // namespace ge::input
