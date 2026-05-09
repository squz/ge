// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>
#include <ge/LoadTexture.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_image/SDL_image.h>

#include <cstdint>
#include <vector>

// ─────────────────────────────────────────────────────────────────────
// Premultiplied-alpha formula tests
//
// The formula used in LoadTexture.cpp for each channel C and alpha A:
//   out = (C * A + 127) / 255
// Test it in isolation — no bgfx required.
// ─────────────────────────────────────────────────────────────────────

namespace {

// Replicate the exact premultiplication formula from LoadTexture.cpp.
uint8_t premul(uint8_t c, uint8_t a) {
    return static_cast<uint8_t>((static_cast<uint32_t>(c) * a + 127u) / 255u);
}

} // namespace

TEST_CASE("loadTexture2D premul: fully opaque pixel is unchanged") {
    // Alpha=255: premul(C, 255) should equal C for all C.
    for (int c = 0; c <= 255; ++c) {
        CHECK(premul(static_cast<uint8_t>(c), 255) == static_cast<uint8_t>(c));
    }
}

TEST_CASE("loadTexture2D premul: fully transparent pixel zeroes RGB") {
    // Alpha=0: premul(C, 0) should equal 0 for all C.
    for (int c = 0; c <= 255; ++c) {
        CHECK(premul(static_cast<uint8_t>(c), 0) == 0u);
    }
}

TEST_CASE("loadTexture2D premul: 50% alpha halves channel value (within rounding)") {
    // premul(255, 128): 255*128 = 32640; (32640+127)/255 = 128 → 128.
    CHECK(premul(255, 128) == 128u);
    // premul(128, 128): 128*128 = 16384; (16384+127)/255 ≈ 64.
    CHECK(premul(128, 128) == 64u);
    // premul(0, 128): 0 → 0.
    CHECK(premul(0, 128) == 0u);
}

TEST_CASE("loadTexture2D premul: premultiplied invariant R <= A holds") {
    // For any C <= 255 and A <= 255, premul(C, A) <= A.
    // (C*A)/255 <= A because C/255 <= 1.
    for (int a = 0; a <= 255; ++a) {
        for (int c = 0; c <= 255; c += 17) { // spot-check to keep test fast
            CHECK(premul(static_cast<uint8_t>(c), static_cast<uint8_t>(a)) <=
                  static_cast<uint8_t>(a));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// SDL pixel round-trip: load a minimal in-memory PNG and verify that
// SDL_ConvertSurface + premultiplication produces the expected bytes.
// bgfx is NOT initialised in this test binary, so we test only the
// SDL/pixel layer — not the bgfx::createTexture2D call.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("loadTexture2D pixel layer: 2x2 half-alpha red PNG premultiplies correctly") {
    // Minimal 2×2 RGBA PNG where all pixels are red at 50% alpha
    // (R=255, G=0, B=0, A=128). Generated with Python (zlib + struct).
    // Each row: filter=0x00, then 2 RGBA pixels 0xff 0x00 0x00 0x80.
    static const uint8_t kPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xd0,
        0x00, 0xc2, 0x0c, 0x30, 0x06, 0x00, 0x38, 0xe8, 0x05, 0xfd, 0x6c, 0xdc,
        0xaf, 0xd7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
        0x60, 0x82,
    };

    SDL_IOStream* io = SDL_IOFromConstMem(kPng, sizeof(kPng));
    REQUIRE(io != nullptr);

    SDL_Surface* raw = IMG_Load_IO(io, true); // closes io
    REQUIRE(raw != nullptr);

    SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    REQUIRE(rgba != nullptr);

    // Check dimensions.
    CHECK(rgba->w == 2);
    CHECK(rgba->h == 2);

    // Read first pixel — before premultiplication.
    const uint8_t* p = static_cast<const uint8_t*>(rgba->pixels);
    const uint8_t r_raw = p[0], g_raw = p[1], b_raw = p[2], a_raw = p[3];

    // PNG encodes R=255, G=0, B=0, A=128.
    CHECK(r_raw == 255u);
    CHECK(g_raw == 0u);
    CHECK(b_raw == 0u);
    CHECK(a_raw == 128u);

    // Premultiply in the same way LoadTexture.cpp does.
    const uint8_t pr = static_cast<uint8_t>((static_cast<uint32_t>(r_raw) * a_raw + 127u) / 255u);
    const uint8_t pg = static_cast<uint8_t>((static_cast<uint32_t>(g_raw) * a_raw + 127u) / 255u);
    const uint8_t pb = static_cast<uint8_t>((static_cast<uint32_t>(b_raw) * a_raw + 127u) / 255u);

    // premul(255, 128) = (255*128+127)/255 = (32640+127)/255 = 32767/255 = 128
    CHECK(pr == 128u);
    CHECK(pg == 0u);
    CHECK(pb == 0u);
    // Invariant: premultiplied channel <= alpha.
    CHECK(pr <= a_raw);

    SDL_DestroySurface(rgba);
}

// ─────────────────────────────────────────────────────────────────────
// Error path: non-existent file returns BGFX_INVALID_HANDLE without
// crashing. No bgfx init required — the error exits before any bgfx call.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("loadTexture2D: non-existent file returns BGFX_INVALID_HANDLE") {
    bgfx::TextureHandle h = ge::loadTexture2D("/tmp/ge-test-does-not-exist-xyzzy.png");
    const bgfx::TextureHandle invalid = BGFX_INVALID_HANDLE;
    CHECK(h.idx == invalid.idx);
}
