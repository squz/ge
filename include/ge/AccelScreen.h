// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Rotate accelerometer samples from device-hardware frame into screen frame.
// Shared by DirectRenderHost (local games) and stream player (spyder).
//
// SDL3 sensor convention (device frame): +X = physical right edge up,
// +Y = physical top edge up, +Z = screen up. Touch/mouse already arrive in
// the rotated UI frame; the accelerometer chip does not, so callers must
// apply this when forwarding SENSOR_UPDATE to game code.
//
// Keyed on the LIVE orientation of the UI (pickScreenOrientation), not
// SessionConfig.orientation (which is only the lock *request*).
#pragma once

#include <SDL3/SDL_video.h>

namespace ge {

// The orientation to frame sensor data by: the platform's own answer for the
// UI when it has one (`platform` is an SDL_DisplayOrientation, UNKNOWN when it
// has none), SDL's display orientation otherwise.
//
// SDL's value is not trusted on iOS. It is the status-bar orientation SDL read
// at init, and it can stay at that: an iPad mini on iPadOS 27.2 running a
// portrait-locked app reported LANDSCAPE with the UI in portrait, so pitch and
// yaw came out swapped (yourworld2, 2026-11-12).
inline SDL_DisplayOrientation pickScreenOrientation(int platform,
                                                    SDL_DisplayOrientation sdl) {
    return platform != SDL_ORIENTATION_UNKNOWN
               ? static_cast<SDL_DisplayOrientation>(platform)
               : sdl;
}

// In-place rotate of d[0], d[1]; d[2] (out-of-screen) is unchanged.
inline void rotateAccelToScreen(SDL_DisplayOrientation orient, float d[/*≥3*/]) {
    const float x = d[0];
    const float y = d[1];
    switch (orient) {
    case SDL_ORIENTATION_LANDSCAPE:
        d[0] = -y;
        d[1] = x;
        break;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
        d[0] = y;
        d[1] = -x;
        break;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED:
        d[0] = -x;
        d[1] = -y;
        break;
    case SDL_ORIENTATION_PORTRAIT:
    case SDL_ORIENTATION_UNKNOWN:
    default:
        break; // identity
    }
}

} // namespace ge
