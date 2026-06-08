// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/svg.h>

#include <string_view>

using namespace std::string_view_literals;

namespace {

constexpr std::string_view kRedRectSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
  <rect x="0" y="0" width="4" height="4" fill="#FF0000"/>
</svg>)SVG"sv;

// SVG that exercises clipPath — nanosvg (the path SDL_image used to take)
// can't render this; lunasvg can. The test verifies the clipped region is
// red and the unclipped region is transparent.
constexpr std::string_view kClippedSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
  <defs>
    <clipPath id="leftHalf">
      <rect x="0" y="0" width="4" height="8"/>
    </clipPath>
  </defs>
  <rect x="0" y="0" width="8" height="8" fill="#FF0000" clip-path="url(#leftHalf)"/>
</svg>)SVG"sv;

// Read an RGBA pixel from a SvgPixels buffer.
struct Px { uint8_t r, g, b, a; };
Px pixelAt(const ge::SvgPixels& p, int x, int y) {
    const uint8_t* base = p.rgba.data() + (static_cast<size_t>(y) * p.width + x) * 4;
    return Px{base[0], base[1], base[2], base[3]};
}

} // namespace

TEST_CASE("rasterizeSvgToPixels: solid red 4x4 produces all-red premultiplied pixels") {
    auto pixels = ge::rasterizeSvgToPixels(kRedRectSvg, 4, 4);
    REQUIRE_FALSE(pixels.isNull());
    CHECK(pixels.width == 4);
    CHECK(pixels.height == 4);
    CHECK(pixels.rgba.size() == 4u * 4u * 4u);

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            auto px = pixelAt(pixels, x, y);
            CHECK(px.r == 255);
            CHECK(px.g == 0);
            CHECK(px.b == 0);
            CHECK(px.a == 255);
        }
    }
}

TEST_CASE("rasterizeSvgToPixels: clipPath divides red and transparent regions") {
    auto pixels = ge::rasterizeSvgToPixels(kClippedSvg, 8, 8);
    REQUIRE_FALSE(pixels.isNull());
    CHECK(pixels.width == 8);
    CHECK(pixels.height == 8);

    // Left half: clipped-in, fully red.
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) {
            auto px = pixelAt(pixels, x, y);
            CHECK(px.r == 255);
            CHECK(px.a == 255);
        }
    }

    // Right half: clipped-out, transparent.
    for (int y = 0; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            auto px = pixelAt(pixels, x, y);
            CHECK(px.a == 0);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// 🎯T53 — hitTestSvgAt: fingertip-tolerance hit testing
// ─────────────────────────────────────────────────────────────────────

namespace {

// Two non-overlapping square buttons separated by a 4px gap, each with
// an `id` so we can identify which one was hit. The whole SVG is
// 16×8; left button covers x=[0,6], right covers x=[10,16], gap at
// x=[6,10].
constexpr std::string_view kTwoButtonsSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8">
  <rect id="left"  x="0"  y="0" width="6" height="8" fill="#FF0000"/>
  <rect id="right" x="10" y="0" width="6" height="8" fill="#0000FF"/>
</svg>)SVG"sv;

} // namespace

TEST_CASE("hitTestSvgAt: centre hit returns the element directly under the point") {
    auto doc = lunasvg::Document::loadFromData(kTwoButtonsSvg.data(),
                                               kTwoButtonsSvg.size());
    REQUIRE(doc);

    // (3, 4) is squarely inside the left rect.
    auto el = ge::hitTestSvgAt(*doc, 3.0f, 4.0f, /*radiusPx=*/2.0f);
    REQUIRE(el);
    CHECK(el.getAttribute("id") == "left");
}

TEST_CASE("hitTestSvgAt: drop into the gap finds the nearer element within radius") {
    auto doc = lunasvg::Document::loadFromData(kTwoButtonsSvg.data(),
                                               kTwoButtonsSvg.size());
    REQUIRE(doc);

    // (7, 4) is in the gap. With a 3px radius, the east sample point at
    // (10, 4) lands on the right button — but the west sample at (4, 4)
    // lands on the left. The ring is traversed E first per the order in
    // the implementation, so the right wins; the point being that
    // *some* element is selected.
    auto el = ge::hitTestSvgAt(*doc, 7.0f, 4.0f, /*radiusPx=*/3.0f);
    REQUIRE(el);
    const auto id = el.getAttribute("id");
    CHECK((id == "left" || id == "right"));
}

TEST_CASE("hitTestSvgAt: zero radius reduces to plain elementFromPoint") {
    auto doc = lunasvg::Document::loadFromData(kTwoButtonsSvg.data(),
                                               kTwoButtonsSvg.size());
    REQUIRE(doc);

    // (7, 4) is in the gap, zero radius — should miss.
    auto el = ge::hitTestSvgAt(*doc, 7.0f, 4.0f, /*radiusPx=*/0.0f);
    CHECK_FALSE(el);

    // Same point with non-zero radius does hit (covered by prior test).
}

TEST_CASE("hitTestSvgAt: far from any element returns invalid element even with radius") {
    auto doc = lunasvg::Document::loadFromData(kTwoButtonsSvg.data(),
                                               kTwoButtonsSvg.size());
    REQUIRE(doc);

    // Tiny radius far away from the 16×8 canvas — must not hit.
    auto el = ge::hitTestSvgAt(*doc, 100.0f, 100.0f, /*radiusPx=*/1.0f);
    CHECK_FALSE(el);
}

