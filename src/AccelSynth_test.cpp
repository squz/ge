// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// AccelSynth unit tests — Shift-gated mouse → SENSOR_UPDATE.

#include "render/AccelSynth.h"

#include <doctest.h>

#include <vector>

using ge::AccelSynth;

namespace {

SDL_Event keyShift(bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = SDL_SCANCODE_LSHIFT;
    return e;
}

SDL_Event motionRel(float xrel, float yrel) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    e.motion.x = 100.f;
    e.motion.y = 100.f;
    return e;
}

SDL_Event motionAbs(float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = 0.f;
    e.motion.yrel = 0.f;
    e.motion.x = x;
    e.motion.y = y;
    return e;
}

} // namespace

TEST_CASE("AccelSynth: Shift + relative motion emits sensor update") {
    AccelSynth synth;
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });

    CHECK(synth.handle(keyShift(true)));
    CHECK(emitted.empty());

    CHECK(synth.handle(motionRel(50.f, 0.f)));
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0].type == SDL_EVENT_SENSOR_UPDATE);
    // +x drag → negative gx (iOS counter-gravity convention).
    CHECK(emitted[0].sensor.data[0] < 0.f);

    CHECK(synth.current().x == doctest::Approx(50.f));
    CHECK(synth.current().y == doctest::Approx(0.f));
}

TEST_CASE("AccelSynth: absolute motion fallback when xrel/yrel are zero") {
    AccelSynth synth;
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });

    REQUIRE(synth.handle(keyShift(true)));
    // First absolute sample seeds the baseline (no delta yet).
    REQUIRE(synth.handle(motionAbs(10.f, 20.f)));
    CHECK(emitted.empty());
    // Second sample → dx=30, dy=0.
    REQUIRE(synth.handle(motionAbs(40.f, 20.f)));
    REQUIRE(emitted.size() == 1);
    CHECK(synth.current().x == doctest::Approx(30.f));
    CHECK(synth.current().y == doctest::Approx(0.f));
}

TEST_CASE("AccelSynth: non-Shift events pass through") {
    AccelSynth synth;
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_A;
    CHECK_FALSE(synth.handle(e));
}

// realSensorAvailable() on desktop unit-test host is environment-dependent;
// only assert the iOS-sim compile-time force when building for simulator.
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
TEST_CASE("AccelSynth: iOS Simulator forces realSensorAvailable false") {
    CHECK_FALSE(AccelSynth::realSensorAvailable());
}
#endif
