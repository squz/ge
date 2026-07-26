// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.2 Non-GPU surface of the tile-pyramid loader: the pure ancestor-
// remap math, and open()'s defensive parsing of malformed/valid packs. GPU
// upload (pump()) needs a live sokol context and is guarded by sg_isvalid(),
// so open()/destroy() here run with no sokol context at all — this pins that
// a pyramid can be opened and torn down safely in that mode.

#include <ge/TilePack.h>

#include <ge/CubeSphere.h>
#include <ge/TilePackFormat.h>

#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ge;

namespace {

// ── tileAncestorRemap ────────────────────────────────────────────────────

// A child tile's remapped rect, in ancestor payload-UV space, must equal the
// child's own tileRect within the ancestor's face (i.e. nest exactly).
void checkNests(uint8_t level, uint16_t tx, uint16_t ty, uint8_t ancestorLevel) {
    const TileAncestorRemap r = tileAncestorRemap(level, tx, ty, ancestorLevel);
    const TileRect childInAncestorFace = tileRect(level, tx, ty);
    // childInAncestorFace is expressed in whole-face UV; the ancestor's own
    // payload-UV [0,1] covers tileRect(ancestorLevel, txa, tya) of that face.
    const uint8_t d = uint8_t(level - ancestorLevel);
    const uint16_t txa = uint16_t(tx >> d);
    const uint16_t tya = uint16_t(ty >> d);
    const TileRect ancestorFace = tileRect(ancestorLevel, txa, tya);

    // Map corner (0,0) and (1,1) of the child's payload UV through the remap,
    // then through the ancestor's own tileRect, and compare against the
    // child's whole-face rect directly.
    for (float u : {0.0f, 1.0f}) {
        for (float v : {0.0f, 1.0f}) {
            const float au = u * r.uvScale.x + r.uvBias.x;
            const float av = v * r.uvScale.y + r.uvBias.y;
            const float faceU = ancestorFace.u0 + au * ancestorFace.extent;
            const float faceV = ancestorFace.v0 + av * ancestorFace.extent;
            const float expectU = childInAncestorFace.u0 + u * childInAncestorFace.extent;
            const float expectV = childInAncestorFace.v0 + v * childInAncestorFace.extent;
            CHECK(faceU == doctest::Approx(expectU).epsilon(1e-5));
            CHECK(faceV == doctest::Approx(expectV).epsilon(1e-5));
        }
    }
    // uvScale/uvBias must keep the whole remapped rect within [0,1]^2.
    CHECK(r.uvBias.x >= 0.0f);
    CHECK(r.uvBias.y >= 0.0f);
    CHECK(r.uvBias.x + r.uvScale.x <= 1.0f + 1e-5f);
    CHECK(r.uvBias.y + r.uvScale.y <= 1.0f + 1e-5f);
}

} // namespace

TEST_CASE("tileAncestorRemap: identity when ancestor == requested level") {
    for (uint8_t level : {0, 1, 2, 3}) {
        const uint16_t side = uint16_t(1u << level);
        for (uint16_t ty = 0; ty < side; ++ty)
            for (uint16_t tx = 0; tx < side; ++tx)
                checkNests(level, tx, ty, level);
    }
}

TEST_CASE("tileAncestorRemap: child rect nests inside every ancestor level") {
    for (uint8_t level : {1, 2, 3}) {
        const uint16_t side = uint16_t(1u << level);
        for (uint16_t ty = 0; ty < side; ++ty) {
            for (uint16_t tx = 0; tx < side; ++tx) {
                for (uint8_t ancestor = 0; ancestor <= level; ++ancestor) {
                    checkNests(level, tx, ty, ancestor);
                }
            }
        }
    }
}

// ── open(): defensive parsing ────────────────────────────────────────────

namespace {

std::filesystem::path tmpPath(const char* name) {
    return std::filesystem::temp_directory_path() /
           (std::string("ge_tilepack_test_") + name);
}

void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    if (!bytes.empty())
        f.write(reinterpret_cast<const char*>(bytes.data()),
               std::streamsize(bytes.size()));
    REQUIRE(f.good());
}

template <typename T>
void append(std::vector<uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

GeTilePlaneDesc makePlaneDesc(const char* name, GeTilePlaneEncoding enc,
                             uint16_t tileSize, uint16_t gutter, uint16_t mipCount,
                             uint16_t levelCount, uint64_t indexOffset) {
    GeTilePlaneDesc d{};
    std::memset(d.name, 0, sizeof(d.name));
    std::strncpy(d.name, name, sizeof(d.name) - 1);
    d.encoding = uint16_t(enc);
    d.tileSize = tileSize;
    d.gutter = gutter;
    d.mipCount = mipCount;
    d.levelCount = levelCount;
    d.filterNearest = 1;
    d.indexOffset = indexOffset;
    return d;
}

} // namespace

