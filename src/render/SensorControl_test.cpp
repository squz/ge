// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "SensorControl.h"

#include <cassert>
#include <cstdio>

using ge::detail::SensorStreamMode;
using ge::detail::accelLatch;
using ge::detail::accelStreamMode;
using ge::detail::resetSensorControl;
using ge::detail::setAccelLatch;
using ge::detail::setAccelStreamMode;

int main() {
    resetSensorControl();
    assert(accelStreamMode() == SensorStreamMode::Passthrough);
    float x = 1, y = 1, z = 1;
    assert(!accelLatch(x, y, z));

    setAccelStreamMode(SensorStreamMode::Override);
    assert(accelStreamMode() == SensorStreamMode::Override);
    setAccelLatch(1.5f, -0.5f, 9.8f);
    assert(accelLatch(x, y, z));
    assert(x == 1.5f && y == -0.5f && z == 9.8f);

    setAccelStreamMode(SensorStreamMode::Mute);
    assert(accelStreamMode() == SensorStreamMode::Mute);
    // Latch retained across mode changes (script can re-override without re-set).
    assert(accelLatch(x, y, z));

    setAccelStreamMode(SensorStreamMode::Passthrough);
    assert(accelStreamMode() == SensorStreamMode::Passthrough);

    resetSensorControl();
    assert(accelStreamMode() == SensorStreamMode::Passthrough);
    assert(!accelLatch(x, y, z));

    std::puts("SensorControl_test ok");
    return 0;
}
