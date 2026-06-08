// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T111 Frame-performance metrics: the Context frame-time EMA behind fps() /
// frameTime(), and the pure onMetrics delivery gate.

#include <doctest.h>
#include <ge/SessionHost.h>

#include <cmath>

using ge::Context;
using ge::DeviceClass;
using ge::shouldReportMetrics;

namespace {
Context makeContext() {
    // An in-memory DB keeps the test free of the filesystem; no GPU/SDL needed.
    return Context(640, 480, DeviceClass::Desktop, ":memory:", "");
}
constexpr float kSixtyHz = 1.0f / 60.0f;
}  // namespace

TEST_CASE("Context::fps is 0 until the first frame is timed") {
    Context ctx = makeContext();
    CHECK(ctx.fps() == 0.0f);
    CHECK(ctx.frameTime() == 0.0f);
}

TEST_CASE("Context first sample seeds the EMA directly") {
    Context ctx = makeContext();
    ctx.recordFrameTime(kSixtyHz);
    CHECK(ctx.frameTime() == doctest::Approx(kSixtyHz));
    CHECK(ctx.fps() == doctest::Approx(60.0f));
}

TEST_CASE("Context fps == 1 / frameTime holds after any sequence") {
    Context ctx = makeContext();
    for (float dt : {1.0f / 30, 1.0f / 50, 1.0f / 60, 1.0f / 45})
        ctx.recordFrameTime(dt);
    REQUIRE(ctx.frameTime() > 0.0f);
    CHECK(ctx.fps() * ctx.frameTime() == doctest::Approx(1.0f));
    CHECK(ctx.fps() == doctest::Approx(1.0f / ctx.frameTime()));
}

TEST_CASE("Context EMA converges toward a sustained frame time") {
    Context ctx = makeContext();
    ctx.recordFrameTime(1.0f / 30);             // seed at 30 fps
    for (int i = 0; i < 200; ++i)               // then sustain 60 fps
        ctx.recordFrameTime(kSixtyHz);
    CHECK(ctx.frameTime() == doctest::Approx(kSixtyHz).epsilon(0.001));
    CHECK(ctx.fps() == doctest::Approx(60.0f).epsilon(0.01));
}

TEST_CASE("Context ignores zero and multi-frame stalls") {
    Context ctx = makeContext();
    ctx.recordFrameTime(kSixtyHz);
    const float before = ctx.frameTime();
    ctx.recordFrameTime(0.0f);     // zero — ignored
    ctx.recordFrameTime(-0.5f);    // negative — ignored
    ctx.recordFrameTime(2.0f);     // 2 s stall (>= 1 s) — ignored
    CHECK(ctx.frameTime() == doctest::Approx(before));
}

TEST_CASE("shouldReportMetrics never fires for a non-positive fps") {
    CHECK_FALSE(shouldReportMetrics(0.0f, 0.0f, 0.0f));    // even every-frame
    CHECK_FALSE(shouldReportMetrics(-1.0f, 60.0f, 0.1f));
}

TEST_CASE("shouldReportMetrics with threshold 0 fires every frame") {
    CHECK(shouldReportMetrics(60.0f, 0.0f, 0.0f));    // baseline
    CHECK(shouldReportMetrics(60.0f, 60.0f, 0.0f));   // unchanged, still fires
    CHECK(shouldReportMetrics(60.1f, 60.0f, 0.0f));
}

TEST_CASE("shouldReportMetrics fires a baseline on the first valid reading") {
    CHECK(shouldReportMetrics(60.0f, 0.0f, 0.1f));   // lastReported <= 0 => baseline
}

TEST_CASE("shouldReportMetrics gates sub-threshold deviations") {
    // Baseline 60 fps, 10% threshold => fire only outside [54, 66].
    CHECK_FALSE(shouldReportMetrics(61.0f, 60.0f, 0.1f));   // +1.7%
    CHECK_FALSE(shouldReportMetrics(59.0f, 60.0f, 0.1f));   // -1.7%
    CHECK(shouldReportMetrics(66.0f, 60.0f, 0.1f));         // +10% exactly (>=)
    CHECK(shouldReportMetrics(54.0f, 60.0f, 0.1f));         // -10% exactly
    CHECK(shouldReportMetrics(80.0f, 60.0f, 0.1f));         // well above
}
