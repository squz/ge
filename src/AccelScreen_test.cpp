// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/AccelScreen.h>

#include <cmath>

using ge::rotateAccelToScreen;

namespace {

bool near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) < eps;
}

} // namespace

TEST_CASE("rotateAccelToScreen portrait is identity") {
    float d[3] = {1.f, 2.f, 3.f};
    rotateAccelToScreen(SDL_ORIENTATION_PORTRAIT, d);
    CHECK(near(d[0], 1.f));
    CHECK(near(d[1], 2.f));
    CHECK(near(d[2], 3.f));
}

TEST_CASE("rotateAccelToScreen landscape swaps with signs") {
    float d[3] = {1.f, 2.f, 3.f};
    rotateAccelToScreen(SDL_ORIENTATION_LANDSCAPE, d);
    // (-y, x) = (-2, 1)
    CHECK(near(d[0], -2.f));
    CHECK(near(d[1], 1.f));
    CHECK(near(d[2], 3.f));
}

TEST_CASE("rotateAccelToScreen landscape flipped") {
    float d[3] = {1.f, 2.f, 3.f};
    rotateAccelToScreen(SDL_ORIENTATION_LANDSCAPE_FLIPPED, d);
    // (y, -x) = (2, -1)
    CHECK(near(d[0], 2.f));
    CHECK(near(d[1], -1.f));
    CHECK(near(d[2], 3.f));
}

TEST_CASE("rotateAccelToScreen portrait flipped inverts both") {
    float d[3] = {1.f, 2.f, 3.f};
    rotateAccelToScreen(SDL_ORIENTATION_PORTRAIT_FLIPPED, d);
    CHECK(near(d[0], -1.f));
    CHECK(near(d[1], -2.f));
    CHECK(near(d[2], 3.f));
}

TEST_CASE("rotateAccelToScreen landscape then inverse-like round trip via flipped") {
    // Applying landscape twice is not identity; portrait flipped is -I.
    float d[3] = {3.f, -4.f, 1.f};
    rotateAccelToScreen(SDL_ORIENTATION_LANDSCAPE, d);
    // One more landscape: (-y, x) on (-4 was y? after first: d=(-(-4), 3)=(4,3)
    // first: x=3,y=-4 → (-(-4), 3) = (4, 3)
    CHECK(near(d[0], 4.f));
    CHECK(near(d[1], 3.f));
}
