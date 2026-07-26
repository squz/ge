// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/CubeSphere.h>

#include <algorithm>
#include <cmath>

namespace ge {

using la::float2;
using la::float3;

// Standard GL/Metal cubemap face basis (sc, tc, major-axis) table, inverted.
// sc = 2u-1, tc = 2v-1. See faceUVForDir for the forward table; the pair is
// round-trip tested in CubeSphere_test.cpp.
float3 cubeFaceDir(CubeFace face, float u, float v) {
    const float sc = 2.0f * u - 1.0f;
    const float tc = 2.0f * v - 1.0f;
    float3 d;
    switch (face) {
        case CubeFace::PosX: d = { 1.0f, -tc,   -sc  }; break;
        case CubeFace::NegX: d = {-1.0f, -tc,    sc  }; break;
        case CubeFace::PosY: d = { sc,    1.0f,  tc  }; break;
        case CubeFace::NegY: d = { sc,   -1.0f, -tc  }; break;
        case CubeFace::PosZ: d = { sc,   -tc,    1.0f}; break;
        case CubeFace::NegZ: d = {-sc,   -tc,   -1.0f}; break;
    }
    return la::normalize(d);
}

CubeFaceUV cubeFaceUVForDir(const float3& dir) {
    const float ax = std::fabs(dir.x), ay = std::fabs(dir.y),
                az = std::fabs(dir.z);
    CubeFace face;
    float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (dir.x > 0) { face = CubeFace::PosX; sc = -dir.z; tc = -dir.y; }
        else           { face = CubeFace::NegX; sc =  dir.z; tc = -dir.y; }
    } else if (ay >= az) {
        ma = ay;
        if (dir.y > 0) { face = CubeFace::PosY; sc =  dir.x; tc =  dir.z; }
        else           { face = CubeFace::NegY; sc =  dir.x; tc = -dir.z; }
    } else {
        ma = az;
        if (dir.z > 0) { face = CubeFace::PosZ; sc =  dir.x; tc = -dir.y; }
        else           { face = CubeFace::NegZ; sc = -dir.x; tc = -dir.y; }
    }
    return {face, 0.5f * (sc / ma + 1.0f), 0.5f * (tc / ma + 1.0f)};
}

TileCoord tileForFaceUV(CubeFace face, uint8_t level, float u, float v) {
    const uint32_t side = 1u << level;
    const auto clampTile = [side](float x) -> uint16_t {
        const float t = std::floor(x * float(side));
        return uint16_t(std::clamp(t, 0.0f, float(side - 1)));
    };
    return {face, level, clampTile(u), clampTile(v)};
}

TileRect tileRect(uint8_t level, uint16_t tx, uint16_t ty) {
    const float extent = 1.0f / float(1u << level);
    return {float(tx) * extent, float(ty) * extent, extent};
}

TilePatch tilePatchMesh(CubeFace face, uint8_t level, uint16_t tx, uint16_t ty,
                        int gridN) {
    TilePatch p;
    p.gridN = gridN;
    const TileRect rect = tileRect(level, tx, ty);
    const int verts = (gridN + 1) * (gridN + 1);
    p.positions.reserve(verts);
    p.uvs.reserve(verts);
    for (int j = 0; j <= gridN; ++j) {
        for (int i = 0; i <= gridN; ++i) {
            const float lu = float(i) / float(gridN);
            const float lv = float(j) / float(gridN);
            p.positions.push_back(cubeFaceDir(face,
                                              rect.u0 + lu * rect.extent,
                                              rect.v0 + lv * rect.extent));
            p.uvs.push_back({lu, lv});
        }
    }
    // du×dv points inward for every face of this basis, so (00,01,10) is CCW
    // viewed from outside the sphere.
    p.indices.reserve(size_t(gridN) * gridN * 6);
    const auto at = [gridN](int i, int j) {
        return uint16_t(j * (gridN + 1) + i);
    };
    for (int j = 0; j < gridN; ++j) {
        for (int i = 0; i < gridN; ++i) {
            p.indices.push_back(at(i, j));
            p.indices.push_back(at(i, j + 1));
            p.indices.push_back(at(i + 1, j));
            p.indices.push_back(at(i + 1, j));
            p.indices.push_back(at(i, j + 1));
            p.indices.push_back(at(i + 1, j + 1));
        }
    }
    return p;
}

float chordErrorForGrid(uint8_t level, int gridN) {
    // Worst-case arc per grid cell: the face spans 90° of arc through the
    // center; the gnomonic projection stretches up to 2× per axis at face
    // edges, and the cell diagonal adds √2. Chord sagitta for arc θ is
    // 1−cos(θ/2) ≈ θ²/8 on a unit sphere. Conservative, not tight.
    const float cells = float(uint32_t(1) << level) * float(gridN);
    const float theta = (float(M_PI) / 2.0f) / cells * 2.0f * float(M_SQRT2);
    return 1.0f - std::cos(0.5f * theta);
}

float2 tilePayloadToTexUV(const float2& uv, int tileSize, int gutter) {
    const float stored = float(tileSize + 2 * gutter);
    return {(float(gutter) + uv.x * float(tileSize)) / stored,
            (float(gutter) + uv.y * float(tileSize)) / stored};
}

float3 dirForLonLat(float lon, float lat) {
    const float cl = std::cos(lat);
    return {cl * std::cos(lon), cl * std::sin(lon), std::sin(lat)};
}

float2 lonLatForDir(const float3& dir) {
    const float3 n = la::normalize(dir);
    return {std::atan2(n.y, n.x), std::asin(std::clamp(n.z, -1.0f, 1.0f))};
}

} // namespace ge
