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
