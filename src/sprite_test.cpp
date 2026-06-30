// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/sprite.h>
#include <ge/transform.h>

#include "sprite_internal.h"

#include <doctest.h>

#include <type_traits>
#include <utility>

// The full GPU submit path (drawRun) needs a live sokol context, so it is
// not exercised here. Its overflow-spreading decision (🎯T113), however, is
// factored into the pure detail::planSpriteRun, which IS tested below — the
// same function drawRun executes, so plan coverage == render coverage.
// The remaining tests verify the vertex-buffer geometry produced by
// `addSprite` via the friend accessor below.

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

// ── 🎯T113: stream-buffer overflow spreads across pooled buffers ──────────

using ge::detail::planSpriteRun;
using ge::detail::SpriteRunStep;

// sizeof(SpriteVertex) == 24 → a 256 KB stream buffer holds 10922 verts,
// 10920 after rounding down to whole quads (1820 quads). This mirrors
// sprite.cpp's kStreamBufferBytes.
static constexpr int kFullVerts = 256 * 1024 / 24;

// Replay a plan and assert the universal 🎯T113 invariants: nothing is
// dropped (chunks sum to the run), every chunk is whole quads and fits the
// buffer it lands in, and only buffer-spills are flagged `newBuffer`.
static void checkPlan(const std::vector<SpriteRunStep>& steps,
                      int totalVerts, int firstFree, int full) {
    int sum     = 0;
    int capLeft = firstFree - firstFree % 6;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& s = steps[i];
        CHECK(s.verts > 0);
        CHECK(s.verts % 6 == 0);                 // never split a triangle
        if (s.newBuffer) capLeft = full - full % 6;  // spilled to a fresh buffer
        CHECK(s.verts <= capLeft);               // fits the buffer it lands in
        capLeft -= s.verts;
        sum     += s.verts;
    }
    CHECK(sum == totalVerts);                    // no silent drop
}

TEST_CASE("planSpriteRun: small run fits the current buffer in one step") {
    auto steps = planSpriteRun(6, kFullVerts, kFullVerts);  // 1 quad
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].newBuffer == false);
    CHECK(steps[0].verts == 6);
    checkPlan(steps, 6, kFullVerts, kFullVerts);
}

TEST_CASE("planSpriteRun: a run larger than one 256 KB buffer is split, not dropped") {
    const int quads = 6000;            // > 5500; ~864 KB ≫ 256 KB
    const int verts = quads * 6;
    auto steps = planSpriteRun(verts, kFullVerts, kFullVerts);
    CHECK(steps.size() > 1);                      // needed multiple buffers
    CHECK(steps.front().newBuffer == false);      // first lands in current buffer
    for (std::size_t i = 1; i < steps.size(); ++i) CHECK(steps[i].newBuffer);
    checkPlan(steps, verts, kFullVerts, kFullVerts);  // sum == verts: all rendered
}

TEST_CASE("planSpriteRun: run continues in a partially-filled buffer") {
    const int firstFree = kFullVerts - 600;       // 100 quads already used
    auto steps = planSpriteRun(60, firstFree, kFullVerts);  // 10 quads, plenty of room
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].newBuffer == false);
    checkPlan(steps, 60, firstFree, kFullVerts);
}

TEST_CASE("planSpriteRun: spills the overflow to a fresh buffer") {
    const int firstFree = 12;                     // room for only 2 quads
    auto steps = planSpriteRun(60, firstFree, kFullVerts);  // 10 quads
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].newBuffer == false);
    CHECK(steps[0].verts == 12);                  // fills current buffer's 2 quads
    CHECK(steps[1].newBuffer == true);
    CHECK(steps[1].verts == 48);                  // remaining 8 quads, fresh buffer
    checkPlan(steps, 60, firstFree, kFullVerts);
}

TEST_CASE("planSpriteRun: a full current buffer puts the first step in a fresh buffer") {
    auto steps = planSpriteRun(60, 0, kFullVerts);  // current buffer full
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].newBuffer == true);
    checkPlan(steps, 60, 0, kFullVerts);
}

