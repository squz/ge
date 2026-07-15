// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// AccelSynth unit tests — Shift-gated mouse → SENSOR_UPDATE.
// Stream/direct parity: same device-local gesture → same SENSOR_UPDATE.

#include "render/AccelSynth.h"

#include <doctest.h>

#include <cmath>
#include <vector>

using ge::AccelSynth;
using ge::isTouchSyntheticMouse;

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
    e.motion.which = 0;
    return e;
}

SDL_Event motionAbs(float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = 0.f;
    e.motion.yrel = 0.f;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.which = 0;
    return e;
}

SDL_Event touchMouseMotion(float xrel, float yrel) {
    SDL_Event e = motionRel(xrel, yrel);
    e.motion.which = SDL_TOUCH_MOUSEID;
    return e;
}

SDL_Event fingerDown(float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_FINGER_DOWN;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.dx = 0.f;
    e.tfinger.dy = 0.f;
    return e;
}

SDL_Event fingerMotion(float x, float y, float dx, float dy) {
    SDL_Event e{};
    e.type = SDL_EVENT_FINGER_MOTION;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.dx = dx;
    e.tfinger.dy = dy;
    return e;
}

// Drive AccelSynth with a device-local gesture sequence (shipped handle()).
// surfaceW/H pin finger denormalization (player DeviceInfo on stream; local
// window on direct). Returns emitted SENSOR_UPDATE events.
std::vector<SDL_Event> driveLocalStyle(int surfaceW, int surfaceH,
                                       const std::vector<SDL_Event>& seq) {
    AccelSynth synth;
    synth.setSurfacePixels(surfaceW, surfaceH);
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });
    for (const auto& e : seq) {
        if (isTouchSyntheticMouse(e)) continue;  // player forward filter
        synth.handle(e);
    }
    return emitted;
}

// Same handle path as if events arrived via SDL_PushEvent on the server
// (no separate code path in AccelSynth — inject is just handle()).
std::vector<SDL_Event> driveWireInjectStyle(int surfaceW, int surfaceH,
                                            const std::vector<SDL_Event>& seq) {
    return driveLocalStyle(surfaceW, surfaceH, seq);
}

void requireMatchingSensor(const std::vector<SDL_Event>& a,
                           const std::vector<SDL_Event>& b) {
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].type == SDL_EVENT_SENSOR_UPDATE);
        REQUIRE(b[i].type == SDL_EVENT_SENSOR_UPDATE);
        CHECK(a[i].sensor.data[0] == doctest::Approx(b[i].sensor.data[0]));
        CHECK(a[i].sensor.data[1] == doctest::Approx(b[i].sensor.data[1]));
    }
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
    CHECK(synth.handle(motionAbs(10.f, 20.f)));
    CHECK(emitted.empty());
    // Second sample → dx=30, dy=0.
    CHECK(synth.handle(motionAbs(40.f, 20.f)));
    REQUIRE(emitted.size() == 1);
    CHECK(synth.current().x == doctest::Approx(30.f));
    CHECK(synth.current().y == doctest::Approx(0.f));
}

TEST_CASE("AccelSynth: motion without Shift is not consumed") {
    AccelSynth synth;
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });
    CHECK_FALSE(synth.handle(motionRel(50.f, 0.f)));
    CHECK(emitted.empty());
}

#if (defined(__APPLE__) && TARGET_OS_SIMULATOR) || defined(GE_SERVER_BUILD)
TEST_CASE("AccelSynth: primary button arms without Shift (sim/server)") {
    AccelSynth synth;
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    CHECK_FALSE(synth.handle(down));  // button not consumed
    CHECK(synth.handle(motionRel(40.f, 0.f)));
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0].type == SDL_EVENT_SENSOR_UPDATE);
}

TEST_CASE("AccelSynth: finger drag arms and tilts (sim/server)") {
    AccelSynth synth;
    synth.setSurfacePixels(1000, 1000);
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });

    CHECK_FALSE(synth.handle(fingerDown(0.5f, 0.5f)));
    // Relative finger delta 0.05 of surface → 50 px.
    CHECK(synth.handle(fingerMotion(0.55f, 0.5f, 0.05f, 0.f)));
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0].type == SDL_EVENT_SENSOR_UPDATE);
    CHECK(synth.current().x == doctest::Approx(50.f));
}
#endif

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

// ── Stream/direct parity (shipped AccelSynth + isTouchSyntheticMouse) ──

TEST_CASE("parity: isTouchSyntheticMouse detects TOUCH_MOUSEID") {
    CHECK(isTouchSyntheticMouse(touchMouseMotion(10.f, 0.f)));
    CHECK_FALSE(isTouchSyntheticMouse(motionRel(10.f, 0.f)));
    SDL_Event btn{};
    btn.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    btn.button.button = SDL_BUTTON_LEFT;
    btn.button.which = SDL_TOUCH_MOUSEID;
    CHECK(isTouchSyntheticMouse(btn));
    btn.button.which = 0;
    CHECK_FALSE(isTouchSyntheticMouse(btn));
}

