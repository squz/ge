// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "SensorControl.h"

#include <ge/SessionHost.h>  // T175.7 liveSessionIds

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ge::detail {
namespace {

// T175.7 One sensor-authority state per session (0 = process default for
// tests / pre-session). Mirrors RefreshRateBoost::M: atomics inside a
// per-instance struct, the map guarded for the rare store creation.
struct AccelState {
    std::atomic<std::uint8_t> mode{static_cast<std::uint8_t>(SensorStreamMode::Passthrough)};
    std::atomic<bool>  latchValid{false};
    std::atomic<float> x{0.f}, y{0.f}, z{0.f};
};

std::mutex g_accelMu;
std::unordered_map<std::uint32_t, std::shared_ptr<AccelState>> g_accel;

std::shared_ptr<AccelState> accelFor(std::uint32_t sid) {
    std::lock_guard<std::mutex> lk(g_accelMu);
    auto& sp = g_accel[sid];
    if (!sp) sp = std::make_shared<AccelState>();
    return sp;
}

std::uint32_t lenientSid() {
    const auto live = ge::liveSessionIds();
    return live.size() == 1 ? live.front() : 0u;
}

} // namespace

void setAccelStreamMode(SensorStreamMode mode) { setAccelStreamMode(lenientSid(), mode); }
void setAccelStreamMode(std::uint32_t sid, SensorStreamMode mode) {
    accelFor(sid)->mode.store(static_cast<std::uint8_t>(mode), std::memory_order_release);
}

SensorStreamMode accelStreamMode() { return accelStreamMode(lenientSid()); }
SensorStreamMode accelStreamMode(std::uint32_t sid) {
    return static_cast<SensorStreamMode>(
        accelFor(sid)->mode.load(std::memory_order_acquire));
}

void setAccelLatch(float x, float y, float z) { setAccelLatch(lenientSid(), x, y, z); }
void setAccelLatch(std::uint32_t sid, float x, float y, float z) {
    auto st = accelFor(sid);
    st->x.store(x, std::memory_order_relaxed);
    st->y.store(y, std::memory_order_relaxed);
    st->z.store(z, std::memory_order_relaxed);
    st->latchValid.store(true, std::memory_order_release);
}

bool accelLatch(float& x, float& y, float& z) { return accelLatch(lenientSid(), x, y, z); }
bool accelLatch(std::uint32_t sid, float& x, float& y, float& z) {
    auto st = accelFor(sid);
    if (!st->latchValid.load(std::memory_order_acquire)) return false;
    x = st->x.load(std::memory_order_relaxed);
    y = st->y.load(std::memory_order_relaxed);
    z = st->z.load(std::memory_order_relaxed);
    return true;
}

void resetSensorControl() {
    std::lock_guard<std::mutex> lk(g_accelMu);
    g_accel.clear();  // every session back to passthrough, latches cleared
}

} // namespace ge::detail