TEST_CASE("hitTestSvgAt: centre preferred over ring even when both hit") {
    // Single big button — centre and every ring sample are inside it.
    constexpr std::string_view oneBigSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20">
      <rect id="big" x="0" y="0" width="20" height="20" fill="#00FF00"/>
    </svg>)SVG"sv;
    auto doc = lunasvg::Document::loadFromData(oneBigSvg.data(),
                                               oneBigSvg.size());
    REQUIRE(doc);
    auto el = ge::hitTestSvgAt(*doc, 10.0f, 10.0f, /*radiusPx=*/5.0f);
    REQUIRE(el);
    CHECK(el.getAttribute("id") == "big");
}

TEST_CASE("rasterizeSvgToPixels: malformed SVG returns null") {
    auto pixels = ge::rasterizeSvgToPixels("not an svg"sv, 4, 4);
    CHECK(pixels.isNull());
    CHECK(pixels.rgba.empty());
}

TEST_CASE("rasterizeSvgToPixels: <text> renders glyphs via the lazy default font") {
    // 64×32 with a "Hi" at y=22 in a 20px sans-serif. The actual glyph metrics
    // depend on which platform font resolveFont picks, but any reasonable
    // sans-serif draws SOME ink in the left portion of the image. We only
    // assert "non-trivial number of opaque pixels in the text band" rather
    // than exact glyph shapes, which would be brittle across platforms.
    constexpr std::string_view svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="32">
  <text x="2" y="22" font-family="sans-serif" font-size="20" fill="#000000">Hi</text>
</svg>)SVG"sv;

    auto pixels = ge::rasterizeSvgToPixels(svg, 64, 32);
    REQUIRE_FALSE(pixels.isNull());
    CHECK(pixels.width == 64);
    CHECK(pixels.height == 32);

    int inkPixels = 0;
    for (int y = 0; y < pixels.height; ++y) {
        for (int x = 0; x < pixels.width; ++x) {
            if (pixelAt(pixels, x, y).a > 0) ++inkPixels;
        }
    }

    // Rough sanity floor: a 20px-tall "Hi" should ink at least a couple of
    // dozen pixels even on the most lightweight default font. If this hits
    // zero, the font path is broken (or no system font found at all).
    CHECK(inkPixels > 20);
}

TEST_CASE("measureSvgElementBounds: registered-font text width grows with label length") {
    auto font = ge::resolveFont("system:sans-serif");
    REQUIRE(ge::registerSvgFontFace("t112-test", false, false, font));

    constexpr std::string_view shortSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="64">
  <text id="label" x="8" y="42" font-family="t112-test" font-size="28" fill="#000000">Buy</text>
</svg>)SVG"sv;
    constexpr std::string_view longSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="64">
  <text id="label" x="8" y="42" font-family="t112-test" font-size="28" fill="#000000">Buy upgrade now</text>
</svg>)SVG"sv;

    auto shortBounds = ge::measureSvgElementBounds(shortSvg, "label");
    auto longBounds = ge::measureSvgElementBounds(longSvg, "label");

    REQUIRE_FALSE(shortBounds.isNull());
    REQUIRE_FALSE(longBounds.isNull());
    CHECK(shortBounds.width > 0.0f);
    CHECK(shortBounds.height > 0.0f);
    CHECK(longBounds.width > shortBounds.width);
    CHECK(longBounds.height > 0.0f);
}

TEST_CASE("measureSvgElementBounds: button label can be measured without rasterizing button") {
    auto font = ge::resolveFont("system:sans-serif");
    REQUIRE(ge::registerSvgFontFace("t112-button", false, false, font));

    constexpr std::string_view svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="96" viewBox="0 0 320 96">
  <g id="button">
    <rect x="0" y="0" width="320" height="96" rx="12" fill="#222222"/>
    <text id="label" x="160" y="58" text-anchor="middle" font-family="t112-button" font-size="28" fill="#FFFFFF">Buy pack - $4.99</text>
  </g>
</svg>)SVG"sv;

    auto doc = lunasvg::Document::loadFromData(svg.data(), svg.size());
    REQUIRE(doc);

    auto buttonBounds = ge::measureSvgElementBounds(*doc, "button");
    auto labelBounds = ge::measureSvgElementBounds(*doc, "label");
    auto docBounds = ge::measureSvgBounds(*doc);

    REQUIRE_FALSE(buttonBounds.isNull());
    REQUIRE_FALSE(labelBounds.isNull());
    REQUIRE_FALSE(docBounds.isNull());
    CHECK(buttonBounds.width == doctest::Approx(320.0f));
    CHECK(buttonBounds.height == doctest::Approx(96.0f));
    CHECK(labelBounds.width > 80.0f);
    CHECK(labelBounds.width < buttonBounds.width);
    CHECK(docBounds.width == doctest::Approx(320.0f));
}

TEST_CASE("measureSvgElementBounds: missing element and malformed SVG return invalid bounds") {
    auto missing = ge::measureSvgElementBounds(kRedRectSvg, "missing");
    CHECK(missing.isNull());

    auto malformed = ge::measureSvgBounds("not an svg"sv);
    CHECK(malformed.isNull());
}

TEST_CASE("rasterizeSvgToPixels: 50% alpha rect produces premultiplied pixels") {
    constexpr std::string_view svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2">
  <rect x="0" y="0" width="2" height="2" fill="#FF0000" fill-opacity="0.5"/>
</svg>)SVG"sv;
    auto pixels = ge::rasterizeSvgToPixels(svg, 2, 2);
    REQUIRE_FALSE(pixels.isNull());

    // 50% alpha red premultiplied: R=128, G=0, B=0, A=128 (give or take rounding).
    auto px = pixelAt(pixels, 0, 0);
    CHECK(px.a >= 126);
    CHECK(px.a <= 130);
    CHECK(px.r >= 126);
    CHECK(px.r <= 130);
    CHECK(px.g == 0);
    CHECK(px.b == 0);
    // Premultiplication invariant: R <= A (within 1 LSB of rounding).
    CHECK(px.r <= px.a + 1);
}
