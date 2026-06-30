// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T111 Frame-performance metrics: the Context frame-time EMA behind fps() /
// frameTime(), and the pure onMetrics delivery gate.

#include <doctest.h>
#include <ge/SessionHost.h>

#include <SDL3/SDL.h>

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

// 🎯T132 render-on-demand flag logic. The loop/host integration (idle-block,
// wake) is exercised at runtime; here we pin the Context state machine.
TEST_CASE("Context render-on-demand: continuous flag + redraw set/take (🎯T132)") {
    Context ctx = makeContext();
    // Default is continuous; nothing pending.
    CHECK(ctx.continuousRendering());
    CHECK_FALSE(ctx.redrawPending());

    // Opt out → on-demand. The host marks a redraw on input; the loop drains it.
    ctx.setContinuousRendering(false);
    CHECK_FALSE(ctx.continuousRendering());
    CHECK_FALSE(ctx.redrawPending());
    ctx.markRedraw();
    CHECK(ctx.redrawPending());
    CHECK(ctx.takeRedraw());          // loop consumes...
    CHECK_FALSE(ctx.takeRedraw());    // ...and it's cleared
    CHECK_FALSE(ctx.redrawPending());

    // requestRedraw() sets the flag (the SDL wake is best-effort; init events so
    // the push has a queue to land in).
    SDL_InitSubSystem(SDL_INIT_EVENTS);
    ctx.requestRedraw();
    CHECK(ctx.redrawPending());
    CHECK(ctx.takeRedraw());

    // Resuming continuous requests a redraw so the next frame always draws.
    (void)ctx.takeRedraw();
    ctx.setContinuousRendering(true);
    CHECK(ctx.continuousRendering());
    CHECK(ctx.takeRedraw());
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

// 🎯T131.1 Render triggers (the *level* signal). The run loop renders while any
// trigger is active and idles when all are false; here we pin the OR predicate.
TEST_CASE("Context render triggers gate anyRenderTriggerActive (🎯T131.1)") {
    Context ctx = makeContext();
    CHECK_FALSE(ctx.anyRenderTriggerActive());   // none registered → idle-eligible

    bool a = false, b = false;
    ctx.addRenderTrigger([&] { return a; });
    ctx.addRenderTrigger([&] { return b; });
    CHECK_FALSE(ctx.anyRenderTriggerActive());   // both false
    a = true;
    CHECK(ctx.anyRenderTriggerActive());         // OR over all triggers
    a = false; b = true;
    CHECK(ctx.anyRenderTriggerActive());
    b = false;
    CHECK_FALSE(ctx.anyRenderTriggerActive());   // activity settled → idle again

    // A null trigger is ignored, not crashed on.
    ctx.addRenderTrigger({});
    CHECK_FALSE(ctx.anyRenderTriggerActive());
}

// 🎯T131.5 The present counter is the ground-truth "did we draw?" signal.
TEST_CASE("Context framesPresented is monotonic from zero (🎯T131.5)") {
    Context ctx = makeContext();
    CHECK(ctx.framesPresented() == 0);
    ctx.recordPresent();
    ctx.recordPresent();
    ctx.recordPresent();
    CHECK(ctx.framesPresented() == 3);
}

// 🎯T131.1 requestRedraw() coalesces: any number of calls in one redraw cycle
// queue exactly one wake event, so scattered callers need no central collector.
TEST_CASE("requestRedraw coalesces the wake event per redraw cycle (🎯T131.1)") {
    SDL_InitSubSystem(SDL_INIT_EVENTS);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    Context ctx = makeContext();

    auto drainWakes = [] {
        int n = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_USER && e.user.code == Context::kRedrawEventCode) ++n;
        return n;
    };

    for (int i = 0; i < 5; ++i) ctx.requestRedraw();  // five callers, one cycle
    CHECK(ctx.redrawPending());
    CHECK(drainWakes() == 1);                          // coalesced, not 5

    // Once the loop consumes the flag, the next requestRedraw wakes again.
    CHECK(ctx.takeRedraw());
    ctx.requestRedraw();
    ctx.requestRedraw();
    CHECK(drainWakes() == 1);
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

// 🎯T131.4 State-diff: a screen renders when its generation counter changes and
// idles when stable; recordPresent() (a drawn frame) advances the baseline.
TEST_CASE("renderWhenStateChanges renders on a generation bump, idles when stable (🎯T131.4)") {
    Context ctx = makeContext();
    uint64_t gen = 1;
    ctx.renderWhenStateChanges([&] { return gen; });
    CHECK_FALSE(ctx.anyRenderTriggerActive());   // seeded at registration → stable

    gen = 2;                                      // render-relevant state changed
    CHECK(ctx.anyRenderTriggerActive());          // → wants a frame
    CHECK(ctx.anyRenderTriggerActive());          // pure: a second poll agrees (no self-consume)
    ctx.recordPresent();                          // the frame is drawn → baseline catches up
    CHECK_FALSE(ctx.anyRenderTriggerActive());    // stable again → idle

    gen = 9;                                       // changes again
    CHECK(ctx.anyRenderTriggerActive());
    ctx.recordPresent();
    CHECK_FALSE(ctx.anyRenderTriggerActive());
}

// A null generation is ignored (renderWhenStateChanges is a no-op), leaving the
// screen idle-eligible rather than registering a dead trigger.
TEST_CASE("renderWhenStateChanges ignores a null generation (🎯T131.4)") {
    Context ctx = makeContext();
    ctx.renderWhenStateChanges({});
    CHECK_FALSE(ctx.anyRenderTriggerActive());
}
