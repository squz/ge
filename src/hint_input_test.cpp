// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T177 InputDriver — the hint timeline drives synthetic touch input.
// No GPU, no SDL init: the driver's sink is captured directly, and the
// production-path proof routes captured events through ge::input::fromSdl
// into a real ge::Button.

#include <ge/button.h>
#include <ge/hint.h>
#include <ge/hint_input.h>
#include <ge/sdl_input.h>

#include <doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ge;
using namespace ge::hint;

namespace {

constexpr float kStep = 1.0f / 120.0f;

struct Harness {
    Context     ctx{800, 600, DeviceClass::Desktop, ":memory:", ""};
    Rect        area{100, 100, 400, 300};  // hint area within the 800x600 surface
    Player      player;
    InputDriver driver;
    std::vector<SDL_Event> events;

    explicit Harness(Kind kind, Params p = {}) : player(kind, p) {
        driver.sink = [this](const SDL_Event& e) { events.push_back(e); };
    }
    void run(float seconds) {
        for (float t = 0; t < seconds; t += kStep) {
            player.update(kStep);
            driver.update(ctx, player, area);
        }
    }
    int count(uint32_t type) const {
        int n = 0;
        for (const auto& e : events) n += e.type == type;
        return n;
    }
};

Params noLoop() {
    Params p;
    p.loop = false;
    return p;
}

}  // namespace

TEST_CASE("🎯T177 tap: one synthetic down/up pair at the mapped position") {
    Params p  = noLoop();
    p.from    = {0.5f, 0.5f};
    Harness h(Kind::Tap, p);
    h.run(h.player.duration() + 0.1f);

    REQUIRE(h.count(SDL_EVENT_FINGER_DOWN) == 1);
    REQUIRE(h.count(SDL_EVENT_FINGER_UP) == 1);

    const auto& down = h.events.front();
    CHECK(down.type == SDL_EVENT_FINGER_DOWN);
    CHECK(down.tfinger.touchID == kSyntheticTouchId);
    CHECK(isSyntheticFinger(down.tfinger.fingerID));
    // unit (0.5, 0.5) of area {100,100,400,300} = (300, 250) pts on the
    // 800x600 surface = normalized (0.375, 0.4167).
    CHECK(down.tfinger.x == doctest::Approx(300.0f / 800.0f));
    CHECK(down.tfinger.y == doctest::Approx(250.0f / 600.0f));
}

TEST_CASE("🎯T177 swipe: down at from, motions advance monotonically, up at to") {
    Params p = noLoop();
    p.from   = {0.2f, 0.5f};
    p.to     = {0.8f, 0.5f};
    Harness h(Kind::Swipe, p);
    h.run(h.player.duration() + 0.1f);

    REQUIRE(h.count(SDL_EVENT_FINGER_DOWN) == 1);
    REQUIRE(h.count(SDL_EVENT_FINGER_UP) == 1);
    CHECK(h.count(SDL_EVENT_FINGER_MOTION) > 5);  // interpolated stroke

    float prevX = -1.0f;
    bool monotone = true;
    for (const auto& e : h.events) {
        if (e.type != SDL_EVENT_FINGER_MOTION) continue;
        monotone = monotone && e.tfinger.x >= prevX;
        prevX = e.tfinger.x;
    }
    CHECK(monotone);

    const auto& up = h.events.back();
    CHECK(up.type == SDL_EVENT_FINGER_UP);
    // to = unit (0.8, 0.5) → (420, 250) pts → normalized x = 0.525.
    CHECK(up.tfinger.x == doctest::Approx((100 + 0.8f * 400) / 800.0f).epsilon(0.02));
}

TEST_CASE("🎯T177 pinch: two distinct synthetic fingers, both down then both up") {
    Harness h(Kind::PinchZoom, noLoop());
    h.run(h.player.duration() + 0.1f);

    CHECK(h.count(SDL_EVENT_FINGER_DOWN) == 2);
    CHECK(h.count(SDL_EVENT_FINGER_UP) == 2);

    SDL_FingerID ids[2] = {0, 0};
    int n = 0;
    for (const auto& e : h.events)
        if (e.type == SDL_EVENT_FINGER_DOWN && n < 2) ids[n++] = e.tfinger.fingerID;
    REQUIRE(n == 2);
    CHECK(ids[0] != ids[1]);
    CHECK(isSyntheticFinger(ids[0]));
    CHECK(isSyntheticFinger(ids[1]));
}

TEST_CASE("🎯T177 cancel lifts a held finger cleanly") {
    Harness h(Kind::LongPress, noLoop());
    h.run(0.8f);  // inside the hold
    CHECK(h.driver.active());
    h.driver.cancel();
    CHECK(!h.driver.active());
    CHECK(h.events.back().type == SDL_EVENT_FINGER_UP);
    // No further events without new contact.
    const size_t n = h.events.size();
    h.run(0.05f);  // player still holding, but finger already lifted...
    // ...so the driver re-presses: contact true + not down ⇒ DOWN again.
    // That is the documented resume semantics; assert it explicitly.
    CHECK(h.events.size() > n);
    CHECK(h.events[n].type == SDL_EVENT_FINGER_DOWN);
}

TEST_CASE("🎯T177 looping clip re-fires a pair per cycle") {
    Params p  = {};
    p.loop    = true;
    p.loopGap = 0.1f;
    Harness h(Kind::Tap, p);
    const float cycle = h.player.duration() + p.loopGap;
    h.run(2.0f * cycle + 0.05f);
    CHECK(h.count(SDL_EVENT_FINGER_DOWN) >= 2);
    CHECK(h.count(SDL_EVENT_FINGER_UP) >= 2);
}

TEST_CASE("🎯T177 production path: a driven tap fires a real ge::Button") {
    // Button at the tap target, dispatched through the exact fromSdl →
    // handleEvent path the game uses — zero correlation code.
    Params p = noLoop();
    p.from   = {0.5f, 0.5f};
    Harness h(Kind::Tap, p);

    Button btn;
    int    fired = 0;
    btn.onFire = [&] { ++fired; };
    // Tap lands at (300, 250) pts; a 44pt button centered there.
    btn.setHitRect(Rect{278, 228, 44, 44});

    h.driver.sink = [&](const SDL_Event& e) {
        if (auto pe = ge::input::fromSdl(e, {800.0f, 600.0f}))
            btn.handleEvent(*pe);
    };
    h.run(h.player.duration() + 0.1f);
    CHECK(fired == 1);
}
