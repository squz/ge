// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/ortho.h>

#include <cmath>

using ge::Rect;
using ge::la::float2;
using ge::la::float4;
using ge::la::float4x4;

// ─────────────────────────────────────────────────────────────────────
// letterbox + screenToContent: round-trip property
// ─────────────────────────────────────────────────────────────────────

namespace {

// Apply M to (p.x, p.y, 0, 1) and return the (x, y) pair.
float2 apply(const float4x4& m, float2 p) {
    auto v = ge::la::mul(m, float4{p.x, p.y, 0.f, 1.f});
    return {v.x, v.y};
}

constexpr float kEps = 1e-4f;

} // namespace

TEST_CASE("ortho::letterbox: square content in landscape screen pillarboxes") {
    // 320×320 square content into 640×320 screen → pillarbox.
    // Visible content rect extends to (-160, 0)..(480, 320) so the
    // square content (0,0)..(320,320) sits centered with extra space
    // on each side.
    const float2 content{320, 320};
    const float2 screen{640, 320};
    const auto m = ge::ortho::letterbox(content, screen);

    // Content corner (0, 0) maps to clip coordinate where x is
    // partway across the screen (not -1, since the screen extends
    // further left). visible.x = -160, visible.w = 640 → x=0 maps to
    // clip x = 2*(0 - (-160))/640 - 1 = 0.5 - 1 = -0.5.
    auto p = apply(m, {0, 0});
    CHECK(p.x == doctest::Approx(-0.5f).epsilon(kEps));
    CHECK(p.y == doctest::Approx( 1.0f).epsilon(kEps));  // top edge

    // Content (160, 160) is the center → clip (0, 0).
    p = apply(m, {160, 160});
    CHECK(p.x == doctest::Approx(0.0f).epsilon(kEps));
    CHECK(p.y == doctest::Approx(0.0f).epsilon(kEps));

    // Content (320, 320) → clip (0.5, -1).
    p = apply(m, {320, 320});
    CHECK(p.x == doctest::Approx( 0.5f).epsilon(kEps));
    CHECK(p.y == doctest::Approx(-1.0f).epsilon(kEps));
}

TEST_CASE("ortho::letterbox: tall screen letterboxes") {
    // 320×320 into 320×640 → letterbox (extra above and below).
    const float2 content{320, 320};
    const float2 screen{320, 640};
    const auto m = ge::ortho::letterbox(content, screen);

    // Center stays at clip (0, 0).
    auto p = apply(m, {160, 160});
    CHECK(p.x == doctest::Approx(0.0f).epsilon(kEps));
    CHECK(p.y == doctest::Approx(0.0f).epsilon(kEps));

    // Content top edge (y=0) should be at clip y = 0.5
    // (visible rect: y in -160..480, so 0 → 1 - 2*(0-(-160))/640 = 0.5).
    p = apply(m, {160, 0});
    CHECK(p.y == doctest::Approx(0.5f).epsilon(kEps));
}

TEST_CASE("ortho::letterbox: matching aspect collapses to identity-style mapping") {
    // 320×480 into 640×960 (same 2:3 aspect) → no letterbox/pillarbox.
    // Content corners hit clip-space corners exactly.
    const float2 content{320, 480};
    const float2 screen{640, 960};
    const auto m = ge::ortho::letterbox(content, screen);

    auto tl = apply(m, {0, 0});
    auto br = apply(m, {320, 480});
    CHECK(tl.x == doctest::Approx(-1.0f).epsilon(kEps));
    CHECK(tl.y == doctest::Approx( 1.0f).epsilon(kEps));
    CHECK(br.x == doctest::Approx( 1.0f).epsilon(kEps));
    CHECK(br.y == doctest::Approx(-1.0f).epsilon(kEps));
}

TEST_CASE("ortho::screenToContent inverts letterbox round-trip on screen midpoint") {
    // Screen midpoint always maps to content midpoint.
    const float2 content{320, 480};
    const float2 screen{1024, 768};
    auto p = ge::ortho::screenToContent({512, 384}, content, screen);
    CHECK(p.x == doctest::Approx(160.f).epsilon(kEps));
    CHECK(p.y == doctest::Approx(240.f).epsilon(kEps));
}

TEST_CASE("ortho::screenToContent inverts letterbox round-trip on screen corners") {
    // Wider screen pillarboxes → screen left edge maps to content x < 0.
    const float2 content{320, 480};
    const float2 screen{1024, 768};

    // 1024×768 vs 320×480: screenAspect 4:3 > contentAspect 2:3 → pillarbox.
    // visible.x = -extra where extra = (1024*480 - 768*320) / (2*768)
    //                                = (491520 - 245760) / 1536
    //                                = 245760 / 1536 = 160.
    // Screen x=0 → content x = -160; screen x=1024 → content x = 480.
    auto tl = ge::ortho::screenToContent({0, 0}, content, screen);
    auto br = ge::ortho::screenToContent({1024, 768}, content, screen);
    CHECK(tl.x == doctest::Approx(-160.f).epsilon(kEps));
    CHECK(tl.y == doctest::Approx(   0.f).epsilon(kEps));
    CHECK(br.x == doctest::Approx( 480.f).epsilon(kEps));
    CHECK(br.y == doctest::Approx( 480.f).epsilon(kEps));
}

