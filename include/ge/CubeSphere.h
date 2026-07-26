// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168 Cube-sphere conventions shared by the tile-pack cook (ge-texpack),
// the runtime loader (TilePack.h), and consumer patch rendering.
//
// A globe is mapped onto six gnomonic (central-projection) cube faces. Face
// order and orientation MATCH sokol cubemap slice order (SG_CUBEFACE_*), so
// a direction that samples texel (u,v) of slice F in a cubemap samples the
// same ground point in tile space on face F. Everything here is pure math —
// no I/O, no sokol — safe in tools and tests.
//
// Definitions:
//  - Face UV: (u,v) ∈ [0,1]² across a full face, u right / v down in texture
//    space, exactly as sokol addresses cubemap slices.
//  - Level L: the face is a 2^L × 2^L grid of tiles (L=0 → one tile).
//  - Tile-local UV: (u,v) ∈ [0,1]² across one tile's payload texels
//    (gutter excluded — see TilePackFormat.h).

#pragma once

#include <ge/Linalg.h>

#include <cstdint>
#include <vector>

namespace ge {

// Matches SG_CUBEFACE_POS_X … SG_CUBEFACE_NEG_Z (values 0..5).
enum class CubeFace : uint8_t {
    PosX = 0, NegX = 1, PosY = 2, NegY = 3, PosZ = 4, NegZ = 5,
};
inline constexpr int kCubeFaceCount = 6;

// Unit direction for a face UV. (u,v) may extend slightly outside [0,1] —
// the gnomonic projection extends continuously across face edges, which is
// how the cook fills tile gutters.
la::float3 cubeFaceDir(CubeFace face, float u, float v);

// Inverse: face + UV for a direction (dir need not be normalized).
struct CubeFaceUV {
    CubeFace face;
    float    u, v;
};
CubeFaceUV cubeFaceUVForDir(const la::float3& dir);

// Tile coordinates at a level. tx grows with u, ty with v.
struct TileCoord {
    CubeFace face;
    uint8_t  level;
    uint16_t tx, ty;
};
TileCoord tileForFaceUV(CubeFace face, uint8_t level, float u, float v);

// Face-UV rectangle covered by a tile's payload: [u0,u0+extent]×[v0,v0+extent]
// with extent = 1 / 2^level.
struct TileRect {
    float u0, v0, extent;
};
TileRect tileRect(uint8_t level, uint16_t tx, uint16_t ty);

// Sphere-patch mesh for one tile: (gridN+1)² unit-sphere positions (row-major,
// v-major like texel order) and tile-local UVs in [0,1]. gridN subdivisions
// per edge bound the chord error; chordErrorForGrid reports the worst-case
// sphere-to-triangle deviation (radians of arc) so callers can pick gridN
// from a screen-space budget instead of guessing.
struct TilePatch {
    int                      gridN = 0;
    // positions.size() == uvs.size() == (gridN+1)*(gridN+1)
    std::vector<la::float3>  positions;
    std::vector<la::float2>  uvs;
    std::vector<uint16_t>    indices; // gridN*gridN*6, CCW viewed from outside
};
TilePatch tilePatchMesh(CubeFace face, uint8_t level, uint16_t tx, uint16_t ty,
                        int gridN);
float chordErrorForGrid(uint8_t level, int gridN);

// Map a tile-local payload UV into the stored (guttered) texture UV:
//   tex = (gutter + uv * tileSize) / (tileSize + 2*gutter)
// Kept here (not the loader) so shaders/tools derive it from one place.
la::float2 tilePayloadToTexUV(const la::float2& uv, int tileSize, int gutter);

// Equirectangular helpers used by the cook: longitude λ ∈ [-π,π] (x east),
// latitude φ ∈ [-π/2,π/2] (up). Direction convention: Z up, lon 0 on +X.
la::float3   dirForLonLat(float lon, float lat);
la::float2   lonLatForDir(const la::float3& dir);

} // namespace ge