TEST_CASE("parity: local vs wire-inject same mouse relative gesture") {
    // Device-local sequence as direct would poll / as player would forward.
    const std::vector<SDL_Event> seq = {
        keyShift(true),
        motionRel(50.f, 0.f),
        motionRel(25.f, 10.f),
    };
    constexpr int kDeviceW = 2752;
    constexpr int kDeviceH = 2064;
    auto local = driveLocalStyle(kDeviceW, kDeviceH, seq);
    auto wire = driveWireInjectStyle(kDeviceW, kDeviceH, seq);
    requireMatchingSensor(local, wire);
    REQUIRE(local.size() >= 1);
    // Sanity: non-trivial tilt (not zero).
    CHECK(std::fabs(local.back().sensor.data[0]) > 0.01f);
}

TEST_CASE("parity: local vs wire-inject same absolute mouse drag") {
    const std::vector<SDL_Event> seq = {
        keyShift(true),
        motionAbs(100.f, 100.f),  // seed
        motionAbs(150.f, 100.f),  // +50 x
        motionAbs(150.f, 130.f),  // +30 y
    };
    auto local = driveLocalStyle(800, 600, seq);
    auto wire = driveWireInjectStyle(800, 600, seq);
    requireMatchingSensor(local, wire);
    REQUIRE(local.size() == 2);
}

TEST_CASE("parity: local vs wire-inject same finger drag on device surface") {
    // Normalized finger motion on a phone-sized surface (DeviceInfo).
    constexpr int kDeviceW = 1170;
    constexpr int kDeviceH = 2532;
    const std::vector<SDL_Event> seq = {
        keyShift(true),  // arm without needing GE_SERVER_BUILD
        fingerMotion(0.5f, 0.5f, 0.f, 0.f),       // seed via abs if dx=0
        fingerMotion(0.55f, 0.5f, 0.05f, 0.f),    // 0.05 * 1170 = 58.5 px
    };
    auto local = driveLocalStyle(kDeviceW, kDeviceH, seq);
    auto wire = driveWireInjectStyle(kDeviceW, kDeviceH, seq);
    requireMatchingSensor(local, wire);
    REQUIRE_FALSE(local.empty());
    // Finger dx path: tilt.x accumulates 0.05 * 1170.
    AccelSynth probe;
    probe.setSurfacePixels(kDeviceW, kDeviceH);
    std::vector<SDL_Event> em;
    probe.setEmit([&](const SDL_Event& e) { em.push_back(e); });
    for (const auto& e : seq) probe.handle(e);
    CHECK(probe.current().x == doctest::Approx(0.05f * float(kDeviceW)));
}

TEST_CASE("parity: finger scale uses device surface not host window size") {
    // Same normalized drag on player surface 1170 vs wrongly using host 2048
    // must produce different tilt — proves setSurfacePixels is the scale.
    const std::vector<SDL_Event> seq = {
        keyShift(true),
        fingerMotion(0.5f, 0.5f, 0.1f, 0.f),
    };
    auto onDevice = driveLocalStyle(1170, 2532, seq);
    auto onHost = driveLocalStyle(2048, 1536, seq);
    REQUIRE(onDevice.size() == 1);
    REQUIRE(onHost.size() == 1);
    // |gx| scales with sin(mag * k); larger surface → larger mag → larger |gx|.
    CHECK(std::fabs(onHost[0].sensor.data[0]) >
          std::fabs(onDevice[0].sensor.data[0]));
    // Device-local path uses 1170, not 2048: 0.1 * 1170 = 117 px tilt.
    AccelSynth d;
    d.setSurfacePixels(1170, 2532);
    d.setEmit([](const SDL_Event&) {});
    for (const auto& e : seq) d.handle(e);
    CHECK(d.current().x == doctest::Approx(117.f));
}

TEST_CASE("parity: touch-mouse duplicate does not double-apply with finger") {
    // Physical touch often yields FINGER_MOTION + TOUCH_MOUSEID mouse.
    // After filter, only finger contributes once.
    constexpr int kW = 1000;
    constexpr int kH = 1000;
    const std::vector<SDL_Event> withDup = {
        keyShift(true),
        fingerMotion(0.5f, 0.5f, 0.05f, 0.f),  // 50 px
        touchMouseMotion(50.f, 0.f),             // would double if not filtered
    };
    const std::vector<SDL_Event> fingerOnly = {
        keyShift(true),
        fingerMotion(0.5f, 0.5f, 0.05f, 0.f),
    };
    auto filtered = driveLocalStyle(kW, kH, withDup);
    auto once = driveLocalStyle(kW, kH, fingerOnly);
    requireMatchingSensor(filtered, once);
    AccelSynth probe;
    probe.setSurfacePixels(kW, kH);
    probe.setEmit([](const SDL_Event&) {});
    for (const auto& e : withDup) {
        if (isTouchSyntheticMouse(e)) continue;
        probe.handle(e);
    }
    CHECK(probe.current().x == doctest::Approx(50.f));  // not 100
}
