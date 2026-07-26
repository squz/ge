// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168 Cube-sphere convention oracle. These tests pin the face basis —
// if any of them fail after an edit, cooked packs and runtime sampling
// have diverged and every shipped .getp is invalidated. Do not "fix" a
// failure by changing the expected values without recooking assets.

#include <ge/CubeSphere.h>
#include <ge/TilePackFormat.h>

#include <doctest.h>

#include <cmath>
#include <random>

using namespace ge;

namespace {
bool approx(const la::float3& a, const la::float3& b, float eps = 1e-5f) {
    return la::length(a - b) < eps;
}
} // namespace

TEST_CASE("cubeFaceDir hits face centers on the axes") {
    CHECK(approx(cubeFaceDir(CubeFace::PosX, 0.5f, 0.5f), { 1, 0, 0}));
    CHECK(approx(cubeFaceDir(CubeFace::NegX, 0.5f, 0.5f), {-1, 0, 0}));
    CHECK(approx(cubeFaceDir(CubeFace::PosY, 0.5f, 0.5f), { 0, 1, 0}));
    CHECK(approx(cubeFaceDir(CubeFace::NegY, 0.5f, 0.5f), { 0,-1, 0}));
    CHECK(approx(cubeFaceDir(CubeFace::PosZ, 0.5f, 0.5f), { 0, 0, 1}));
    CHECK(approx(cubeFaceDir(CubeFace::NegZ, 0.5f, 0.5f), { 0, 0,-1}));
}

TEST_CASE("cubeFaceDir matches the GL cubemap basis at face corners") {
    // +X face, u=v=0 → sc=tc=-1 → dir ∝ (1, 1, 1).
    CHECK(approx(cubeFaceDir(CubeFace::PosX, 0.0f, 0.0f),
                 la::normalize(la::float3{1, 1, 1})));
    // +Z face, u=1,v=0 → sc=1,tc=-1 → dir ∝ (1, 1, 1).
    CHECK(approx(cubeFaceDir(CubeFace::PosZ, 1.0f, 0.0f),
                 la::normalize(la::float3{1, 1, 1})));
}

TEST_CASE("face UV round-trips through direction") {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> uv(0.001f, 0.999f);
    for (int f = 0; f < kCubeFaceCount; ++f) {
        for (int i = 0; i < 200; ++i) {
            const auto face = CubeFace(f);
            const float u = uv(rng), v = uv(rng);
            const auto d  = cubeFaceDir(face, u, v);
            const auto r  = cubeFaceUVForDir(d);
            CHECK(r.face == face);
            CHECK(r.u == doctest::Approx(u).epsilon(1e-4));
            CHECK(r.v == doctest::Approx(v).epsilon(1e-4));
        }
    }
}

TEST_CASE("direction round-trips through face UV") {
    std::mt19937 rng(7);
    std::normal_distribution<float> g;
    for (int i = 0; i < 1000; ++i) {
        la::float3 d{g(rng), g(rng), g(rng)};
        if (la::length(d) < 1e-3f) continue;
        d = la::normalize(d);
        const auto fu = cubeFaceUVForDir(d);
        CHECK(approx(cubeFaceDir(fu.face, fu.u, fu.v), d, 1e-4f));
    }
}

TEST_CASE("lon/lat round-trips and pins the globe orientation") {
    // Z up, lon 0 on +X, east positive.
    CHECK(approx(dirForLonLat(0, 0), {1, 0, 0}));
    CHECK(approx(dirForLonLat(float(M_PI) / 2, 0), {0, 1, 0}));
    CHECK(approx(dirForLonLat(0, float(M_PI) / 2), {0, 0, 1}));
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> lon(-3.1f, 3.1f);
    std::uniform_real_distribution<float> lat(-1.5f, 1.5f);
    for (int i = 0; i < 500; ++i) {
        const float lo = lon(rng), la_ = lat(rng);
        const auto ll = lonLatForDir(dirForLonLat(lo, la_));
        CHECK(ll.x == doctest::Approx(lo).epsilon(1e-4));
        CHECK(ll.y == doctest::Approx(la_).epsilon(1e-4));
    }
}

