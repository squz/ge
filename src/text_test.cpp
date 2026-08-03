// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/FontLoader.h>
#include <ge/text.h>

namespace {

struct Px { uint8_t r, g, b, a; };
Px pixelAt(const ge::TextPixels& p, int x, int y) {
    const uint8_t* base = p.rgba.data() +
        (static_cast<size_t>(y) * static_cast<size_t>(p.width) + static_cast<size_t>(x)) * 4;
    return Px{base[0], base[1], base[2], base[3]};
}

} // namespace

TEST_CASE("rasterizeTextToPixels: 'A' at 24pt produces valid dimensions and real ink") {
    ge::FontRef font;
    try { font = ge::resolveFont("system:sans-serif"); }
    catch (const std::exception&) { return; }  // skip on CI without fonts

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
    ge::FontRef font;
    try { font = ge::resolveFont("system:sans-serif"); }
    catch (const std::exception&) { return; }

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
    ge::FontRef font;
    try { font = ge::resolveFont("system:sans-serif"); }
    catch (const std::exception&) { return; }

    ge::TextPixels px = ge::rasterizeTextToPixels("", font, 24.0f, {1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(px.isNull());
}

// UTF-8 multi-byte must become one FreeType codepoint — not Latin-1 per byte
// (which rendered "Réunion" as "RÃ©union" / Ã + © for U+00E9).
TEST_CASE("rasterizeTextToPixels: UTF-8 multi-byte is one glyph not Latin-1 bytes") {
    ge::FontRef font;
    try { font = ge::resolveFont("system:sans-serif"); }
    catch (const std::exception&) { return; }

    const std::string e = "e";
    const std::string eAcute = "\xc3\xa9";  // UTF-8 é (U+00E9)
    const std::string mojibake = "\xc3\x83\xc2\xa9";  // Ã© if double-encoded nonsense

    ge::TextPixels pxE = ge::rasterizeTextToPixels(e, font, 24.0f, {1, 1, 1, 1});
    ge::TextPixels pxA = ge::rasterizeTextToPixels(eAcute, font, 24.0f, {1, 1, 1, 1});
    if (pxE.isNull() || pxA.isNull()) return;

    // One codepoint ≈ one letter width, not two (C3 + A9 as separate glyphs).
    CHECK(pxA.width < pxE.width * 1.85f);
    CHECK(pxA.width > pxE.width * 0.4f);

    // "Réunion" (7 letters) must be closer to "Reunion" than to 8-glyph mojibake width.
    ge::TextPixels reu = ge::rasterizeTextToPixels("R\xc3\xa9union", font, 24.0f, {1, 1, 1, 1});
    ge::TextPixels reuAscii = ge::rasterizeTextToPixels("Reunion", font, 24.0f, {1, 1, 1, 1});
    if (reu.isNull() || reuAscii.isNull()) return;
    CHECK(std::abs(reu.width - reuAscii.width) < reuAscii.width * 0.35f);
    (void)mojibake;
}

// ── 🎯T176 concurrency ──────────────────────────────────────────────
// TSan is not wired into this build lane (documented CI-infeasibility per
// the T176 acceptance), so this exercises the contract directly: many
// threads rasterizing concurrently — mixed strings, sizes, and both the
// path and memory faces — must produce byte-identical output to a serial
// reference. Pre-T176 this was a data race on the FT_Library lazy init,
// the face create/destroy, and the font-bytes cache.
#include <atomic>
#include <thread>

TEST_CASE("🎯T176: concurrent rasterizeTextToPixels matches serial output") {
    ge::FontRef font;
    try { font = ge::resolveFont("system:sans-serif"); }
    catch (...) { return; }  // headless CI without fonts: nothing to test

    struct Job { const char* text; float pt; };
    const Job jobs[] = {{"alpha", 18.f}, {"Bravo 42", 24.f},
                        {"charlie!", 13.f}, {"Δelta", 32.f}};

    // Serial reference (also warms the byte cache single-threaded).
    ge::TextPixels ref[4];
    for (int i = 0; i < 4; ++i)
        ref[i] = ge::rasterizeTextToPixels(jobs[i].text, font, jobs[i].pt,
                                           {1, 1, 1, 1});
    REQUIRE(!ref[0].isNull());

    constexpr int kThreads = 8, kIters = 25;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                const Job& j = jobs[(t + i) % 4];
                auto px = ge::rasterizeTextToPixels(j.text, font, j.pt, {1, 1, 1, 1});
                const auto& r = ref[(t + i) % 4];
                if (px.width != r.width || px.height != r.height ||
                    px.rgba != r.rgba)
                    mismatches.fetch_add(1);
            }
        });
    }
    for (auto& th : pool) th.join();
    CHECK(mismatches.load() == 0);
}
