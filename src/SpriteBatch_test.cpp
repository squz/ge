// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/SpriteBatch.h>

#include <doctest.h>

// Note: submit() is not tested here because it requires a live bgfx context.
// The tests below verify the vertex-buffer geometry produced by addSprite /
// addQuad by reaching into the internal sprite vector via the friend accessor
// defined below.

// A friend shim that exposes the internal sprites_ vector for testing.
// Defined here so it doesn't pollute production headers.
namespace ge {
struct SpriteBatchTestAccess {
    static const std::vector<SpriteBatch::Sprite>& sprites(const SpriteBatch& b) {
        return b.sprites_;
    }
};
} // namespace ge

using ge::SpriteBatch;
using ge::SpriteBatchTestAccess;
using ge::Rect;
using ge::SpriteVertex;

// Fake texture handle — we don't init bgfx, so we construct one directly.
static bgfx::TextureHandle fakeTexHandle(uint16_t idx) {
    bgfx::TextureHandle h;
    h.idx = idx;
    return h;
}

TEST_CASE("SpriteBatch starts empty") {
    SpriteBatch batch;
    CHECK(SpriteBatchTestAccess::sprites(batch).empty());
}

TEST_CASE("SpriteBatch clear empties the queue") {
    SpriteBatch batch;
    batch.addSprite(0, 0, 10, 10, fakeTexHandle(1), 0, 0, 1, 1);
    batch.clear();
    CHECK(SpriteBatchTestAccess::sprites(batch).empty());
}

TEST_CASE("addSprite with invalid tex is a no-op") {
    SpriteBatch batch;
    bgfx::TextureHandle invalid = BGFX_INVALID_HANDLE;
    batch.addSprite(0, 0, 10, 10, invalid, 0, 0, 1, 1);
    CHECK(SpriteBatchTestAccess::sprites(batch).empty());
}

TEST_CASE("addSprite produces correct vertex positions") {
    SpriteBatch batch;
    batch.addSprite(100, 200, 50, 30, fakeTexHandle(7),
                    0.f, 0.f, 1.f, 1.f);
    const auto& sprites = SpriteBatchTestAccess::sprites(batch);
    REQUIRE(sprites.size() == 1);
    const auto& v = sprites[0].verts;

    // Quad spans [50,150] x [170,230] (cx±hw, cy±hh).
    // Two CCW triangles: bl,br,tr,bl,tr,tl
    CHECK(v[0].x == doctest::Approx(50.f));   // bl
    CHECK(v[0].y == doctest::Approx(170.f));
    CHECK(v[1].x == doctest::Approx(150.f));  // br
    CHECK(v[1].y == doctest::Approx(170.f));
    CHECK(v[2].x == doctest::Approx(150.f));  // tr
    CHECK(v[2].y == doctest::Approx(230.f));
    CHECK(v[3].x == doctest::Approx(50.f));   // bl (again)
    CHECK(v[3].y == doctest::Approx(170.f));
    CHECK(v[4].x == doctest::Approx(150.f));  // tr (again)
    CHECK(v[4].y == doctest::Approx(230.f));
    CHECK(v[5].x == doctest::Approx(50.f));   // tl
    CHECK(v[5].y == doctest::Approx(230.f));
}

TEST_CASE("addSprite stores UVs correctly") {
    SpriteBatch batch;
    batch.addSprite(0, 0, 1, 1, fakeTexHandle(3),
                    0.1f, 0.2f, 0.8f, 0.9f);
    const auto& v = SpriteBatchTestAccess::sprites(batch)[0].verts;

    // bl: uvL, uvB
    CHECK(v[0].u == doctest::Approx(0.1f));
    CHECK(v[0].v == doctest::Approx(0.9f));
    // br: uvR, uvB
    CHECK(v[1].u == doctest::Approx(0.8f));
    CHECK(v[1].v == doctest::Approx(0.9f));
    // tr: uvR, uvT
    CHECK(v[2].u == doctest::Approx(0.8f));
    CHECK(v[2].v == doctest::Approx(0.2f));
    // tl: uvL, uvT
    CHECK(v[5].u == doctest::Approx(0.1f));
    CHECK(v[5].v == doctest::Approx(0.2f));
}

