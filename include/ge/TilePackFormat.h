// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.1 On-disk .getp tile-pack format: a cube-sphere pyramid of
// compressed tiles, multiple planes (imagery / ID / SDF) per pack.
//
// This is a NEW container beside .getex (whose 16-byte header is frozen —
// STABILITY.md "data-breaking"). One pack file holds, per plane, a quadtree
// pyramid over six cube faces (CubeSphere.h conventions).
//
// File layout (little-endian, offsets from file start):
//   GeTilePackHeader
//   GeTilePlaneDesc[planeCount]
//   per plane: GeTileEntry[tileCount(plane)]   (index; offset given in desc)
//   tile payload blobs, ordered coarse level → fine level, planes interleaved
//     per level, so cooking with a smaller levelCap yields a byte-prefix-
//     compatible "coarser" pack and a device can stop reading early.
//
// tileCount(plane) = Σ_{L=0}^{levelCount-1} 6 · 4^L, indexed [level][face]
// [ty][tx] with level outermost, tx innermost.
//
// A tile payload blob is: uint32_t mipSizes[mipCount] then the mip payloads
// contiguous (mip 0 first). Mip 0 dimensions are (tileSize + 2·gutter)².
// Per-tile mips only go as deep as stored (mipCount); coarser minification
// comes from switching to a coarser pyramid level.

#pragma once

#include <cstdint>

namespace ge {

constexpr uint8_t kGeTilePackMagic[4] = {0x47, 0x45, 0x54, 0x50}; // "GETP"
constexpr uint16_t kGeTilePackVersion = 1;

// Per-plane texel encodings. Values are frozen once released.
enum class GeTilePlaneEncoding : uint16_t {
    Astc4x4 = 0, // → SG_PIXELFORMAT_ASTC_4x4_RGBA (imagery)
    R8      = 1, // raw R8 unorm, nearest-sampled
    EacR11  = 2, // → SG_PIXELFORMAT_EAC_R11 (reserved, not cooked in v1)
    Rg8     = 3, // raw RG8 unorm carrying a 16-bit ID: id = R + G*256
                 // (little-endian u16 source), nearest-sampled, single mip.
                 // Country-ID plane — 254 regions overflowed u8 (🎯T69.2).
};

struct GeTilePackHeader {
    uint8_t  magic[4];    // kGeTilePackMagic
    uint16_t version;     // kGeTilePackVersion
    uint16_t planeCount;
    uint64_t fileSize;    // total pack size; a byte-prefix truncated at
                          // levelTruncOffset[k] is a valid shallower pack
    uint32_t _reserved[2];
};
static_assert(sizeof(GeTilePackHeader) == 24);

struct GeTilePlaneDesc {
    char     name[8];       // NUL-padded: "rgb", "id", "sdf"
    uint16_t encoding;      // GeTilePlaneEncoding
    uint16_t tileSize;      // payload texels per tile edge (e.g. 1280)
    uint16_t gutter;        // duplicated texels per edge (e.g. 4); stored
                            // tile edge = tileSize + 2*gutter
    uint16_t mipCount;      // per-tile mips stored (>= 1)
    uint16_t levelCount;    // pyramid levels; face edge at level L =
                            // tileSize * 2^L
    uint16_t filterNearest; // 1 → sample nearest (ID plane), 0 → linear
    uint32_t _reserved;
    uint64_t indexOffset;   // file offset of this plane's GeTileEntry array
};
static_assert(sizeof(GeTilePlaneDesc) == 32);

struct GeTileEntry {
    uint64_t offset; // payload blob file offset (0 = tile absent)
    uint32_t size;   // blob size in bytes
    uint32_t _pad;
};
static_assert(sizeof(GeTileEntry) == 16);

// Number of tiles for one plane with `levelCount` levels.
constexpr uint32_t geTilePlaneTileCount(uint16_t levelCount) {
    uint32_t n = 0;
    for (uint16_t l = 0; l < levelCount; ++l) n += 6u * (1u << (2 * l));
    return n;
}

// Flat index of a tile within a plane's GeTileEntry array.
constexpr uint32_t geTileEntryIndex(uint16_t level, uint8_t face,
                                    uint16_t tx, uint16_t ty) {
    uint32_t base = 0;
    for (uint16_t l = 0; l < level; ++l) base += 6u * (1u << (2 * l));
    const uint32_t side = 1u << level;
    return base + (uint32_t(face) * side + ty) * side + tx;
}

} // namespace ge
