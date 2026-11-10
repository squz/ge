#include <doctest.h>
#include <ge/GlobeController.h>

#include <chrono>
#include <cmath>
#include <thread>

// 🎯T122/T123 — GlobeController is camera-orientation-agnostic. The same mouse
// drag spins about a different world axis depending on the consumer's camera
// basis: world Z under the default (Z-up) convention, world Y after the
// consumer declares a Y-up camera via setCameraBasis — no per-app axis literals.

namespace {

void mouseDown(ge::GlobeController& g, float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.x = x;
    e.button.y = y;
    g.event(e);
}

void mouseMove(ge::GlobeController& g, float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = x;
    e.motion.y = y;
    g.event(e);
}

void mouseUp(ge::GlobeController& g) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    g.event(e);
}

// Eight frames of a steady 20 px/frame horizontal drag at 60 Hz.
// sensitivity 0.01 → 12 rad/s about screen-up (default Z).
void steadyHorizontalFlick(ge::GlobeController& g) {
    mouseDown(g, 100.0f, 100.0f);
    for (int i = 1; i <= 8; ++i) {
        mouseMove(g, 100.0f + 20.0f * float(i), 100.0f);
        g.update(1.0f / 60.0f);
    }
}

} // namespace

TEST_CASE("GlobeController default basis: horizontal drag spins about Z") {
    ge::GlobeController g;
    mouseDown(g, 100.0f, 100.0f);
    mouseMove(g, 160.0f, 100.0f);  // 60px right, past the 10px drag threshold
    auto q = g.rotation().orientation();
    CHECK(q.z != doctest::Approx(0.0f));
    CHECK(q.y == doctest::Approx(0.0f));
}

TEST_CASE("GlobeController Y-up basis: horizontal drag spins about Y") {
    ge::GlobeController g;
    g.setCameraBasis({1, 0, 0}, {0, 1, 0}, {0, 0, -1});  // esfera Y-up camera
    mouseDown(g, 100.0f, 100.0f);
    mouseMove(g, 160.0f, 100.0f);
    auto q = g.rotation().orientation();
    CHECK(q.y != doctest::Approx(0.0f));
    CHECK(q.z == doctest::Approx(0.0f));
}

TEST_CASE("GlobeController Y-up basis: vertical drag spins about screen-right (X)") {
    ge::GlobeController g;
    g.setCameraBasis({1, 0, 0}, {0, 1, 0}, {0, 0, -1});
    mouseDown(g, 100.0f, 100.0f);
    mouseMove(g, 100.0f, 160.0f);  // 60px down
    auto q = g.rotation().orientation();
    CHECK(q.x != doctest::Approx(0.0f));
    CHECK(q.y == doctest::Approx(0.0f));
}

// 🎯T131.3 Render-on-demand: a fresh idle controller stays silent; while a drag
// is in flight the controller keeps the loop awake (onRedraw each frame) so the
// spin stays responsive without forcing continuous rendering.
TEST_CASE("GlobeController onRedraw: idle is silent, dragging redraws") {
    ge::GlobeController g;
    int redraws = 0;
    g.onRedraw = [&]{ ++redraws; };

    g.update(0.016f);            // fresh: not dragging, not moving → no redraw
    CHECK(redraws == 0);

    mouseDown(g, 100.0f, 100.0f);
    g.update(0.016f);            // dragging → redraw each frame held
    g.update(0.016f);
    CHECK(redraws == 2);
}

// A flick that is still moving on the frame it is released keeps its
// inertia. 12 rad/s is the drag speed (20 px * 60 Hz * sensitivity 0.01).
TEST_CASE("GlobeController flick immediately before release keeps inertia") {
    ge::GlobeController g;
    steadyHorizontalFlick(g);
    mouseUp(g);
    auto v = g.rotation().angularVelocity();
    CHECK(v.z == doctest::Approx(12.0f).epsilon(0.02));
    CHECK(v.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(v.y == doctest::Approx(0.0f).epsilon(0.01));
}

// Holding still for >= 1 s must not release the pre-hold flick. One
// update spanning the whole second (the render-on-demand wake after a
// stationary hold) is the case the per-call EMA gets wrong: a single
// lerp toward zero leaves most of the old speed.
TEST_CASE("GlobeController stationary hold releases no spin") {
    ge::GlobeController g;
    steadyHorizontalFlick(g);
    g.update(1.0f);  // finger still down, no pointer motion, one second
    mouseUp(g);
    auto v = g.rotation().angularVelocity();
    CHECK(std::fabs(v.x) < 1e-2f);
    CHECK(std::fabs(v.y) < 1e-2f);
    CHECK(std::fabs(v.z) < 1e-2f);
}

// The frame loop does not tick during a stationary hold when nothing
// requests a redraw (yourworld2 leaves GlobeController::onRedraw unwired).
// Release is the next time anyone looks. The pre-hold flick must be gone.
TEST_CASE("GlobeController hold with no ticks then release has no spin") {
    ge::GlobeController g;
    steadyHorizontalFlick(g);
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    mouseUp(g);
    auto v = g.rotation().angularVelocity();
    CHECK(std::fabs(v.x) < 1e-2f);
    CHECK(std::fabs(v.y) < 1e-2f);
    CHECK(std::fabs(v.z) < 1e-2f);
}
