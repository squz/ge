// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for RefreshRateBoost counter semantics (🎯T63).
//
// These tests exercise the counter logic only — platform-specific boost
// calls (CADisplayLink, JNI) are the desktop no-op stubs in this build,
// which makes the counter the observable under test.

#include <doctest.h>

#include <ge/RefreshRateBoost.h>

TEST_CASE("RefreshRateBoost: starts at zero") {
    ge::RefreshRateBoost boost;
    CHECK(boost.pressCount() == 0);
}

TEST_CASE("RefreshRateBoost: single engage/release round-trip") {
    ge::RefreshRateBoost boost;
    boost.engagePress();
    CHECK(boost.pressCount() == 1);
    boost.releasePress();
    CHECK(boost.pressCount() == 0);
}

TEST_CASE("RefreshRateBoost: multi-finger accumulation") {
    ge::RefreshRateBoost boost;
    boost.engagePress();   // finger 1 down
    boost.engagePress();   // finger 2 down
    boost.engagePress();   // finger 3 down
    CHECK(boost.pressCount() == 3);

    boost.releasePress();  // finger 1 up
    CHECK(boost.pressCount() == 2);

    boost.releasePress();  // finger 2 up
    CHECK(boost.pressCount() == 1);

    boost.releasePress();  // finger 3 up
    CHECK(boost.pressCount() == 0);
}

TEST_CASE("RefreshRateBoost: spurious release does not underflow") {
    ge::RefreshRateBoost boost;
    // Count starts at 0 — release with no prior engage.
    boost.releasePress();
    CHECK(boost.pressCount() == 0);  // clamped, not negative

    // A second spurious release also stays clamped.
    boost.releasePress();
    CHECK(boost.pressCount() == 0);
}

TEST_CASE("RefreshRateBoost: drainPresses resets to zero") {
    ge::RefreshRateBoost boost;
    boost.engagePress();
    boost.engagePress();
    CHECK(boost.pressCount() == 2);

    boost.drainPresses();
    CHECK(boost.pressCount() == 0);
}

TEST_CASE("RefreshRateBoost: drain on empty is a no-op") {
    ge::RefreshRateBoost boost;
    boost.drainPresses();
    CHECK(boost.pressCount() == 0);
}

TEST_CASE("RefreshRateBoost: engage after drain resumes from zero") {
    ge::RefreshRateBoost boost;
    boost.engagePress();
    boost.drainPresses();
    CHECK(boost.pressCount() == 0);

    boost.engagePress();
    CHECK(boost.pressCount() == 1);
    boost.releasePress();
    CHECK(boost.pressCount() == 0);
}

TEST_CASE("RefreshRateBoost: interleaved engage/release stays consistent") {
    ge::RefreshRateBoost boost;
    // Simulate: finger A down, finger B down, finger A up, finger B up.
    boost.engagePress();   // A down
    CHECK(boost.pressCount() == 1);
    boost.engagePress();   // B down
    CHECK(boost.pressCount() == 2);
    boost.releasePress();  // A up
    CHECK(boost.pressCount() == 1);
    boost.releasePress();  // B up
    CHECK(boost.pressCount() == 0);
}
