// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/sdl_input.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_init.h>

using ge::PointerEvent;
using ge::kMouseId;
using ge::input::fromSdl;

namespace {

constexpr ge::la::float2 kSurface{1024.f, 768.f};

// Most tests build SDL_Events by hand. The mouse path needs a real
// SDL_Window (so SDL_GetWindowFromID resolves), so a dedicated
// fixture handles that lifecycle.

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Non-pointer events return nullopt.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("fromSdl: non-pointer event returns nullopt") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_KEY_DOWN;
    CHECK_FALSE(fromSdl(ev, kSurface).has_value());
}

TEST_CASE("fromSdl: SDL_EVENT_QUIT returns nullopt") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_QUIT;
    CHECK_FALSE(fromSdl(ev, kSurface).has_value());
}

// ─────────────────────────────────────────────────────────────────────
// Finger events — pure math, no window lookup needed.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("fromSdl: SDL_EVENT_FINGER_DOWN denormalizes to surface pts") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_FINGER_DOWN;
    ev.tfinger.x = 0.5f;
    ev.tfinger.y = 0.25f;
    ev.tfinger.fingerID = 42;

    auto pe = fromSdl(ev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->kind == PointerEvent::Down);
    CHECK(pe->pos.x == doctest::Approx(512.f));
    CHECK(pe->pos.y == doctest::Approx(192.f));
    CHECK(pe->id == SDL_FingerID(42));
}

TEST_CASE("fromSdl: SDL_EVENT_FINGER_UP maps to PointerEvent::Up") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_FINGER_UP;
    ev.tfinger.x = 1.0f;
    ev.tfinger.y = 1.0f;
    ev.tfinger.fingerID = 1;

    auto pe = fromSdl(ev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->kind == PointerEvent::Up);
    CHECK(pe->pos.x == doctest::Approx(1024.f));
    CHECK(pe->pos.y == doctest::Approx(768.f));
    CHECK(pe->id == SDL_FingerID(1));
}

TEST_CASE("fromSdl: SDL_EVENT_FINGER_MOTION maps to PointerEvent::Move") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_FINGER_MOTION;
    ev.tfinger.x = 0.0f;
    ev.tfinger.y = 0.0f;
    ev.tfinger.fingerID = 7;

    auto pe = fromSdl(ev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->kind == PointerEvent::Move);
    CHECK(pe->pos.x == 0.f);
    CHECK(pe->pos.y == 0.f);
    CHECK(pe->id == SDL_FingerID(7));
}

TEST_CASE("fromSdl: finger fingerID is preserved verbatim") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_FINGER_DOWN;
    ev.tfinger.fingerID = 999999;
    auto pe = fromSdl(ev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->id == SDL_FingerID(999999));
}

// ─────────────────────────────────────────────────────────────────────
// Touch-synthetic mouse events are filtered.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("fromSdl: mouse-button event from SDL_TOUCH_MOUSEID returns nullopt") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ev.button.which = SDL_TOUCH_MOUSEID;
    ev.button.x = 100;
    ev.button.y = 100;
    CHECK_FALSE(fromSdl(ev, kSurface).has_value());

    ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
    CHECK_FALSE(fromSdl(ev, kSurface).has_value());
}

TEST_CASE("fromSdl: mouse-motion event from SDL_TOUCH_MOUSEID returns nullopt") {
    SDL_Event ev{};
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.which = SDL_TOUCH_MOUSEID;
    ev.motion.x = 100;
    ev.motion.y = 100;
    CHECK_FALSE(fromSdl(ev, kSurface).has_value());
}

// ─────────────────────────────────────────────────────────────────────
// Mouse events — require a real SDL_Window for the point→pixel scale.
//
// Inside a single TEST_CASE so we can REQUIRE the SDL init and the
// window creation without affecting unrelated tests.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("fromSdl: mouse events use SDL window coords (pt) and synthesise kMouseId") {
    // After 🎯T60, PointerEvent.pos is in point space. SDL mouse events
    // carry window-point coords directly — no pixel scaling is applied.
    // The window parameter is kept to ensure fromSdl still accepts a
    // valid windowID without crashing, but the coords should be verbatim.
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window* win = SDL_CreateWindow("ge-test", 200, 100, 0);
    REQUIRE(win != nullptr);
    const SDL_WindowID wid = SDL_GetWindowID(win);

    SDL_Event ev{};
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ev.button.which = 1;   // some non-touch mouse ID
    ev.button.windowID = wid;
    ev.button.x = 50.f;
    ev.button.y = 25.f;

    auto pe = fromSdl(ev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->kind == PointerEvent::Down);
    CHECK(pe->id == kMouseId);
    // Coords are window points — no px scaling applied (🎯T60).
    CHECK(pe->pos.x == doctest::Approx(50.f));
    CHECK(pe->pos.y == doctest::Approx(25.f));

    // MOUSE_UP shares the path.
    ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
    pe = fromSdl(ev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->kind == PointerEvent::Up);
    CHECK(pe->id == kMouseId);

    // MOTION: same — raw window-point coords.
    SDL_Event mev{};
    mev.type = SDL_EVENT_MOUSE_MOTION;
    mev.motion.which = 1;
    mev.motion.windowID = wid;
    mev.motion.x = 100.f;
    mev.motion.y = 50.f;
    pe = fromSdl(mev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->kind == PointerEvent::Move);
    CHECK(pe->id == kMouseId);
    CHECK(pe->pos.x == doctest::Approx(100.f));
    CHECK(pe->pos.y == doctest::Approx(50.f));

    SDL_DestroyWindow(win);
    SDL_Quit();
}

TEST_CASE("fromSdl: unknown windowID returns raw window-point coords") {
    // After 🎯T60, mouse coords are window pts — no scaling regardless of
    // whether the windowID resolves or not.
    SDL_Event ev{};
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ev.button.which = 1;
    ev.button.windowID = 99999;   // not a real window
    ev.button.x = 123.f;
    ev.button.y = 45.f;

    auto pe = fromSdl(ev, kSurface);
    REQUIRE(pe.has_value());
    CHECK(pe->pos.x == doctest::Approx(123.f));
    CHECK(pe->pos.y == doctest::Approx(45.f));
}