TEST_CASE("addSprite stores color on all vertices") {
    SpriteBatch batch;
    constexpr uint32_t kRed = 0xFF0000FFu;  // ABGR: A=FF, B=00, G=00, R=FF
    batch.addSprite(0, 0, 1, 1, fakeTexHandle(5), 0, 0, 1, 1, kRed);
    const auto& v = SpriteBatchTestAccess::sprites(batch)[0].verts;
    for (int i = 0; i < 6; ++i) {
        CHECK(v[i].abgr == kRed);
    }
}

TEST_CASE("addQuad maps dest rect and uv rect to addSprite vertices") {
    SpriteBatch batch;
    // dest: x=10, y=20, w=80, h=60  → cx=50, cy=50, hw=40, hh=30
    // uvs:  x=0.1, y=0.2, w=0.5, h=0.4 → uvL=0.1, uvT=0.2, uvR=0.6, uvB=0.6
    Rect dest{10, 20, 80, 60};
    Rect uvs{0.1f, 0.2f, 0.5f, 0.4f};
    batch.addQuad(dest, fakeTexHandle(9), uvs, 0xFFFFFFFF);
    const auto& v = SpriteBatchTestAccess::sprites(batch)[0].verts;

    // bl: cx-hw=10, cy-hh=20
    CHECK(v[0].x == doctest::Approx(10.f));
    CHECK(v[0].y == doctest::Approx(20.f));
    // tr: cx+hw=90, cy+hh=80
    CHECK(v[2].x == doctest::Approx(90.f));
    CHECK(v[2].y == doctest::Approx(80.f));

    // uvL=0.1, uvT=0.2, uvR=0.6, uvB=0.6
    CHECK(v[0].u == doctest::Approx(0.1f));  // bl.u = uvL
    CHECK(v[0].v == doctest::Approx(0.6f));  // bl.v = uvB
    CHECK(v[2].u == doctest::Approx(0.6f));  // tr.u = uvR
    CHECK(v[2].v == doctest::Approx(0.2f));  // tr.v = uvT
}

TEST_CASE("addQuad default uvs cover full texture") {
    SpriteBatch batch;
    Rect dest{0, 0, 100, 100};
    batch.addQuad(dest, fakeTexHandle(2));
    const auto& v = SpriteBatchTestAccess::sprites(batch)[0].verts;
    // bl: uvL=0, uvB=1
    CHECK(v[0].u == doctest::Approx(0.f));
    CHECK(v[0].v == doctest::Approx(1.f));
    // tr: uvR=1, uvT=0
    CHECK(v[2].u == doctest::Approx(1.f));
    CHECK(v[2].v == doctest::Approx(0.f));
}

TEST_CASE("addSprite z coordinate is zero") {
    SpriteBatch batch;
    batch.addSprite(5, 10, 3, 4, fakeTexHandle(11), 0, 0, 1, 1);
    const auto& v = SpriteBatchTestAccess::sprites(batch)[0].verts;
    for (int i = 0; i < 6; ++i) {
        CHECK(v[i].z == doctest::Approx(0.f));
    }
}

TEST_CASE("addSprite accumulates multiple sprites") {
    SpriteBatch batch;
    batch.addSprite(0, 0, 1, 1, fakeTexHandle(1), 0, 0, 1, 1);
    batch.addSprite(10, 0, 1, 1, fakeTexHandle(2), 0, 0, 1, 1);
    batch.addSprite(20, 0, 1, 1, fakeTexHandle(1), 0, 0, 1, 1);
    CHECK(SpriteBatchTestAccess::sprites(batch).size() == 3);
}
