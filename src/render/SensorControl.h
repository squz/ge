// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Fine-grained accelerometer authority for app-channel / host scripts.
// Default is passthrough (real device sensors win). Scripts opt into override
// or mute explicitly per sensor — never blanket for the whole session.

#pragma once

#include <cstdint>

namespace ge::detail {

// Per-sensor stream modes. Only "accel" is implemented today; the API is
// shaped so gyro/etc. can join later without a second control plane.
enum class SensorStreamMode : std::uint8_t {
    Passthrough = 0, // real hardware/synth samples flow; inject is one-shot
    Override    = 1, // drop real samples; only script latch / inject applies
    Mute        = 2, // drop real samples; emit neutral (0,0,0) each frame
};

// SDL_SensorEvent.which for samples produced by app-channel inject / latch.
// Real open sensors never use this id, so pumpEvents can tell them apart.
inline constexpr std::uint32_t kSyntheticAccelWhich = 0x53504F56u; // 'SPOV'

void setAccelStreamMode(SensorStreamMode mode);
SensorStreamMode accelStreamMode();

// Latched device-frame sample used while mode == Override.
// Also updated by input_inject type=accel when override is active.
void setAccelLatch(float x, float y, float z);
bool accelLatch(float& x, float& y, float& z); // false if never set this session

// Reset to factory defaults (passthrough, clear latch). For tests / detach.
void resetSensorControl();

} // namespace ge::detail