TEST_CASE("ortho::screenToContent + letterbox round-trip preserves arbitrary screen points") {
    // For any screen point sp, applying letterbox to screenToContent(sp)
    // should produce the same NDC as sp's NDC computed directly.
    const float2 content{320, 480};
    const float2 screen{1024, 768};
    const auto m = ge::ortho::letterbox(content, screen);

    for (float sx : {0.f, 256.f, 512.f, 800.f, 1024.f}) {
        for (float sy : {0.f, 192.f, 384.f, 576.f, 768.f}) {
            auto cp = ge::ortho::screenToContent({sx, sy}, content, screen);
            auto ndc = apply(m, cp);
            // Direct NDC of (sx, sy): clip-space top-left = (-1, 1).
            // ndc_x = 2*sx/screen.x - 1; ndc_y = 1 - 2*sy/screen.y.
            const float ex = 2.f * sx / screen.x - 1.f;
            const float ey = 1.f - 2.f * sy / screen.y;
            CHECK(ndc.x == doctest::Approx(ex).epsilon(kEps));
            CHECK(ndc.y == doctest::Approx(ey).epsilon(kEps));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// pixelOrtho
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("ortho::pixelOrtho maps top-left to clip (-1, 1)") {
    const auto m = ge::ortho::pixelOrtho(800, 600);
    auto p = apply(m, {0, 0});
    CHECK(p.x == doctest::Approx(-1.f).epsilon(kEps));
    CHECK(p.y == doctest::Approx( 1.f).epsilon(kEps));
}

TEST_CASE("ortho::pixelOrtho maps bottom-right to clip (1, -1)") {
    const auto m = ge::ortho::pixelOrtho(800, 600);
    auto p = apply(m, {800, 600});
    CHECK(p.x == doctest::Approx( 1.f).epsilon(kEps));
    CHECK(p.y == doctest::Approx(-1.f).epsilon(kEps));
}

TEST_CASE("ortho::pixelOrtho maps center to clip origin") {
    const auto m = ge::ortho::pixelOrtho(800, 600);
    auto p = apply(m, {400, 300});
    CHECK(p.x == doctest::Approx(0.f).epsilon(kEps));
    CHECK(p.y == doctest::Approx(0.f).epsilon(kEps));
}

// ─────────────────────────────────────────────────────────────────────
// constexpr smoke tests (compile-time evaluation)
// ─────────────────────────────────────────────────────────────────────

namespace {
constexpr auto kLBMatrix    = ge::ortho::letterbox({320, 480}, {640, 960});
constexpr auto kPxMatrix    = ge::ortho::pixelOrtho(800, 600);
constexpr auto kS2C         = ge::ortho::screenToContent({512, 384}, {320, 480}, {1024, 768});

// Same-aspect content → corners map to clip-space corners exactly.
static_assert(kLBMatrix[0][0] == 2.f / 320.f);
static_assert(kPxMatrix[0][0] == 2.f / 800.f);
static_assert(kS2C.x          == 160.f);
static_assert(kS2C.y          == 240.f);
} // namespace

// ─────────────────────────────────────────────────────────────────────
// tilt (🎯T94): perspective tilt composed onto a base projection
// ─────────────────────────────────────────────────────────────────────

namespace {
// Apply M to (p.x, p.y, 0, 1) WITH the perspective w-divide → NDC (x, y).
float2 applyNdc(const float4x4& m, float2 p) {
    auto v = ge::la::mul(m, float4{p.x, p.y, 0.f, 1.f});
    return {v.x / v.w, v.y / v.w};
}
} // namespace

TEST_CASE("ortho::tilt: zero tilt is the identity") {
    const auto m = ge::ortho::tilt(1.5f, {0.f, 0.f});
    for (float2 p : {float2{0, 0}, float2{1, 1}, float2{-1, 0.5f}, float2{0.25f, -1}}) {
        auto q = applyNdc(m, p);
        CHECK(q.x == doctest::Approx(p.x).epsilon(kEps));
        CHECK(q.y == doctest::Approx(p.y).epsilon(kEps));
    }
}

TEST_CASE("ortho::tilt: rotated quad stays inside the screen and fills it (bbox fit)") {
    const auto m = ge::ortho::tilt(1.0f, {0.3f, 0.0f});  // ~17° about the y-axis
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (float2 c : {float2{-1, -1}, float2{1, -1}, float2{1, 1}, float2{-1, 1}}) {
        auto q = applyNdc(m, c);
        CHECK(q.x >= -1.f - kEps);
        CHECK(q.x <=  1.f + kEps);
        CHECK(q.y >= -1.f - kEps);
        CHECK(q.y <=  1.f + kEps);
        minX = std::min(minX, q.x); maxX = std::max(maxX, q.x);
        minY = std::min(minY, q.y); maxY = std::max(maxY, q.y);
    }
    // The fit scales the rotated quad until its bbox fills [-1,1] on the
    // binding axis — so at least one axis spans the full 2.0.
    const bool fillsX = std::abs((maxX - minX) - 2.f) < 1e-3f;
    const bool fillsY = std::abs((maxY - minY) - 2.f) < 1e-3f;
    CHECK((fillsX || fillsY));
}

TEST_CASE("ortho::tilt: +x presentation tilt recedes the right edge (sign convention)") {
    // tiltRad = (+angle, 0) → axis (0,1,0): the right edge (x=+1) rotates away
    // from the camera, so it should project SHORTER than the left edge. This
    // pins the convention ported from the pre-T38 player composite.
    const auto m = ge::ortho::tilt(1.0f, {0.4f, 0.0f});
    const float leftH  = applyNdc(m, {-1, 1}).y - applyNdc(m, {-1, -1}).y;
    const float rightH = applyNdc(m, { 1, 1}).y - applyNdc(m, { 1, -1}).y;
    CHECK(std::abs(leftH) > std::abs(rightH));
}
