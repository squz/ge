// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T134 On-demand rendering must idle a static screen even while a sensor
// (accelerometer) stream is live: sensor samples don't force a redraw, but real
// user input + structural events do. The host (DirectRenderHost::pumpEvents)
// gates its redraw-mark on this predicate.

#include <doctest.h>

#include "render/RenderOnDemand.h"

using ge::detail::eventResumesRendering;

namespace {
SDL_Event ev(SDL_EventType t) {
    SDL_Event e{};
    e.type = t;
    return e;
}
}  // namespace

TEST_CASE("a continuous sensor stream does NOT resume on-demand rendering") {
    CHECK_FALSE(eventResumesRendering(ev(SDL_EVENT_SENSOR_UPDATE)));
}

TEST_CASE("user input + structural events DO resume rendering") {
    CHECK(eventResumesRendering(ev(SDL_EVENT_FINGER_DOWN)));
    CHECK(eventResumesRendering(ev(SDL_EVENT_FINGER_UP)));
    CHECK(eventResumesRendering(ev(SDL_EVENT_FINGER_MOTION)));
    CHECK(eventResumesRendering(ev(SDL_EVENT_MOUSE_BUTTON_DOWN)));
    CHECK(eventResumesRendering(ev(SDL_EVENT_MOUSE_MOTION)));
    CHECK(eventResumesRendering(ev(SDL_EVENT_KEY_DOWN)));
    CHECK(eventResumesRendering(ev(SDL_EVENT_WINDOW_RESIZED)));
    CHECK(eventResumesRendering(ev(SDL_EVENT_DID_ENTER_FOREGROUND)));
}
