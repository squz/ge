// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/sprite.h>
#include <ge/transform.h>

#include <doctest.h>

// `submit()` is not tested here because it requires a live bgfx context.
// The tests below verify the vertex-buffer geometry produced by `addSprite`
// by reaching into the internal queue via the friend accessor below.

namespace ge {
struct SpriteBatchTestAccess {
    static const std::vector<SpriteBatch::Quad>& quads(const SpriteBatch& b) {
        return b.quads_;
    }
};
} // namespace ge

using ge::Rect;
using ge::Sprite;
using ge::SpriteBatch;
using ge::SpriteBatchTestAccess;

// Construct a fake Sprite with a non-null tex handle (we don't init sokol, so
// addSprite's null-check just sees the non-zero image id).
static Sprite fakeSprite(uint16_t idx, int w = 100, int h = 50) {
    Sprite s;
    s.tex.id = idx;
    s.width  = w;
    s.height = h;
    return s;
}

TEST_CASE("SpriteBatch starts empty") {
    SpriteBatch batch;
    CHECK(SpriteBatchTestAccess::quads(batch).empty());
}

TEST_CASE("SpriteBatch clear empties the queue") {
    SpriteBatch batch;
    batch.addSprite(ge::frame(Rect{0, 0, 1, 1}), fakeSprite(1));
    batch.clear();
    CHECK(SpriteBatchTestAccess::quads(batch).empty());
}

TEST_CASE("addSprite with null sprite is a no-op") {
    SpriteBatch batch;
    batch.addSprite(ge::frame(Rect{0, 0, 1, 1}), Sprite{});
    CHECK(SpriteBatchTestAccess::quads(batch).empty());
}

TEST_CASE("addSprite maps unit-square corners through the matrix") {
    SpriteBatch batch;
    // frame({10, 20, 80, 60}) maps unit-square (0..1)² to rect (10,20)..(90,80).
    // Vertex order: tl, tr, br, tl, br, bl
    batch.addSprite(ge::frame(Rect{10, 20, 80, 60}), fakeSprite(7));

    const auto& q = SpriteBatchTestAccess::quads(batch);
    REQUIRE(q.size() == 1);
    const auto& v = q[0].verts;

    // tl: (0,0) → (10, 20)
    CHECK(v[0].x == doctest::Approx(10.f));
    CHECK(v[0].y == doctest::Approx(20.f));
    // tr: (1,0) → (90, 20)
    CHECK(v[1].x == doctest::Approx(90.f));
    CHECK(v[1].y == doctest::Approx(20.f));
    // br: (1,1) → (90, 80)
    CHECK(v[2].x == doctest::Approx(90.f));
    CHECK(v[2].y == doctest::Approx(80.f));
    // bl: (0,1) → (10, 80)
    CHECK(v[5].x == doctest::Approx(10.f));
    CHECK(v[5].y == doctest::Approx(80.f));
}

TEST_CASE("addSprite default UVs cover full texture") {
    SpriteBatch batch;
    batch.addSprite(ge::frame(Rect{0, 0, 1, 1}), fakeSprite(2));
    const auto& v = SpriteBatchTestAccess::quads(batch)[0].verts;

    // tl → (0, 0); br → (1, 1)
    CHECK(v[0].u == doctest::Approx(0.f));
    CHECK(v[0].v == doctest::Approx(0.f));
    CHECK(v[2].u == doctest::Approx(1.f));
    CHECK(v[2].v == doctest::Approx(1.f));
}

TEST_CASE("addSprite UV sub-rect is stored correctly") {
    SpriteBatch batch;
    batch.addSprite(ge::frame(Rect{0, 0, 1, 1}), fakeSprite(3),
                    Rect{0.1f, 0.2f, 0.5f, 0.4f}, 0xFFFFFFFFu);
    const auto& v = SpriteBatchTestAccess::quads(batch)[0].verts;

    // tl → (uvL, uvT) = (0.1, 0.2)
    CHECK(v[0].u == doctest::Approx(0.1f));
    CHECK(v[0].v == doctest::Approx(0.2f));
    // tr → (uvR, uvT) = (0.6, 0.2)
    CHECK(v[1].u == doctest::Approx(0.6f));
    CHECK(v[1].v == doctest::Approx(0.2f));
    // br → (uvR, uvB) = (0.6, 0.6)
    CHECK(v[2].u == doctest::Approx(0.6f));
    CHECK(v[2].v == doctest::Approx(0.6f));
}

TEST_CASE("addSprite stores color on all vertices") {
    SpriteBatch batch;
    constexpr uint32_t kRed = 0xFF0000FFu;
    batch.addSprite(ge::frame(Rect{0, 0, 1, 1}), fakeSprite(5), kRed);
    const auto& v = SpriteBatchTestAccess::quads(batch)[0].verts;
    for (int i = 0; i < 6; ++i) {
        CHECK(v[i].abgr == kRed);
    }
}

TEST_CASE("addSprite z coordinate is zero") {
    SpriteBatch batch;
    batch.addSprite(ge::frame(Rect{5, 10, 3, 4}), fakeSprite(11));
    const auto& v = SpriteBatchTestAccess::quads(batch)[0].verts;
    for (int i = 0; i < 6; ++i) {
        CHECK(v[i].z == doctest::Approx(0.f));
    }
}

TEST_CASE("addSprite accumulates multiple sprites") {
    SpriteBatch batch;
    batch.addSprite(ge::frame(Rect{0,  0, 1, 1}), fakeSprite(1));
    batch.addSprite(ge::frame(Rect{10, 0, 1, 1}), fakeSprite(2));
    batch.addSprite(ge::frame(Rect{20, 0, 1, 1}), fakeSprite(1));
    CHECK(SpriteBatchTestAccess::quads(batch).size() == 3);
}
