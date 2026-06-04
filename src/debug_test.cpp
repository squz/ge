// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// CPU-side tests for the ge::debug overlay (🎯T97). These exercise the
// enable flag and primitive accumulation only — flush() needs a live sokol
// context, so it's covered by the tiltbuggy demo + device smoke, not here.

#include <doctest.h>
#include <ge/debug.h>

#include <cstdint>

using ge::la::float2;
using ge::la::float3;

namespace {

// Put the layer in a known state: enabled/disabled with empty queues. The
// debug state is a process-global singleton, so each case must reset it.
void reset(bool on) {
    ge::debug::setEnabled(on);
    ge::debug::clear();
}

} // namespace

TEST_CASE("ge::debug accumulation is a no-op while disabled") {
    reset(false);
    ge::debug::line(float2{0, 0}, float2{1, 1});
    ge::debug::tri(float2{0, 0}, float2{1, 0}, float2{0, 1});
    ge::debug::text({4, 4}, "hidden");
    CHECK(ge::debug::testing::lineVertexCount() == 0);
    CHECK(ge::debug::testing::triVertexCount() == 0);
    CHECK(ge::debug::testing::textItemCount() == 0);
}

TEST_CASE("ge::debug line + tri accumulate vertices when enabled") {
    reset(true);
    ge::debug::line(float2{0, 0}, float2{1, 1});
    CHECK(ge::debug::testing::lineVertexCount() == 2);
    ge::debug::tri(float3{0, 0, 0}, float3{1, 0, 0}, float3{0, 1, 0});
    CHECK(ge::debug::testing::triVertexCount() == 3);
    ge::debug::text({4, 4}, "fps: 60");
    CHECK(ge::debug::testing::textItemCount() == 1);
    ge::debug::clear();
    CHECK(ge::debug::testing::lineVertexCount() == 0);
    CHECK(ge::debug::testing::triVertexCount() == 0);
    CHECK(ge::debug::testing::textItemCount() == 0);
}

TEST_CASE("ge::debug::mesh wireframe (default) derives 3 edges (6 verts) per triangle") {
    reset(true);
    const float2   verts[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const uint16_t idx[]   = {0, 1, 2, 1, 3, 2};  // two triangles
    ge::debug::mesh(verts, idx);                  // default: magenta wire, fill absent
    CHECK(ge::debug::testing::lineVertexCount() == 12);  // 2 tris * 3 edges * 2
    CHECK(ge::debug::testing::triVertexCount()  == 0);   // no fill by default
}

TEST_CASE("ge::debug::mesh fill-only (wire absent) emits 3 verts per triangle") {
    reset(true);
    const float3   verts[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const uint16_t idx[]   = {0, 1, 2};
    // wireColor alpha 0 → wire layer skipped; fillColor present → fill only.
    ge::debug::mesh(verts, idx, /*wireColor=*/{}, /*fillColor=*/{1.0f, 0.0f, 1.0f, 0.25f});
    CHECK(ge::debug::testing::triVertexCount()  == 3);
    CHECK(ge::debug::testing::lineVertexCount() == 0);
}

TEST_CASE("ge::debug::mesh draws both fill and wireframe when both colours present") {
    reset(true);
    const float2   verts[] = {{0, 0}, {1, 0}, {0, 1}};
    const uint16_t idx[]   = {0, 1, 2};
    ge::debug::mesh(verts, idx, ge::debug::kWireColor, ge::debug::kFillColor);
    CHECK(ge::debug::testing::triVertexCount()  == 3);  // one filled triangle
    CHECK(ge::debug::testing::lineVertexCount() == 6);  // three edges
}

TEST_CASE("ge::debug::box outlines four edges, fills two triangles") {
    reset(true);
    ge::debug::box(ge::Rect{0, 0, 2, 1});  // wire only (default)
    CHECK(ge::debug::testing::lineVertexCount() == 8);  // 4 edges * 2
    CHECK(ge::debug::testing::triVertexCount()  == 0);

    reset(true);
    ge::debug::box(ge::Rect{0, 0, 2, 1}, /*wireColor=*/{}, ge::debug::kFillColor);
    CHECK(ge::debug::testing::triVertexCount()  == 6);  // 2 tris * 3
    CHECK(ge::debug::testing::lineVertexCount() == 0);
}

TEST_CASE("ge::debug::circle emits one segment per side, one fan tri per side") {
    reset(true);
    ge::debug::circle({0.0f, 0.0f}, 1.0f, ge::debug::kWireColor, {}, /*segments=*/8);
    CHECK(ge::debug::testing::lineVertexCount() == 16);  // 8 segments * 2
    CHECK(ge::debug::testing::triVertexCount()  == 0);

    reset(true);
    ge::debug::circle({0.0f, 0.0f}, 1.0f, /*wireColor=*/{}, ge::debug::kFillColor, 8);
    CHECK(ge::debug::testing::triVertexCount()  == 24);  // 8 fan tris * 3
    CHECK(ge::debug::testing::lineVertexCount() == 0);
}

TEST_CASE("ge::debug::point queues a marker; the quad is built at flush") {
    reset(true);
    ge::debug::point(float2{1.0f, 2.0f});
    CHECK(ge::debug::testing::pointItemCount() == 1);
    // Nothing in the tri/line streams yet — a point is a fixed on-screen size,
    // so its quad can't be built until flush() knows worldToClip + the surface.
    CHECK(ge::debug::testing::triVertexCount()  == 0);
    CHECK(ge::debug::testing::lineVertexCount() == 0);

    ge::debug::point(float3{0.0f, 0.0f, 5.0f});  // 3D overload also queues one
    CHECK(ge::debug::testing::pointItemCount() == 2);

    // alpha 0 is absent, like the other shapes' skipped layers.
    ge::debug::point(float2{9.0f, 9.0f}, /*color=*/{1, 0, 1, 0});
    CHECK(ge::debug::testing::pointItemCount() == 2);
}

TEST_CASE("ge::debug::point is a no-op while disabled") {
    reset(false);
    ge::debug::point(float2{1.0f, 2.0f});
    CHECK(ge::debug::testing::pointItemCount() == 0);
}

TEST_CASE("ge::debug::mesh ignores a degenerate tail and out-of-range indices") {
    reset(true);
    const float2 verts[] = {{0, 0}, {1, 0}, {0, 1}};

    const uint16_t withTail[] = {0, 1, 2, 9};  // one triangle + a 1-index tail
    ge::debug::mesh(verts, withTail);
    CHECK(ge::debug::testing::lineVertexCount() == 6);  // only the valid triangle

    reset(true);
    const uint16_t oob[] = {0, 1, 99};  // index past verts.size() → whole tri skipped
    ge::debug::mesh(verts, oob);
    CHECK(ge::debug::testing::lineVertexCount() == 0);
}

TEST_CASE("ge::debug::setEnabled toggles accumulation at runtime") {
    reset(true);
    ge::debug::line(float2{0, 0}, float2{1, 1});
    CHECK(ge::debug::testing::lineVertexCount() == 2);
    ge::debug::setEnabled(false);
    ge::debug::line(float2{0, 0}, float2{1, 1});
    CHECK(ge::debug::testing::lineVertexCount() == 2);  // unchanged while off
}
