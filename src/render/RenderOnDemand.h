// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T134 Which SDL events resume on-demand rendering.
//
// In on-demand mode (🎯T132, Context::setContinuousRendering(false)) the run
// loop idles until something worth drawing arrives. Discrete user input
// (touch / mouse / key) and structural events (resize / lifecycle) resume
// rendering; a CONTINUOUS sensor stream (accelerometer / gyro, kept open
// session-wide for a tilt-controlled Playing screen) does NOT — it isn't a
// per-frame redraw request, and on a physical device it would otherwise fire
// every sample and defeat the idle entirely (multimaze2 on-device, 🎯T134). A
// consumer that wants sensor-driven redraws opts in by calling
// Context::requestRedraw() from onEvent on the sensor samples it cares about.
#pragma once

#include <SDL3/SDL_events.h>

namespace ge::detail {

inline bool eventResumesRendering(const SDL_Event& e) {
    // A continuous sensor stream is not discrete input demanding a frame.
    return e.type != SDL_EVENT_SENSOR_UPDATE;
}

} // namespace ge::detail