TEST_CASE("tile math: rects nest and tileForFaceUV inverts tileRect") {
    for (uint8_t level : {0, 1, 3}) {
        const uint32_t side = 1u << level;
        for (uint16_t ty = 0; ty < side; ++ty) {
            for (uint16_t tx = 0; tx < side; ++tx) {
                const auto r = tileRect(level, tx, ty);
                const auto c = tileForFaceUV(CubeFace::PosX, level,
                                             r.u0 + r.extent * 0.5f,
                                             r.v0 + r.extent * 0.5f);
                CHECK(c.tx == tx);
                CHECK(c.ty == ty);
            }
        }
    }
    // Edge clamping: u=v=1 lands in the last tile, not off the end.
    const auto c = tileForFaceUV(CubeFace::PosX, 2, 1.0f, 1.0f);
    CHECK(c.tx == 3);
    CHECK(c.ty == 3);
}

TEST_CASE("patch mesh geometry: on-sphere, CCW from outside, chord bound") {
    const auto p = tilePatchMesh(CubeFace::NegY, 2, 1, 2, 8);
    REQUIRE(p.positions.size() == 81);
    REQUIRE(p.indices.size() == 8 * 8 * 6);
    for (const auto& pos : p.positions)
        CHECK(la::length(pos) == doctest::Approx(1.0f).epsilon(1e-5));
    // Every triangle's outward normal agrees with its centroid direction.
    for (size_t t = 0; t < p.indices.size(); t += 3) {
        const auto& a = p.positions[p.indices[t]];
        const auto& b = p.positions[p.indices[t + 1]];
        const auto& c = p.positions[p.indices[t + 2]];
        const auto n  = la::cross(b - a, c - a);
        CHECK(la::dot(n, a + b + c) > 0.0f);
    }
    // Max deviation of edge midpoints from the sphere obeys the bound.
    const float bound = chordErrorForGrid(2, 8);
    for (size_t t = 0; t < p.indices.size(); t += 3) {
        const auto& a = p.positions[p.indices[t]];
        const auto& b = p.positions[p.indices[t + 1]];
        const float sag = 1.0f - la::length((a + b) * 0.5f);
        CHECK(sag <= bound);
    }
}

TEST_CASE("payload→tex UV maps payload corners inside the gutter") {
    const auto lo = tilePayloadToTexUV({0, 0}, 1280, 4);
    const auto hi = tilePayloadToTexUV({1, 1}, 1280, 4);
    CHECK(lo.x == doctest::Approx(4.0f / 1288.0f));
    CHECK(hi.x == doctest::Approx(1284.0f / 1288.0f));
}

TEST_CASE("tile-pack index math") {
    CHECK(geTilePlaneTileCount(1) == 6);
    CHECK(geTilePlaneTileCount(4) == 6 * (1 + 4 + 16 + 64));
    CHECK(geTileEntryIndex(0, 0, 0, 0) == 0);
    CHECK(geTileEntryIndex(1, 0, 0, 0) == 6);
    // level 1, face 2, ty=1, tx=0 → 6 + 2*4 + 1*2 + 0
    CHECK(geTileEntryIndex(1, 2, 0, 1) == 6 + 8 + 2);
    // Entries are dense and non-overlapping across a whole plane.
    uint32_t expect = 0;
    for (uint16_t l = 0; l < 3; ++l) {
        const uint16_t side = uint16_t(1u << l);
        for (uint8_t f = 0; f < 6; ++f)
            for (uint16_t ty = 0; ty < side; ++ty)
                for (uint16_t tx = 0; tx < side; ++tx)
                    CHECK(geTileEntryIndex(l, f, tx, ty) == expect++);
    }
    CHECK(expect == geTilePlaneTileCount(3));
}