TEST_CASE("open(): truncated header is rejected without crashing") {
    const auto p = tmpPath("truncated_header");
    writeFile(p, std::vector<uint8_t>(sizeof(GeTilePackHeader) - 4, 0));
    TilePyramid pyr = TilePyramid::open(p.string());
    CHECK(pyr.isNull());
}

TEST_CASE("open(): bad magic is rejected without crashing") {
    const auto p = tmpPath("bad_magic");
    GeTilePackHeader hdr{};
    hdr.magic[0] = 'X'; hdr.magic[1] = 'X'; hdr.magic[2] = 'X'; hdr.magic[3] = 'X';
    hdr.version = kGeTilePackVersion;
    hdr.planeCount = 1;
    hdr.fileSize = sizeof(hdr);
    std::vector<uint8_t> bytes;
    append(bytes, hdr);
    writeFile(p, bytes);
    TilePyramid pyr = TilePyramid::open(p.string());
    CHECK(pyr.isNull());
}

TEST_CASE("open(): oversized planeCount is rejected without crashing") {
    const auto p = tmpPath("oversized_planecount");
    GeTilePackHeader hdr{};
    std::memcpy(hdr.magic, kGeTilePackMagic, 4);
    hdr.version = kGeTilePackVersion;
    hdr.planeCount = 200; // way beyond the planeCount<=8 sanity cap
    hdr.fileSize = sizeof(hdr);
    std::vector<uint8_t> bytes;
    append(bytes, hdr);
    writeFile(p, bytes);
    TilePyramid pyr = TilePyramid::open(p.string());
    CHECK(pyr.isNull());
}

TEST_CASE("open(): entries beyond fileSize are treated as absent, not fatal") {
    // 🎯T168.1 truncation property: a capped/CDN-truncated pack keeps its FULL
    // index, so out-of-range entries are expected and must degrade to
    // "absent" — never reject the pack.
    const auto p = tmpPath("entry_oob");

    const uint64_t descsOff = sizeof(GeTilePackHeader);
    const uint64_t indexOffset = descsOff + sizeof(GeTilePlaneDesc);
    const GeTilePlaneDesc desc = makePlaneDesc(
        "id", GeTilePlaneEncoding::R8, 4, 0, 1, /*levelCount=*/1, indexOffset);
    const uint32_t tileCount = geTilePlaneTileCount(desc.levelCount); // 6
    const uint64_t totalSize = indexOffset + uint64_t(tileCount) * sizeof(GeTileEntry);

    GeTilePackHeader hdr{};
    std::memcpy(hdr.magic, kGeTilePackMagic, 4);
    hdr.version = kGeTilePackVersion;
    hdr.planeCount = 1;
    hdr.fileSize = totalSize;

    std::vector<uint8_t> bytes;
    append(bytes, hdr);
    append(bytes, desc);
    for (uint32_t i = 0; i < tileCount; ++i) {
        GeTileEntry e{};
        e.offset = totalSize + 1'000'000; // beyond the (tiny) file
        e.size = 16;
        append(bytes, e);
    }
    REQUIRE(bytes.size() == totalSize);

    writeFile(p, bytes);
    TilePyramid pyr = TilePyramid::open(p.string());
    REQUIRE_FALSE(pyr.isNull());
    CHECK(pyr.bytesTotal() == 0); // every tile absent — nothing to load
    CHECK(pyr.fullyResident());   // vacuously: 0 of 0 tiles
}

