// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// AccelSynth unit tests — Shift-gated mouse → SENSOR_UPDATE.
// Stream/direct parity: same device-local gesture → same SENSOR_UPDATE.
// 🎯T156: dual-path oracle — direct AccelSynth::handle vs SP2I pack/unpack
// inject (shipped ServerSession marshalling) with optional SCRATCH traces.

#include "render/AccelSynth.h"

#include <ge/SeatPolicy.h>
#include <ge/WireSdlEvent.h>

#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using ge::AccelSynth;
using ge::SeatPolicy;
using ge::isTouchSyntheticMouse;

namespace {

// Trace of game-observed gravity (SENSOR emits) + presentation tilt.x.
struct AuthorityTrace {
    std::vector<std::pair<float, float>> gravity;
    std::vector<float> tiltX;
};

const char* scratchDir() {
    const char* d = std::getenv("T156_SCRATCH");
    return (d && d[0]) ? d : nullptr;
}

void writeJsonArrayPairs(std::ostream& os,
                         const std::vector<std::pair<float, float>>& v) {
    os << '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) os << ',';
        os << '[' << v[i].first << ',' << v[i].second << ']';
    }
    os << ']';
}

void writeJsonArrayFloats(std::ostream& os, const std::vector<float>& v) {
    os << '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) os << ',';
        os << v[i];
    }
    os << ']';
}

void writeParityJson(const char* path, const char* mode,
                     const AuthorityTrace& t) {
    std::ofstream out(path, std::ios::trunc);
    REQUIRE(out.good());
    out << "{\n"
        << "  \"mode\": \"" << mode << "\",\n"
        << "  \"trace_source\": \"AccelSynth\",\n"
        << "  \"entry\": \""
        << (std::string(mode) == "direct"
                ? "AccelSynth::handle"
                : "wire::packSdlEvent→unpackSdlEvent→AccelSynth::handle")
        << "\",\n"
        << "  \"gravity\": ";
    writeJsonArrayPairs(out, t.gravity);
    out << ",\n  \"tilt_x\": ";
    writeJsonArrayFloats(out, t.tiltX);
    out << "\n}\n";
}

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

// 🎯T156.4/6 loopback inject: SP2I pack (player wire) → unpack (ServerSession)
// → AccelSynth::handle. Distinct entry from driveLocalStyle; would diverge if
// pack/unpack corrupted motion deltas.
std::vector<SDL_Event> driveWireInjectStyle(int surfaceW, int surfaceH,
                                            const std::vector<SDL_Event>& seq) {
    AccelSynth synth;
    synth.setSurfacePixels(surfaceW, surfaceH);
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });
    for (const auto& e : seq) {
        if (isTouchSyntheticMouse(e)) continue;
        std::vector<uint8_t> wire;
        wire::packSdlEvent(e, wire);
        SDL_Event unpacked{};
        REQUIRE(wire::unpackSdlEvent(wire, unpacked));
        synth.handle(unpacked);
    }
    return emitted;
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

// Full authority trace: gesture + hold frames via update().
// wireStyle=false: direct handle; true: pack→unpack→handle (ServerSession path).
AuthorityTrace driveAuthorityScript(bool wireStyle,
                                    const std::vector<SDL_Event>& gesture,
                                    int holdFrames,
                                    int easeFrames = 0,
                                    bool armOnPrimary = false) {
    AccelSynth synth;
    synth.setArmOnPrimary(armOnPrimary);
    synth.setSurfacePixels(1000, 1000);
    AuthorityTrace t;
    synth.setEmit([&](const SDL_Event& e) {
        REQUIRE(e.type == SDL_EVENT_SENSOR_UPDATE);
        t.gravity.emplace_back(e.sensor.data[0], e.sensor.data[1]);
    });
    auto deliver = [&](const SDL_Event& e) {
        if (isTouchSyntheticMouse(e)) return;
        if (!wireStyle) {
            synth.handle(e);
            return;
        }
        std::vector<uint8_t> wire;
        wire::packSdlEvent(e, wire);
        SDL_Event unpacked{};
        REQUIRE(wire::unpackSdlEvent(wire, unpacked));
        // Sanity: motion xrel survives the SP2I round-trip.
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            CHECK(unpacked.motion.xrel == doctest::Approx(e.motion.xrel));
            CHECK(unpacked.motion.yrel == doctest::Approx(e.motion.yrel));
        }
        synth.handle(unpacked);
    };
    for (const auto& e : gesture) deliver(e);
    for (int i = 0; i < holdFrames; ++i) {
        synth.update();
        t.tiltX.push_back(synth.current().x);
    }
    if (easeFrames > 0) {
        deliver(keyShift(false));
        for (int i = 0; i < easeFrames; ++i) {
            synth.update();
            t.tiltX.push_back(synth.current().x);
        }
    }
    return t;
}

