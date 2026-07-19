// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T156.3 primary-seat policy unit tests (shipped SeatPolicy).

#include <ge/SeatPolicy.h>

#include <doctest.h>

using ge::SeatPolicy;

namespace {

SDL_Event sensorEvent(float gx) {
    SDL_Event e{};
    e.type = SDL_EVENT_SENSOR_UPDATE;
    e.sensor.data[0] = gx;
    e.sensor.data[1] = 0.f;
    return e;
}

SDL_Event motionEvent() {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = 10.f;
    return e;
}

} // namespace

TEST_CASE("SeatPolicy: first attach is primary") {
    SeatPolicy seats;
    const int a = 1, b = 2;
    seats.onAttach(&a);
    seats.onAttach(&b);
    CHECK(seats.isPrimary(&a));
    CHECK_FALSE(seats.isPrimary(&b));
    CHECK(seats.acceptDeviceInfo(&a));
    CHECK_FALSE(seats.acceptDeviceInfo(&b));
    CHECK(seats.acceptSafeArea(&a));
    CHECK_FALSE(seats.acceptSafeArea(&b));
}

TEST_CASE("SeatPolicy: only primary may inject sensors or input") {
    SeatPolicy seats;
    const int desk = 10, phone = 20;
    seats.onAttach(&desk);
    seats.onAttach(&phone);
    CHECK(seats.acceptSdlEvent(&desk, sensorEvent(1.f)));
    CHECK(seats.acceptSdlEvent(&desk, motionEvent()));
    CHECK_FALSE(seats.acceptSdlEvent(&phone, sensorEvent(-9.f)));
    CHECK_FALSE(seats.acceptSdlEvent(&phone, motionEvent()));
}

TEST_CASE("SeatPolicy: attach order desktop-then-mobile keeps desktop seat") {
    SeatPolicy seats;
    const int desktop = 1, mobile = 2;
    seats.onAttach(&desktop); // first
    seats.onAttach(&mobile);
    // Mobile must not retarget surface or steal AccelSynth.
    CHECK_FALSE(seats.acceptDeviceInfo(&mobile));
    CHECK_FALSE(seats.acceptSdlEvent(&mobile, sensorEvent(5.f)));
    CHECK(seats.acceptDeviceInfo(&desktop));
    CHECK(seats.acceptSdlEvent(&desktop, motionEvent()));
}

TEST_CASE("SeatPolicy: detach all clears primary") {
    SeatPolicy seats;
    const int a = 1;
    seats.onAttach(&a);
    seats.onDetachAll();
    CHECK(seats.primary() == nullptr);
    seats.onAttach(&a);
    CHECK(seats.isPrimary(&a));
}

TEST_CASE("SeatPolicy: primary detach promotes explicitly (🎯T156.3)") {
    ge::SeatPolicy seats;
    const int a = 1, b = 2, c = 3;
    seats.onAttach(&a);
    seats.onAttach(&b);
    seats.onAttach(&c);
    REQUIRE(seats.isPrimary(&a));

    // Spectator detach does not disturb the seat.
    CHECK_FALSE(seats.onDetach(&b));
    CHECK(seats.isPrimary(&a));

    // Primary detach reports loss; caller promotes the eldest survivor.
    CHECK(seats.onDetach(&a));
    CHECK(seats.primary() == nullptr);
    seats.promote(&c);
    CHECK(seats.isPrimary(&c));

    // Promoted seat now owns DeviceInfo/input authority.
    CHECK(seats.acceptDeviceInfo(&c));
    SDL_Event ev{};
    ev.type = SDL_EVENT_SENSOR_UPDATE;
    CHECK(seats.acceptSdlEvent(&c, ev));
}
