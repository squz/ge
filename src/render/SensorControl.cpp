// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "SensorControl.h"

#include <atomic>
#include <cmath>

namespace ge::detail {
namespace {

std::atomic<std::uint8_t> g_accelMode{static_cast<std::uint8_t>(SensorStreamMode::Passthrough)};
std::atomic<bool> g_accelLatchValid{false};
// Pack x,y,z as three atomics — rare writes from RPC; reads on game thread.
std::atomic<float> g_accelX{0.f};
std::atomic<float> g_accelY{0.f};
std::atomic<float> g_accelZ{0.f};

} // namespace

void setAccelStreamMode(SensorStreamMode mode) {
    g_accelMode.store(static_cast<std::uint8_t>(mode), std::memory_order_release);
}

SensorStreamMode accelStreamMode() {
    return static_cast<SensorStreamMode>(
        g_accelMode.load(std::memory_order_acquire));
}

void setAccelLatch(float x, float y, float z) {
    g_accelX.store(x, std::memory_order_relaxed);
    g_accelY.store(y, std::memory_order_relaxed);
    g_accelZ.store(z, std::memory_order_relaxed);
    g_accelLatchValid.store(true, std::memory_order_release);
}

bool accelLatch(float& x, float& y, float& z) {
    if (!g_accelLatchValid.load(std::memory_order_acquire)) return false;
    x = g_accelX.load(std::memory_order_relaxed);
    y = g_accelY.load(std::memory_order_relaxed);
    z = g_accelZ.load(std::memory_order_relaxed);
    return true;
}

void resetSensorControl() {
    g_accelMode.store(static_cast<std::uint8_t>(SensorStreamMode::Passthrough),
                      std::memory_order_release);
    g_accelLatchValid.store(false, std::memory_order_release);
    g_accelX.store(0.f, std::memory_order_relaxed);
    g_accelY.store(0.f, std::memory_order_relaxed);
    g_accelZ.store(0.f, std::memory_order_relaxed);
}

} // namespace ge::detail
