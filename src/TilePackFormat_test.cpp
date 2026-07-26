// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.1 Round-trip oracle for the .getp cook (ge::cookTilePack). Cooks a
// tiny synthetic two-plane config (astc4x4 "rgb" + r8 "id") to a temp file
// and validates the on-disk layout against TilePackFormat.h by raw struct
// reads — no dependency on the (not-yet-built) runtime loader.
//
// The "id" plane's source is a constant byte value, not a pattern that
// depends on the cube-sphere sampling math — that keeps the r8 exact-value
// check independent of any legitimate reprojection subtlety (bilinear vs
// nearest, gutter extension, supersampling), while still exercising the
// real tile-slicing / encode / write path.

#include "../tools/TilePackWriter.h"

#include <ge/TilePackFormat.h>

#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace ge;
namespace fs = std::filesystem;

namespace {

void writeRawPlane(const fs::path& path, int w, int h, int channels, uint8_t value) {
    std::vector<uint8_t> bytes(size_t(w) * h * channels, value);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(bool(out));
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void writeRawPlaneU16(const fs::path& path, int w, int h, uint16_t value) {
    std::vector<uint8_t> bytes(size_t(w) * h * 2);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
        bytes[i * 2]     = uint8_t(value & 0xff);
        bytes[i * 2 + 1] = uint8_t(value >> 8);
    }
    std::ofstream out(path, std::ios::binary);
    REQUIRE(bool(out));
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> readWholeFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(bool(in));
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string planeName(const GeTilePlaneDesc& d) {
    return std::string(d.name, strnlen(d.name, sizeof(d.name)));
}

nlohmann::json buildConfig(const fs::path& dir, const std::string& outputName) {
    nlohmann::json cfg;
    cfg["output"] = (dir / outputName).string();
    cfg["levelCapOverride"] = nullptr;
    cfg["planes"] = nlohmann::json::array({
        {
            {"name", "rgb"}, {"encoding", "astc4x4"},
            {"tileSize", 32}, {"gutter", 2}, {"mips", 2}, {"levels", 2},
            {"input", {
                {"path", (dir / "rgb.bin").string()},
                {"width", 8}, {"height", 4}, {"channels", 3},
                {"filter", "linear"},
            }},
        },
        {
            {"name", "id"}, {"encoding", "r8"},
            {"tileSize", 32}, {"gutter", 2}, {"mips", 1}, {"levels", 2},
            {"input", {
                {"path", (dir / "id.bin").string()},
                {"width", 8}, {"height", 4}, {"channels", 1},
                {"filter", "nearest"},
            }},
        },
        {
            // 🎯T69.2 u16 country IDs: 274 regions overflowed u8, so IDs ride
            // as RG8 (id = R + G*256) from a raw u16 source.
            {"name", "id16"}, {"encoding", "rg8"},
            {"tileSize", 32}, {"gutter", 2}, {"mips", 1}, {"levels", 2},
            {"input", {
                {"path", (dir / "id16.bin").string()},
                {"width", 8}, {"height", 4}, {"channels", 1},
                {"dtype", "u16"}, {"filter", "nearest"},
            }},
        },
    });
    return cfg;
}

} // namespace

TEST_CASE("cookTilePack round-trip: layout, r8 exact values, truncation prefix") {
    fs::path dir = fs::temp_directory_path() /
        ("getp_test_" + std::to_string(uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()))));
    fs::create_directories(dir);

    writeRawPlane(dir / "rgb.bin", 8, 4, 3, 140);
    writeRawPlane(dir / "id.bin", 8, 4, 1, 200);
    writeRawPlaneU16(dir / "id16.bin", 8, 4, 300); // deliberately > 255

    nlohmann::json config = buildConfig(dir, "full.getp");
    std::string err;
    REQUIRE_MESSAGE(cookTilePack(config, err), "cook failed: " << err);

    std::vector<uint8_t> full = readWholeFile(dir / "full.getp");

    GeTilePackHeader header{};
    std::memcpy(&header, full.data(), sizeof(header));
    CHECK(std::memcmp(header.magic, kGeTilePackMagic, 4) == 0);
    CHECK(header.version == kGeTilePackVersion);
    CHECK(header.planeCount == 3);
    CHECK(header.fileSize == full.size());

    std::vector<GeTilePlaneDesc> descs(header.planeCount);
    std::memcpy(descs.data(), full.data() + sizeof(header), descs.size() * sizeof(GeTilePlaneDesc));

    int rgbPlane = -1, idPlane = -1, id16Plane = -1;
    for (size_t i = 0; i < descs.size(); ++i) {
        if (planeName(descs[i]) == "rgb") rgbPlane = int(i);
        if (planeName(descs[i]) == "id") idPlane = int(i);
        if (planeName(descs[i]) == "id16") id16Plane = int(i);
    }
    REQUIRE(rgbPlane >= 0);
    REQUIRE(idPlane >= 0);
    REQUIRE(id16Plane >= 0);
    CHECK(descs[id16Plane].encoding == uint16_t(GeTilePlaneEncoding::Rg8));
    CHECK(descs[idPlane].encoding == uint16_t(GeTilePlaneEncoding::R8));
    CHECK(descs[idPlane].tileSize == 32);
    CHECK(descs[idPlane].gutter == 2);
    CHECK(descs[idPlane].mipCount == 1);
    CHECK(descs[idPlane].levelCount == 2);
    CHECK(descs[idPlane].filterNearest == 1);
    CHECK(descs[rgbPlane].encoding == uint16_t(GeTilePlaneEncoding::Astc4x4));
    CHECK(descs[rgbPlane].filterNearest == 0);

    // ── entry table density: every entry real, in-bounds, and the whole
    // blob region is packed with no gaps or overlaps ──
    struct Span { uint64_t offset; uint32_t size; };
    std::vector<Span> spans;
    uint64_t blobRegionStart = sizeof(header) + descs.size() * sizeof(GeTilePlaneDesc);
    for (auto& d : descs) blobRegionStart += geTilePlaneTileCount(d.levelCount) * sizeof(GeTileEntry);

    for (size_t pi = 0; pi < descs.size(); ++pi) {
        uint32_t n = geTilePlaneTileCount(descs[pi].levelCount);
        std::vector<GeTileEntry> entries(n);
        std::memcpy(entries.data(), full.data() + descs[pi].indexOffset, n * sizeof(GeTileEntry));
        for (auto& e : entries) {
            CHECK(e.offset > 0);
            CHECK(e.size > 0);
            CHECK(e.offset + e.size <= header.fileSize);
            spans.push_back({e.offset, e.size});
        }
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.offset < b.offset; });
    REQUIRE(!spans.empty());
    CHECK(spans.front().offset == blobRegionStart);
    for (size_t i = 1; i < spans.size(); ++i) {
        CHECK(spans[i].offset == spans[i - 1].offset + spans[i - 1].size); // packed, no gaps/overlap
    }
    CHECK(spans.back().offset + spans.back().size == header.fileSize);

    // ── r8 plane payload round-trips exact sample values ──
    // Level 0, face 0 (PosX), tile (0,0) — entry index 0 within the id plane.
    uint32_t idx0 = geTileEntryIndex(0, 0, 0, 0);
    GeTileEntry idEntry{};
    std::memcpy(&idEntry, full.data() + descs[idPlane].indexOffset + idx0 * sizeof(GeTileEntry), sizeof(idEntry));
    const uint8_t* blob = full.data() + idEntry.offset;
    uint32_t mip0Size;
    std::memcpy(&mip0Size, blob, 4); // mipSizes[0] (mipCount==1, no more entries)
    int storedEdge = 32 + 2 * 2;
    CHECK(mip0Size == uint32_t(storedEdge * storedEdge));
    const uint8_t* payload = blob + 4; // past mipSizes[1]
    for (uint32_t i = 0; i < mip0Size; ++i) {
        CHECK(payload[i] == 200);
    }

    // ── rg8 plane: u16 value 300 round-trips as lo=44, hi=1 ──
    GeTileEntry id16Entry{};
    std::memcpy(&id16Entry, full.data() + descs[id16Plane].indexOffset + idx0 * sizeof(GeTileEntry),
                sizeof(id16Entry));
    const uint8_t* blob16 = full.data() + id16Entry.offset;
    uint32_t mip0Size16;
    std::memcpy(&mip0Size16, blob16, 4);
    CHECK(mip0Size16 == uint32_t(storedEdge * storedEdge * 2));
    const uint8_t* payload16 = blob16 + 4;
    for (uint32_t i = 0; i < uint32_t(storedEdge * storedEdge); ++i) {
        CHECK(payload16[i * 2] == 44);      // 300 & 0xff
        CHECK(payload16[i * 2 + 1] == 1);   // 300 >> 8
    }

    // ── truncation property: levelCapOverride=0 is a byte-identical prefix
    // of the full file, except the header's own fileSize field ──
    nlohmann::json cappedConfig = buildConfig(dir, "capped.getp");
    cappedConfig["levelCapOverride"] = 0;
    std::string err2;
    REQUIRE_MESSAGE(cookTilePack(cappedConfig, err2), "capped cook failed: " << err2);
    std::vector<uint8_t> capped = readWholeFile(dir / "capped.getp");

    GeTilePackHeader cappedHeader{};
    std::memcpy(&cappedHeader, capped.data(), sizeof(cappedHeader));
    CHECK(cappedHeader.fileSize == capped.size());
    REQUIRE(capped.size() < full.size()); // meaningfully shallower

    // fileSize lives at bytes [8,16) of GeTilePackHeader (magic[4]+version+planeCount = 8 bytes prefix).
    CHECK(std::memcmp(capped.data(), full.data(), 8) == 0);
    CHECK(std::memcmp(capped.data() + 16, full.data() + 16, capped.size() - 16) == 0);

    fs::remove_all(dir);
}
