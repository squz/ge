// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/FontLoader.h>
#include <ge/RasterizeText.h>

namespace {

struct Px { uint8_t r, g, b, a; };
Px pixelAt(const ge::TextPixels& p, int x, int y) {
    const uint8_t* base = p.rgba.data() +
        (static_cast<size_t>(y) * static_cast<size_t>(p.width) + static_cast<size_t>(x)) * 4;
    return Px{base[0], base[1], base[2], base[3]};
}

} // namespace

TEST_CASE("rasterizeTextToPixels: 'A' at 24pt produces valid dimensions and real ink") {
    ge::FontRef font = ge::resolveFont("system:sans-serif");
    // resolveFont may return empty on CI without fonts — skip gracefully.
    if (font.path.empty()) return;

    ge::TextPixels px = ge::rasterizeTextToPixels("A", font, 24.0f, {1.0f, 1.0f, 1.0f, 1.0f});

    REQUIRE_FALSE(px.isNull());
    CHECK(px.width > 0);
    CHECK(px.height > 0);

    // Count fully opaque pixels (alpha == 255) and fully transparent pixels
    // (alpha == 0). A real 'A' glyph must have both.
    int opaqueCount      = 0;
    int transparentCount = 0;
    for (int y = 0; y < px.height; ++y) {
        for (int x = 0; x < px.width; ++x) {
            uint8_t a = pixelAt(px, x, y).a;
            if (a == 255) ++opaqueCount;
            if (a == 0)   ++transparentCount;
        }
    }
    CHECK(opaqueCount > 0);
    CHECK(transparentCount > 0);
}

TEST_CASE("rasterizeTextToPixels: premultiplied alpha invariant (R <= A)") {
    ge::FontRef font = ge::resolveFont("system:sans-serif");
    if (font.path.empty()) return;

    // White text: R = G = B = A for any glyph alpha. Premul: r = alpha, a = alpha.
    ge::TextPixels px = ge::rasterizeTextToPixels("A", font, 24.0f, {1.0f, 1.0f, 1.0f, 1.0f});
    if (px.isNull()) return;

    for (int y = 0; y < px.height; ++y) {
        for (int x = 0; x < px.width; ++x) {
            auto p = pixelAt(px, x, y);
            // Premul invariant: each channel <= alpha (within 1 LSB of rounding).
            CHECK(p.r <= p.a + 1);
            CHECK(p.g <= p.a + 1);
            CHECK(p.b <= p.a + 1);
        }
    }
}

TEST_CASE("rasterizeTextToPixels: empty font path returns null") {
    ge::FontRef bad;  // empty path
    ge::TextPixels px = ge::rasterizeTextToPixels("A", bad, 24.0f, {1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(px.isNull());
}

TEST_CASE("rasterizeTextToPixels: empty string returns null") {
    ge::FontRef font = ge::resolveFont("system:sans-serif");
    if (font.path.empty()) return;

    ge::TextPixels px = ge::rasterizeTextToPixels("", font, 24.0f, {1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(px.isNull());
}
