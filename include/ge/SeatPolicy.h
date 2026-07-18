// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Primary-seat policy for stream multi-attach (🎯T156.3).
// First attached remote device is the primary seat; spectators may receive
// content fan-out but must not update DeviceInfo/content surface or inject
// sensor samples into the single-world game virtual device.

#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace ge {

// Opaque wire identity (shared_ptr address is stable for a connection).
using SeatId = const void*;

// SeatPolicy is pure logic — unit-tested without websockets.
class SeatPolicy {
public:
    void onAttach(SeatId id) {
        if (!primary_) primary_ = id;
    }

    void onDetachAll() { primary_ = nullptr; }

    SeatId primary() const { return primary_; }

    bool isPrimary(SeatId id) const {
        return primary_ != nullptr && id == primary_;
    }

    // DeviceInfo / SafeArea: only primary retargets content surface.
    bool acceptDeviceInfo(SeatId id) const { return isPrimary(id); }
    bool acceptSafeArea(SeatId id) const { return isPrimary(id); }

    // SP2I SDL payloads: drop sensor samples from non-primary seats.
    // Other input (mouse/finger/key) from non-primary is also dropped for
    // single-world games so seats cannot steal AccelSynth control.
    bool acceptSdlEvent(SeatId id, const SDL_Event& ev) const {
        if (isPrimary(id)) return true;
        // Spectators: no input, no sensors.
        (void)ev;
        return false;
    }

    // For tests: classify a raw SP2I-shaped buffer (header + SDL_Event).
    static bool isSensorEvent(const SDL_Event& ev) {
        return ev.type == SDL_EVENT_SENSOR_UPDATE;
    }

private:
    SeatId primary_ = nullptr;
};

} // namespace ge
