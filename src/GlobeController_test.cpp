#include <doctest.h>
#include <ge/GlobeController.h>

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