TEST_CASE("planSpriteRun: exact multi-buffer fit leaves no empty trailing step") {
    const int full = 60;                          // tiny buffers: 10 quads each
    auto steps = planSpriteRun(180, full, full);  // exactly 3 buffers
    REQUIRE(steps.size() == 3);
    CHECK(steps[0].newBuffer == false);
    CHECK(steps[1].newBuffer == true);
    CHECK(steps[2].newBuffer == true);
    for (const auto& s : steps) CHECK(s.verts == 60);
    checkPlan(steps, 180, full, full);
}

TEST_CASE("planSpriteRun: empty run produces no steps") {
    CHECK(planSpriteRun(0, kFullVerts, kFullVerts).empty());
}

// ── 🎯T135: ge::Sprite RAII ownership ──────────────────────────────
//
// These run without a live sokol context: Sprite::destroy() guards its
// sg_destroy_* calls on sg_isvalid() (false here), but still counts the
// release event via ge::detail::spriteReleaseCount(). So we verify the
// ownership *logic* (move transfers + nulls source, move-assign releases the
// prior slot, copy is deleted) headlessly, and prove the leak fix at scale via
// the release counter — a stand-in for live sokol pool occupancy.

TEST_CASE("🎯T135 Sprite is move-only (copy deleted, move enabled)") {
    static_assert(!std::is_copy_constructible_v<Sprite>, "Sprite must not be copyable");
    static_assert(!std::is_copy_assignable_v<Sprite>,    "Sprite must not be copy-assignable");
    static_assert(std::is_move_constructible_v<Sprite>,  "Sprite must be move-constructible");
    static_assert(std::is_move_assignable_v<Sprite>,     "Sprite must be move-assignable");
    CHECK(true);  // the contract is the static_asserts above
}

TEST_CASE("🎯T135 move-construct transfers handles and nulls the source") {
    const uint64_t before = ge::detail::spriteReleaseCount();
    Sprite a = fakeSprite(42, 7, 9);
    REQUIRE_FALSE(a.isNull());

    Sprite b = std::move(a);
    CHECK(b.tex.id == 42);
    CHECK(b.width  == 7);
    CHECK(b.height == 9);
    CHECK(a.isNull());  // moved-from source is null → its dtor releases nothing
    // Move-construction releases no resource (b had no prior slot).
    CHECK(ge::detail::spriteReleaseCount() == before);
}

TEST_CASE("🎯T135 move-assign releases the destination's prior resource") {
    const uint64_t before = ge::detail::spriteReleaseCount();
    Sprite slot = fakeSprite(1);
    Sprite next = fakeSprite(2);
    slot = std::move(next);            // overwrites a non-null slot
    CHECK(slot.tex.id == 2);
    CHECK(next.isNull());
    CHECK(ge::detail::spriteReleaseCount() - before == 1);  // the prior (id=1) was released
}

TEST_CASE("🎯T135 destroy() on a null sprite is a counted-release no-op") {
    const uint64_t before = ge::detail::spriteReleaseCount();
    Sprite s;
    s.destroy();
    s.destroy();
    CHECK(s.isNull());
    CHECK(ge::detail::spriteReleaseCount() == before);  // null release isn't counted
}

TEST_CASE("🎯T135 re-rasterizing one slot N≫pool times releases every prior (no orphaning)") {
    // The bug: each `slot = rasterizeSvg(...)` orphaned the prior sg_image+view;
    // after ~128 image / 256 view orphans the pool filled and sg_make_* aborted
    // (multimaze2 iPhone-13 SIGABRT). With RAII move-assignment, every reassign
    // releases the prior, so live occupancy stays at one regardless of N.
    constexpr int N = 2000;  // ≫ the image(128) / view(256) pools and the ≥1000 acceptance
    const uint64_t before = ge::detail::spriteReleaseCount();
    Sprite slot;  // starts null
    for (int i = 1; i <= N; ++i) {
        slot = fakeSprite(static_cast<uint16_t>(i % 60000 + 1));  // re-rasterize the slot
    }
    // First assign overwrote a null slot (no release); the other N-1 each
    // released the prior occupant — so no handle was ever orphaned.
    CHECK(ge::detail::spriteReleaseCount() - before == static_cast<uint64_t>(N - 1));
    slot.destroy();
    CHECK(ge::detail::spriteReleaseCount() - before == static_cast<uint64_t>(N));
}