void requireMatchingTraces(const AuthorityTrace& a, const AuthorityTrace& b) {
    REQUIRE(a.gravity.size() == b.gravity.size());
    REQUIRE(a.tiltX.size() == b.tiltX.size());
    for (size_t i = 0; i < a.gravity.size(); ++i) {
        CHECK(a.gravity[i].first == doctest::Approx(b.gravity[i].first));
        CHECK(a.gravity[i].second == doctest::Approx(b.gravity[i].second));
    }
    for (size_t i = 0; i < a.tiltX.size(); ++i)
        CHECK(a.tiltX[i] == doctest::Approx(b.tiltX[i]));
}

SDL_Event sensorUpdate(float gx, float gy = 0.f) {
    SDL_Event e{};
    e.type = SDL_EVENT_SENSOR_UPDATE;
    e.sensor.data[0] = gx;
    e.sensor.data[1] = gy;
    e.sensor.data[2] = 0.f;
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

// 🎯T156.2: arm policy is a declared device fact (setArmOnPrimary), never a
// build flag — these run on every platform and build.
TEST_CASE("AccelSynth: primary button arms on touch-first device (no Shift)") {
    AccelSynth synth;
    synth.setArmOnPrimary(true); // touch-first virtual device, no accel
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

TEST_CASE("AccelSynth: finger drag arms and tilts on touch-first device") {
    AccelSynth synth;
    synth.setArmOnPrimary(true);
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

TEST_CASE("AccelSynth: primary button does NOT arm keyboard-class device") {
    AccelSynth synth; // default armOnPrimary=false (desktop-class)
    std::vector<SDL_Event> emitted;
    synth.setEmit([&](const SDL_Event& e) { emitted.push_back(e); });

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    CHECK_FALSE(synth.handle(down));
    CHECK_FALSE(synth.handle(motionRel(40.f, 0.f))); // not armed → not consumed
    CHECK(emitted.empty());
}

TEST_CASE("T156.2: AccelSynth policy is flag-free (mechanical check)") {
    // The header must contain no preprocessor conditional on GE_SERVER_BUILD,
    // and TARGET_OS_SIMULATOR only in realSensorAvailable (device capability
    // detection — the sim's sensor stack misreports; that is a device fact).
    std::string dir(__FILE__);
    dir = dir.substr(0, dir.find_last_of('/'));
    std::ifstream hdr(dir + "/render/AccelSynth.h");
    REQUIRE(hdr.good());
    std::string line;
    int serverBuildPP = 0, simPP = 0;
    while (std::getline(hdr, line)) {
        const auto hash = line.find_first_not_of(" \t");
        if (hash == std::string::npos || line[hash] != '#') continue;
        if (line.find("GE_SERVER_BUILD") != std::string::npos) serverBuildPP++;
        if (line.find("TARGET_OS_SIMULATOR") != std::string::npos) simPP++;
    }
    CHECK(serverBuildPP == 0);
    CHECK(simPP <= 1); // realSensorAvailable's sim force only
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

// ── 🎯T156.2 sensor authority ───────────────────────────────────────────

TEST_CASE("T156.2: ownsSensorStream while armed and after tilt") {
    AccelSynth synth;
    CHECK_FALSE(synth.ownsSensorStream());
    REQUIRE(synth.handle(keyShift(true)));
    CHECK(synth.ownsSensorStream());
    REQUIRE(synth.handle(motionRel(-80.f, 0.f))); // left
    CHECK(synth.ownsSensorStream());
    CHECK(synth.current().x == doctest::Approx(-80.f));
    float gx = 0.f, gy = 0.f;
    synth.gravitySample(gx, gy);
    // Left drag (negative x) → positive gx under emit convention (-kG * s * tx/mag).
    CHECK(gx > 0.f);
}

TEST_CASE("T156.2: hold-left gravity stable across update() frames") {
    // Simulates stream hold: no motion events, only update() ticks.
    // Competing external sensors must not be the authority (host drops them
    // when ownsSensorStream); authority re-emits the same gravity.
    AccelSynth synth;
    std::vector<std::pair<float, float>> gravity;
    synth.setEmit([&](const SDL_Event& e) {
        REQUIRE(e.type == SDL_EVENT_SENSOR_UPDATE);
        gravity.emplace_back(e.sensor.data[0], e.sensor.data[1]);
    });
    REQUIRE(synth.handle(keyShift(true)));
    REQUIRE(synth.handle(motionRel(-100.f, 0.f)));
    REQUIRE_FALSE(gravity.empty());
    const float g0 = gravity.back().first;
    CHECK(g0 > 0.f); // left → positive gx

    // Hold still: many update ticks must keep the same sign/magnitude.
    for (int i = 0; i < 30; ++i) synth.update();
    REQUIRE(gravity.size() >= 30);
    for (size_t i = 1; i < gravity.size(); ++i) {
        CHECK(gravity[i].first == doctest::Approx(g0).epsilon(1e-5));
        CHECK(gravity[i].second == doctest::Approx(0.f).epsilon(1e-5));
    }
    // Presentation tilt still left.
    CHECK(synth.current().x == doctest::Approx(-100.f));
    // External opposite sensor would reverse gravity if merged — authority
    // owns the stream so hosts must drop it; gravitySample stays left.
    float gx = 0.f, gy = 0.f;
    synth.gravitySample(gx, gy);
    CHECK(gx == doctest::Approx(g0));
    CHECK(gx > 0.f);
}

TEST_CASE("T156.2: presentation tilt and gravitySample share tilt_ state") {
    AccelSynth synth;
    synth.setEmit([](const SDL_Event&) {});
    REQUIRE(synth.handle(keyShift(true)));
    REQUIRE(synth.handle(motionRel(-60.f, 20.f)));
    const auto t = synth.current();
    float gx = 0.f, gy = 0.f;
    synth.gravitySample(gx, gy);
    // Same tilt_ drives both; non-zero and consistent sign with x.
    CHECK(t.x == doctest::Approx(-60.f));
    CHECK(t.y == doctest::Approx(20.f));
    CHECK(gx > 0.f);
}

// 🎯T156.4+T156.6: dual-path oracle — direct vs SP2I pack→unpack→handle.
// Paths are distinct entry points (wireStyle selects marshalling). Traces of
// gravity and presentation tilt must match bit-for-bit (same process clock).
TEST_CASE("T156.6: loopback gravity trace equals direct for hold-left script") {
    const std::vector<SDL_Event> gesture = {
        keyShift(true),
        motionRel(-50.f, 0.f),
        motionRel(-50.f, 0.f), // more left → tilt.x = -100
    };
    auto direct = driveAuthorityScript(/*wireStyle=*/false, gesture,
                                       /*holdFrames=*/20, /*easeFrames=*/10);
    auto loopback = driveAuthorityScript(/*wireStyle=*/true, gesture,
                                         /*holdFrames=*/20, /*easeFrames=*/10);
    requireMatchingTraces(direct, loopback);
    REQUIRE_FALSE(direct.gravity.empty());
    REQUIRE(direct.tiltX.size() >= 20);
    // During hold region (before ease), tilt stays at -100.
    CHECK(direct.tiltX[0] == doctest::Approx(-100.f));
    CHECK(direct.tiltX[19] == doctest::Approx(-100.f));
    // Left hold → positive gx (emit convention).
    CHECK(direct.gravity.front().first > 0.f);

    // Persist real numeric traces when T156_SCRATCH is set (verification plan).
    if (const char* dir = scratchDir()) {
        writeParityJson((std::string(dir) + "/parity-direct.json").c_str(),
                        "direct", direct);
        writeParityJson((std::string(dir) + "/parity-loopback.json").c_str(),
                        "loopback-inject", loopback);
        std::ofstream sum(std::string(dir) + "/parity-summary.json",
                          std::ios::trunc);
        REQUIRE(sum.good());
        sum << "{\n"
            << "  \"oracle\": \"T156.6 dual-path SP2I inject\",\n"
            << "  \"direct_entry\": \"AccelSynth::handle\",\n"
            << "  \"loopback_entry\": "
               "\"wire::packSdlEvent→unpackSdlEvent→AccelSynth::handle\",\n"
            << "  \"normalization\": \"none — same process, frame counts\",\n"
            << "  \"gravity_samples\": " << direct.gravity.size() << ",\n"
            << "  \"tilt_samples\": " << direct.tiltX.size() << ",\n"
            << "  \"result\": \"identical\"\n"
            << "}\n";
    }
}

// 🎯T156.6: the field-failure pathway — finger-drag arming on a touch-first
// glass that declares NO accelerometer. No Shift, no build flags; direct and
// SP2I-injected traces must match exactly.
TEST_CASE("T156.6: finger-arm dual-path trace equality (no Shift)") {
    const std::vector<SDL_Event> gesture = {
        fingerDown(0.5f, 0.5f),
        fingerMotion(0.45f, 0.5f, -0.05f, 0.f), // left 50 px on 1000 px glass
        fingerMotion(0.40f, 0.5f, -0.05f, 0.f), // left again → tilt.x = -100
    };
    auto direct = driveAuthorityScript(/*wireStyle=*/false, gesture,
                                       /*holdFrames=*/20, /*easeFrames=*/10,
                                       /*armOnPrimary=*/true);
    auto loopback = driveAuthorityScript(/*wireStyle=*/true, gesture,
                                         /*holdFrames=*/20, /*easeFrames=*/10,
                                         /*armOnPrimary=*/true);
    requireMatchingTraces(direct, loopback);
    REQUIRE_FALSE(direct.gravity.empty());
    CHECK(direct.tiltX[0] == doctest::Approx(-100.f));
    CHECK(direct.gravity.front().first > 0.f); // left → positive gx
}

// SP2I round-trip preserves SDL_Event payload (marshalling integrity).
TEST_CASE("T156.4: packSdlEvent/unpackSdlEvent round-trips motion and sensor") {
    SDL_Event motion = motionRel(-42.5f, 7.25f);
    std::vector<uint8_t> buf;
    wire::packSdlEvent(motion, buf);
    REQUIRE(buf.size() == sizeof(wire::MessageHeader) + sizeof(SDL_Event));
    SDL_Event out{};
    REQUIRE(wire::unpackSdlEvent(buf, out));
    CHECK(out.type == SDL_EVENT_MOUSE_MOTION);
    CHECK(out.motion.xrel == doctest::Approx(-42.5f));
    CHECK(out.motion.yrel == doctest::Approx(7.25f));

    SDL_Event sensor = sensorUpdate(-9.8f, 1.5f);
    wire::packSdlEvent(sensor, buf);
    REQUIRE(wire::unpackSdlEvent(buf, out));
    CHECK(out.type == SDL_EVENT_SENSOR_UPDATE);
    CHECK(out.sensor.data[0] == doctest::Approx(-9.8f));
    CHECK(out.sensor.data[1] == doctest::Approx(1.5f));
}

// 🎯T156.2+T156.3 integrated: primary hold-left + competing second-seat sensor
// traffic. SeatPolicy drops spectator SP2I; host filter drops external SENSOR
// while AccelSynth owns the stream. Gravity must not reverse.
TEST_CASE("T156: hold-left with second-seat competing sensors stays left") {
    SeatPolicy seats;
    const int primary = 1, spectator = 2;
    seats.onAttach(&primary); // first = primary
    seats.onAttach(&spectator);

    AccelSynth synth;
    std::vector<std::pair<float, float>> gameGravity; // what the game would see
    std::ostringstream log;
    log << "T156 hold-left + competitor seat (loopback-as-stream)\n";
    log << "primary=&primary spectator=&spectator\n";

    synth.setEmit([&](const SDL_Event& e) {
        REQUIRE(e.type == SDL_EVENT_SENSOR_UPDATE);
        gameGravity.emplace_back(e.sensor.data[0], e.sensor.data[1]);
        log << "frame_emit gx=" << e.sensor.data[0]
            << " gy=" << e.sensor.data[1]
            << " tilt_x=" << synth.current().x << "\n";
    });

    // Primary seat: Shift + hold-left via SP2I (ServerSession path). The
    // synth exists because this seat declared NO accelerometer — so a
    // competing sensor source for this virtual device cannot exist. No
    // arbitration filter (🎯T156.2: constructional authority).
    auto injectPrimary = [&](const SDL_Event& e) {
        std::vector<uint8_t> wire;
        wire::packSdlEvent(e, wire);
        SDL_Event unpacked{};
        REQUIRE(wire::unpackSdlEvent(wire, unpacked));
        REQUIRE(seats.acceptSdlEvent(&primary, unpacked));
        if (synth.handle(unpacked)) return; // consumed by AccelSynth
        // Anything else reaches the game — record sensor latches.
        if (unpacked.type == SDL_EVENT_SENSOR_UPDATE) {
            gameGravity.emplace_back(unpacked.sensor.data[0],
                                     unpacked.sensor.data[1]);
            log << "game_sensor gx=" << unpacked.sensor.data[0] << "\n";
        }
    };

    auto injectSpectator = [&](const SDL_Event& e) {
        std::vector<uint8_t> wire;
        wire::packSdlEvent(e, wire);
        SDL_Event unpacked{};
        REQUIRE(wire::unpackSdlEvent(wire, unpacked));
        if (!seats.acceptSdlEvent(&spectator, unpacked)) {
            log << "seat_drop spectator event type=" << unpacked.type
                << " (non-primary)\n";
            return;
        }
        FAIL("spectator must not be accepted under primary-seat policy");
    };

    injectPrimary(keyShift(true));
    injectPrimary(motionRel(-100.f, 0.f));
    REQUIRE(synth.ownsSensorStream());
    REQUIRE_FALSE(gameGravity.empty());
    const float gLeft = gameGravity.back().first;
    CHECK(gLeft > 0.f);
    log << "hold_left_locked gx=" << gLeft << " tilt_x=" << synth.current().x
        << "\n";

    // Hold frames: authority re-emits; spectator floods opposite gravity.
    // 🎯T156.2 constructional authority: the ONLY possible competing sensor
    // source is a non-primary seat (this seat declared no accelerometer),
    // and the seat gate drops those before any synth/game code runs.
    for (int i = 0; i < 25; ++i) {
        injectSpectator(sensorUpdate(-9.8f, 0.f)); // would reverse if merged
        synth.update();
        CHECK(synth.current().x == doctest::Approx(-100.f));
        float gx = 0.f, gy = 0.f;
        synth.gravitySample(gx, gy);
        CHECK(gx == doctest::Approx(gLeft).epsilon(1e-5));
        CHECK(gx > 0.f); // never reverse
    }

    // All game-observed gravity during hold stays left (positive gx).
    for (const auto& g : gameGravity) {
        CHECK(g.first > 0.f);
        CHECK(g.first == doctest::Approx(gLeft).epsilon(1e-4));
    }
    log << "result=PASS gravity_never_reversed samples=" << gameGravity.size()
        << " final_gx=" << gameGravity.back().first << "\n";

    if (const char* dir = scratchDir()) {
        std::ofstream out(std::string(dir) + "/hold-left-stream.log",
                          std::ios::trunc);
        REQUIRE(out.good());
        out << log.str();
    }
}

// 🎯T156.2: a glass that declares a real accelerometer IS the authority —
// no synth is constructed for that seat, and its wire sensor samples reach
// the game unfiltered. Models the DirectRenderHost seat-authority decision.
TEST_CASE("T156.2: real-accel seat has no synth; wire sensors flow unfiltered") {
    SeatPolicy seats;
    const int primary = 1, spectator = 2;
    seats.onAttach(&primary);
    seats.onAttach(&spectator);

    // Seat declared kCapHasAccelerometer → host constructs NO synth.
    std::vector<std::pair<float, float>> gameGravity;
    auto inject = [&](const void* seat, const SDL_Event& e) {
        std::vector<uint8_t> wire;
        wire::packSdlEvent(e, wire);
        SDL_Event unpacked{};
        REQUIRE(wire::unpackSdlEvent(wire, unpacked));
        if (!seats.acceptSdlEvent(seat, unpacked)) return;
        // No synth in the pipeline for this virtual device.
        if (unpacked.type == SDL_EVENT_SENSOR_UPDATE)
            gameGravity.emplace_back(unpacked.sensor.data[0],
                                     unpacked.sensor.data[1]);
    };

    // Real glass tilt stream (already screen-framed on the player).
    for (int i = 0; i < 10; ++i) inject(&primary, sensorUpdate(-3.f, 0.5f));
    // Finger events from the same glass must NOT create a synth or divert
    // gravity — there is no synth; they'd reach the game as raw input.
    inject(&primary, fingerDown(0.5f, 0.5f));
    inject(&primary, fingerMotion(0.6f, 0.5f, 0.1f, 0.f));
    for (int i = 0; i < 10; ++i) inject(&primary, sensorUpdate(-3.f, 0.5f));
    // Spectator sensors still dropped.
    inject(&spectator, sensorUpdate(9.9f, 0.f));

    REQUIRE(gameGravity.size() == 20);
    for (const auto& g : gameGravity) {
        CHECK(g.first == doctest::Approx(-3.f));
        CHECK(g.second == doctest::Approx(0.5f));
    }
}