TEST_CASE("open(): capped pack loads present levels, absents the truncated leaf") {
    // Two-level plane whose level-1 blobs lie beyond fileSize (physically
    // truncated pack). Level 0 must be loadable; level 1 absent.
    const auto p = tmpPath("capped_pack");

    constexpr uint16_t kTileSize = 4;
    constexpr uint16_t kLevelCount = 2; // 6 + 24 = 30 tiles in the index
    const uint32_t tileCount = geTilePlaneTileCount(kLevelCount);
    REQUIRE(tileCount == 30);

    const uint64_t descsOff = sizeof(GeTilePackHeader);
    const uint64_t indexOffset = descsOff + sizeof(GeTilePlaneDesc);
    const GeTilePlaneDesc desc = makePlaneDesc(
        "id", GeTilePlaneEncoding::R8, kTileSize, 0, 1, kLevelCount, indexOffset);

    const uint32_t payloadBytes = kTileSize * kTileSize;
    const uint64_t blobsOff = indexOffset + uint64_t(tileCount) * sizeof(GeTileEntry);
    const uint64_t blobStride = sizeof(uint32_t) + payloadBytes;

    // Full layout: 30 blobs; file physically ends after the first 6 (level 0).
    std::vector<GeTileEntry> entries(tileCount);
    for (uint32_t i = 0; i < tileCount; ++i) {
        entries[i].offset = blobsOff + i * blobStride;
        entries[i].size = uint32_t(blobStride);
    }
    const uint64_t cappedSize = blobsOff + 6 * blobStride;

    GeTilePackHeader hdr{};
    std::memcpy(hdr.magic, kGeTilePackMagic, 4);
    hdr.version = kGeTilePackVersion;
    hdr.planeCount = 1;
    hdr.fileSize = cappedSize;

    std::vector<uint8_t> bytes;
    append(bytes, hdr);
    append(bytes, desc);
    for (const auto& e : entries) append(bytes, e);
    for (uint32_t i = 0; i < 6; ++i) {
        append(bytes, payloadBytes);
        std::vector<uint8_t> payload(payloadBytes, uint8_t(i));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    REQUIRE(bytes.size() == cappedSize);

    writeFile(p, bytes);
    TilePyramid pyr = TilePyramid::open(p.string());
    REQUIRE_FALSE(pyr.isNull());
    CHECK(pyr.bytesTotal() == 6 * blobStride); // level 0 only
}

TEST_CASE("open(): valid minimal one-plane pack parses and reports byte totals") {
    const auto p = tmpPath("valid_minimal");

    constexpr uint16_t kTileSize = 4;
    constexpr uint16_t kGutter = 0;
    constexpr uint16_t kMipCount = 1;
    constexpr uint16_t kLevelCount = 1; // level 0 only: 6 tiles (one per face)
    const uint32_t tileCount = geTilePlaneTileCount(kLevelCount);
    REQUIRE(tileCount == 6);

    const uint64_t descsOff = sizeof(GeTilePackHeader);
    const uint64_t indexOffset = descsOff + sizeof(GeTilePlaneDesc);
    const GeTilePlaneDesc desc = makePlaneDesc(
        "id", GeTilePlaneEncoding::R8, kTileSize, kGutter, kMipCount, kLevelCount,
        indexOffset);

    // Payload: mipSizes[1] + (tileSize+2*gutter)^2 bytes of R8 payload.
    const uint32_t payloadBytes = uint32_t(kTileSize + 2 * kGutter) *
                                  uint32_t(kTileSize + 2 * kGutter);
    const uint64_t blobsOff = indexOffset + uint64_t(tileCount) * sizeof(GeTileEntry);
    const uint64_t blobStride = sizeof(uint32_t) * kMipCount + payloadBytes;

    std::vector<GeTileEntry> entries(tileCount);
    for (uint32_t i = 0; i < tileCount; ++i) {
        entries[i].offset = blobsOff + i * blobStride;
        entries[i].size = uint32_t(blobStride);
    }

    GeTilePackHeader hdr{};
    std::memcpy(hdr.magic, kGeTilePackMagic, 4);
    hdr.version = kGeTilePackVersion;
    hdr.planeCount = 1;
    hdr.fileSize = blobsOff + tileCount * blobStride;

    std::vector<uint8_t> bytes;
    append(bytes, hdr);
    append(bytes, desc);
    for (const auto& e : entries) append(bytes, e);
    for (uint32_t i = 0; i < tileCount; ++i) {
        const uint32_t mipSize = payloadBytes;
        append(bytes, mipSize);
        std::vector<uint8_t> payload(payloadBytes, uint8_t(0x42 + i));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    REQUIRE(bytes.size() == hdr.fileSize);

    writeFile(p, bytes);
    TilePyramid pyr = TilePyramid::open(p.string());
    REQUIRE_FALSE(pyr.isNull());
    CHECK(pyr.planeIndex("id") == 0);
    CHECK(pyr.planeIndex("rgb") == -1);
    CHECK(pyr.planeDesc(0).tileSize == kTileSize);
    CHECK(pyr.planeDesc(0).levelCount == kLevelCount);
    CHECK(pyr.bytesTotal() == tileCount * blobStride);
    // No sokol context in this test binary: pump() is inert, so nothing can
    // have become resident yet, but the object must still tear down cleanly.
    pyr.pump();
    CHECK_FALSE(pyr.baseResident());
    CHECK(pyr.bytesResident() == 0);
    // Destructor must join the reader thread and free everything without
    // crashing, even though the reader may still be mid-flight.
}

TEST_CASE("open(): missing file is rejected without crashing") {
    TilePyramid pyr = TilePyramid::open("/nonexistent/path/to/nowhere.getp");
    CHECK(pyr.isNull());
}
