// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "TilePackWriter.h"

#include <ge/CubeSphere.h>
#include <ge/TilePackFormat.h>

#include <astcenc.h>
// TilePackWriter.o is linked into both bin/ge-texpack and bin/ge-test (for
// the round-trip doctest); neither libge.a nor tools/texpack.cpp provides
// the stb_image read implementation (libge.a only pulls in the *write*
// half via png.cpp), so it's defined here — the one TU common to both link
// targets — rather than in a CLI main() as tools/texenc.cpp does.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace ge {

namespace {

namespace fs = std::filesystem;
constexpr int kMaxChannels = 4;

// ── small utilities ─────────────────────────────────────────────────────

std::string resolvePath(const std::string& baseDir, const std::string& path) {
    if (baseDir.empty() || path.empty() || path.front() == '/') return path;
    return (fs::path(baseDir) / path).string();
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Runs `body(i)` for i in [0,count) across hardware_concurrency threads.
// Falls back to serial execution below `minParallel` items.
void parallelFor(int count, int minParallel, const std::function<void(int)>& body) {
    if (count <= 0) return;
    unsigned nThreads = std::thread::hardware_concurrency();
    if (nThreads == 0) nThreads = 1;
    nThreads = std::min<unsigned>(nThreads, static_cast<unsigned>(count));
    if (count < minParallel || nThreads <= 1) {
        for (int i = 0; i < count; ++i) body(i);
        return;
    }
    std::vector<std::future<void>> futures;
    int chunk = (count + static_cast<int>(nThreads) - 1) / static_cast<int>(nThreads);
    for (unsigned t = 0; t < nThreads; ++t) {
        int start = static_cast<int>(t) * chunk;
        int end = std::min(count, start + chunk);
        if (start >= end) continue;
        futures.push_back(std::async(std::launch::async, [start, end, &body] {
            for (int i = start; i < end; ++i) body(i);
        }));
    }
    for (auto& f : futures) f.get();
}

// gunzip via `zcat` subprocess (no zlib vendored yet — see tools/TilePackWriter.h
// header comment / 🎯T168.1 cook report for the tradeoff).
std::vector<uint8_t> runZcat(const std::string& path) {
    std::string cmd = "zcat '" + path + "'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) throw std::runtime_error("popen(zcat) failed for " + path);
    std::vector<uint8_t> out;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    int rc = pclose(p);
    if (rc != 0) {
        throw std::runtime_error("zcat failed (exit " + std::to_string(rc) + ") for " + path);
    }
    return out;
}

std::vector<uint8_t> readMaybeGz(const std::string& path) {
    if (endsWith(path, ".gz")) return runZcat(path);
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ── plane config ────────────────────────────────────────────────────────

struct PlaneCfg {
    std::string name;
    GeTilePlaneEncoding encoding = GeTilePlaneEncoding::Astc4x4;
    int tileSize = 0;
    int gutter = 0;
    int mipCount = 1;
    int levelCount = 1;
    bool filterNearest = false;
    std::string inputPath;
    int rawWidth = 0, rawHeight = 0, rawChannels = 0;
    bool rawU16 = false; // input dtype u16 (little-endian); else u8
    bool isRaw = false;  // else PNG
};

GeTilePlaneEncoding parseEncoding(const std::string& s) {
    if (s == "astc4x4") return GeTilePlaneEncoding::Astc4x4;
    if (s == "r8") return GeTilePlaneEncoding::R8;
    if (s == "rg8") return GeTilePlaneEncoding::Rg8;
    if (s == "eac_r11" || s == "eacr11") return GeTilePlaneEncoding::EacR11;
    throw std::runtime_error("unknown plane encoding: " + s);
}

PlaneCfg parsePlaneCfg(const nlohmann::json& j) {
    PlaneCfg p;
    p.name = j.at("name").get<std::string>();
    if (p.name.size() >= sizeof(GeTilePlaneDesc{}.name)) {
        throw std::runtime_error("plane name too long (max 7 chars): " + p.name);
    }
    p.encoding = parseEncoding(j.at("encoding").get<std::string>());
    if (p.encoding == GeTilePlaneEncoding::EacR11) {
        // 🎯T168.1 v1 scope: EacR11 is a reserved enum value, not implemented.
        throw std::runtime_error("plane '" + p.name +
            "': eac_r11 encoding is reserved, not implemented in this cook (v1)");
    }
    p.tileSize = j.at("tileSize").get<int>();
    p.gutter = j.at("gutter").get<int>();
    p.mipCount = j.at("mips").get<int>();
    p.levelCount = j.at("levels").get<int>();
    if (p.tileSize <= 0 || p.gutter < 0 || p.mipCount <= 0 || p.levelCount <= 0) {
        throw std::runtime_error("plane '" + p.name + "': tileSize/gutter/mips/levels out of range");
    }

    const auto& in = j.at("input");
    p.inputPath = in.at("path").get<std::string>();
    p.filterNearest = in.value("filter", std::string("linear")) == "nearest";
    p.rawU16 = in.value("dtype", std::string("u8")) == "u16";

    if (p.encoding == GeTilePlaneEncoding::Rg8) {
        // Rg8 carries discrete 16-bit IDs: blending or mipping IDs would
        // manufacture nonexistent countries, and PNG sources can't carry u16.
        if (!p.filterNearest || p.mipCount != 1 || !p.rawU16) {
            throw std::runtime_error("plane '" + p.name +
                "': rg8 requires filter=nearest, mips=1, and a raw u16 input");
        }
    } else if (p.rawU16) {
        throw std::runtime_error("plane '" + p.name + "': dtype u16 is only valid with rg8");
    }

    if (endsWith(p.inputPath, ".png")) {
        p.isRaw = false;
    } else if (endsWith(p.inputPath, ".bin") || endsWith(p.inputPath, ".bin.gz")) {
        p.isRaw = true;
        p.rawWidth = in.at("width").get<int>();
        p.rawHeight = in.at("height").get<int>();
        p.rawChannels = in.at("channels").get<int>();
        if (p.rawWidth <= 0 || p.rawHeight <= 0 ||
            p.rawChannels <= 0 || p.rawChannels > kMaxChannels) {
            throw std::runtime_error("plane '" + p.name + "': invalid raw input dims/channels");
        }
    } else {
        throw std::runtime_error("plane '" + p.name + "': unrecognized input kind for " + p.inputPath);
    }
    return p;
}

// ── equirectangular source image ───────────────────────────────────────
//
// Values are stored un-normalized (0..255 float, one component per source
// channel) so nearest-sampled R8 planes round-trip byte-exact: float(byte)
// then std::round() on the way back out is lossless for integers in this
// range.

struct EquirectSource {
    int width = 0, height = 0, channels = 0;
    std::vector<float> data; // width*height*channels, row 0 = north (lat +90)

    int wrapX(int x) const {
        x %= width;
        if (x < 0) x += width;
        return x;
    }
    int clampY(int y) const { return std::clamp(y, 0, height - 1); }

    // lon in [-pi,pi], lat in [-pi/2,pi/2] (radians); see CubeSphere.h.
    void sampleNearest(float lon, float lat, float* out) const {
        float u = (lon + float(M_PI)) / (2.0f * float(M_PI));
        float v = (float(M_PI) / 2.0f - lat) / float(M_PI);
        int px = wrapX(static_cast<int>(std::floor(u * float(width))));
        int py = clampY(static_cast<int>(std::floor(v * float(height))));
        const float* p = &data[(static_cast<size_t>(py) * width + px) * channels];
        for (int c = 0; c < channels; ++c) out[c] = p[c];
    }

    void sampleBilinear(float lon, float lat, float* out) const {
        float u = (lon + float(M_PI)) / (2.0f * float(M_PI));
        float v = (float(M_PI) / 2.0f - lat) / float(M_PI);
        float fx = u * float(width) - 0.5f;
        float fy = v * float(height) - 0.5f;
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        float tx = fx - float(x0);
        float ty = fy - float(y0);
        int x0w = wrapX(x0), x1w = wrapX(x0 + 1);
        int y0c = clampY(y0), y1c = clampY(y0 + 1);
        const float* p00 = &data[(static_cast<size_t>(y0c) * width + x0w) * channels];
        const float* p10 = &data[(static_cast<size_t>(y0c) * width + x1w) * channels];
        const float* p01 = &data[(static_cast<size_t>(y1c) * width + x0w) * channels];
        const float* p11 = &data[(static_cast<size_t>(y1c) * width + x1w) * channels];
        for (int c = 0; c < channels; ++c) {
            float top = p00[c] * (1 - tx) + p10[c] * tx;
            float bot = p01[c] * (1 - tx) + p11[c] * tx;
            out[c] = top * (1 - ty) + bot * ty;
        }
    }
};

int desiredChannelsForPng(GeTilePlaneEncoding enc) {
    return enc == GeTilePlaneEncoding::R8 ? 1 : 4;
}

EquirectSource loadEquirectSource(const PlaneCfg& p, const std::string& resolvedPath) {
    EquirectSource src;
    if (p.isRaw) {
        std::vector<uint8_t> bytes = readMaybeGz(resolvedPath);
        size_t texels = size_t(p.rawWidth) * p.rawHeight * p.rawChannels;
        size_t expected = texels * (p.rawU16 ? 2 : 1);
        if (bytes.size() != expected) {
            throw std::runtime_error("plane '" + p.name + "': raw input " + resolvedPath +
                " has " + std::to_string(bytes.size()) + " bytes, expected " +
                std::to_string(expected));
        }
        src.width = p.rawWidth;
        src.height = p.rawHeight;
        src.channels = p.rawChannels;
        src.data.resize(texels);
        if (p.rawU16) {
            // Little-endian u16 → float. Exact for the full 0..65535 range
            // (floats hold integers ≤ 2^24), so nearest-sampled IDs
            // round-trip losslessly through the float pipeline.
            for (size_t i = 0; i < texels; ++i) {
                src.data[i] = float(uint16_t(bytes[i * 2]) | uint16_t(bytes[i * 2 + 1]) << 8);
            }
        } else {
            for (size_t i = 0; i < texels; ++i) src.data[i] = float(bytes[i]);
        }
    } else {
        int w = 0, h = 0, fileChannels = 0;
        int desired = desiredChannelsForPng(p.encoding);
        unsigned char* img = stbi_load(resolvedPath.c_str(), &w, &h, &fileChannels, desired);
        if (!img) {
            throw std::runtime_error("plane '" + p.name + "': stbi_load failed for " +
                resolvedPath + " (" +
                (stbi_failure_reason() ? stbi_failure_reason() : "unknown") + ")");
        }
        src.width = w;
        src.height = h;
        src.channels = desired;
        src.data.resize(size_t(w) * h * desired);
        size_t n = src.data.size();
        for (size_t i = 0; i < n; ++i) src.data[i] = float(img[i]);
        stbi_image_free(img);
    }
    if ((p.encoding == GeTilePlaneEncoding::R8 ||
         p.encoding == GeTilePlaneEncoding::Rg8) && src.channels != 1) {
        throw std::runtime_error("plane '" + p.name +
            "': r8/rg8 encoding requires a 1-channel input");
    }
    return src;
}

// ── supersampling ───────────────────────────────────────────────────────

// N x N bilinear taps per output texel so a leaf (or gutter) texel doesn't
// alias when many source equirect texels fall inside its footprint. Uses
// this level's own texel footprint across the face (independent of tile
// boundaries), so it scales correctly for both the finest level and every
// coarser level's directly-resampled gutters.
int supersampleN(int level, int tileSize, int srcWidth) {
    double texelsAcrossFaceEdge = double(tileSize) * double(uint64_t(1) << level);
    double footprintRadians = (M_PI / 2.0) / texelsAcrossFaceEdge;
    double srcTexelsPerOutputTexel = double(srcWidth) * footprintRadians / (2.0 * M_PI);
    int n = static_cast<int>(std::ceil(srcTexelsPerOutputTexel));
    return std::clamp(n, 1, 8);
}

// ── per-face payload mosaics (linear planes only) ──────────────────────
//
// A face's payload mosaic at level L is the whole face rendered at
// tileSize*2^L resolution (no gutter — gutters are always resampled
// directly per tile, see cookPlane). The leaf mosaic (finest configured
// level) is built by direct equirect supersampling; every coarser mosaic
// is a 2x box filter of the next-finer one. This chain always starts from
// the plane's true leaf level regardless of levelCapOverride, so a capped
// cook's level-0 payload is bit-identical to an uncapped cook's (required
// for the truncation byte-prefix property — see cookTilePack).

struct FaceMosaic {
    int size = 0; // texels per edge
    std::vector<float> data; // size*size*channels
};

FaceMosaic buildLeafFaceMosaic(CubeFace face, int leafLevel, int tileSize, int channels,
                                const EquirectSource& src) {
    FaceMosaic m;
    m.size = tileSize * int(uint64_t(1) << leafLevel);
    m.data.resize(size_t(m.size) * m.size * channels);
    int ssN = supersampleN(leafLevel, tileSize, src.width);
    parallelFor(m.size, 4, [&](int py) {
        std::array<float, kMaxChannels> acc{};
        std::array<float, kMaxChannels> tmp{};
        for (int px = 0; px < m.size; ++px) {
            acc.fill(0.0f);
            for (int j = 0; j < ssN; ++j) {
                for (int i = 0; i < ssN; ++i) {
                    float faceU = (float(px) + (float(i) + 0.5f) / float(ssN)) / float(m.size);
                    float faceV = (float(py) + (float(j) + 0.5f) / float(ssN)) / float(m.size);
                    la::float3 dir = cubeFaceDir(face, faceU, faceV);
                    la::float2 ll = lonLatForDir(dir);
                    src.sampleBilinear(ll.x, ll.y, tmp.data());
                    for (int c = 0; c < channels; ++c) acc[c] += tmp[c];
                }
            }
            float* dst = &m.data[(size_t(py) * m.size + px) * channels];
            float inv = 1.0f / float(ssN * ssN);
            for (int c = 0; c < channels; ++c) dst[c] = acc[c] * inv;
        }
    });
    return m;
}

FaceMosaic coarsenFaceMosaic(const FaceMosaic& fine, int channels) {
    FaceMosaic m;
    m.size = std::max(1, fine.size / 2);
    m.data.assign(size_t(m.size) * m.size * channels, 0.0f);
    for (int y = 0; y < m.size; ++y) {
        for (int x = 0; x < m.size; ++x) {
            std::array<float, kMaxChannels> acc{};
            int n = 0;
            for (int sy = 0; sy < 2 && y * 2 + sy < fine.size; ++sy) {
                for (int sx = 0; sx < 2 && x * 2 + sx < fine.size; ++sx) {
                    const float* p = &fine.data[(size_t(y * 2 + sy) * fine.size + (x * 2 + sx)) * channels];
                    for (int c = 0; c < channels; ++c) acc[c] += p[c];
                    ++n;
                }
            }
            float* dst = &m.data[(size_t(y) * m.size + x) * channels];
            for (int c = 0; c < channels; ++c) dst[c] = acc[c] / float(n);
        }
    }
    return m;
}

// Full mosaic pyramid for one plane, indexed [level][face]. Only built for
// linear-filtered planes (nearest planes resample every level directly —
// see directSampleStoredTile).
std::vector<std::array<FaceMosaic, kCubeFaceCount>> buildMosaicPyramid(
        const PlaneCfg& p, const EquirectSource& src) {
    std::vector<std::array<FaceMosaic, kCubeFaceCount>> levels(p.levelCount);
    int leaf = p.levelCount - 1;
    for (int f = 0; f < kCubeFaceCount; ++f) {
        levels[leaf][f] = buildLeafFaceMosaic(static_cast<CubeFace>(f), leaf, p.tileSize,
                                              src.channels, src);
    }
    for (int L = leaf - 1; L >= 0; --L) {
        for (int f = 0; f < kCubeFaceCount; ++f) {
            levels[L][f] = coarsenFaceMosaic(levels[L + 1][f], src.channels);
        }
    }
    return levels;
}

// ── per-tile stored-canvas construction ────────────────────────────────
//
// Builds the (tileSize+2*gutter)^2 stored canvas for one tile. Gutter
// texels always come from direct equirect resampling — cubeFaceDir extends
// continuously past a face's [0,1] UV range, which is precisely how a
// tile's gutter picks up its neighbor's content (see CubeSphere.h). Payload
// texels come from the same direct resampling for nearest planes / the
// leaf level, or are supplied by the caller from the box-filtered mosaic
// for a linear plane's non-leaf levels (mosaic == nullptr disables the
// skip and direct-samples the whole canvas).
void directSampleStoredTile(CubeFace face, uint8_t level, uint16_t tx, uint16_t ty,
                             int tileSize, int gutter, int channels,
                             const EquirectSource& src, bool nearest, int ssN,
                             bool skipPayloadInterior,
                             std::vector<float>& canvas) {
    int storedEdge = tileSize + 2 * gutter;
    TileRect rect = tileRect(level, tx, ty);
    std::array<float, kMaxChannels> acc{};
    std::array<float, kMaxChannels> tmp{};
    for (int py = 0; py < storedEdge; ++py) {
        for (int px = 0; px < storedEdge; ++px) {
            bool inPayload = px >= gutter && px < gutter + tileSize &&
                              py >= gutter && py < gutter + tileSize;
            if (skipPayloadInterior && inPayload) continue;
            if (nearest) {
                float pu = float(px - gutter) + 0.5f;
                float pv = float(py - gutter) + 0.5f;
                float faceU = rect.u0 + (pu / float(tileSize)) * rect.extent;
                float faceV = rect.v0 + (pv / float(tileSize)) * rect.extent;
                la::float3 dir = cubeFaceDir(face, faceU, faceV);
                la::float2 ll = lonLatForDir(dir);
                src.sampleNearest(ll.x, ll.y, acc.data());
            } else {
                acc.fill(0.0f);
                for (int j = 0; j < ssN; ++j) {
                    for (int i = 0; i < ssN; ++i) {
                        float pu = float(px - gutter) + (float(i) + 0.5f) / float(ssN);
                        float pv = float(py - gutter) + (float(j) + 0.5f) / float(ssN);
                        float faceU = rect.u0 + (pu / float(tileSize)) * rect.extent;
                        float faceV = rect.v0 + (pv / float(tileSize)) * rect.extent;
                        la::float3 dir = cubeFaceDir(face, faceU, faceV);
                        la::float2 ll = lonLatForDir(dir);
                        src.sampleBilinear(ll.x, ll.y, tmp.data());
                        for (int c = 0; c < channels; ++c) acc[c] += tmp[c];
                    }
                }
                float inv = 1.0f / float(ssN * ssN);
                for (int c = 0; c < channels; ++c) acc[c] *= inv;
            }
            float* dst = &canvas[(size_t(py) * storedEdge + px) * channels];
            for (int c = 0; c < channels; ++c) dst[c] = acc[c];
        }
    }
}

void cropMosaicPayload(const FaceMosaic& mosaic, int tx, int ty, int tileSize, int gutter,
                        int channels, std::vector<float>& canvas) {
    int storedEdge = tileSize + 2 * gutter;
    for (int y = 0; y < tileSize; ++y) {
        const float* src = &mosaic.data[(size_t(ty * tileSize + y) * mosaic.size + tx * tileSize) * channels];
        float* dst = &canvas[(size_t(y + gutter) * storedEdge + gutter) * channels];
        std::memcpy(dst, src, size_t(tileSize) * channels * sizeof(float));
    }
}

// ── per-tile mip chain (box filter, texenc's downscale2x pattern) ──────

std::vector<float> boxDownscale2xGeneric(const std::vector<float>& src, int w, int h, int channels) {
    int dw = std::max(1, w / 2), dh = std::max(1, h / 2);
    std::vector<float> dst(size_t(dw) * dh * channels, 0.0f);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            std::array<float, kMaxChannels> acc{};
            int n = 0;
            for (int sy = 0; sy < 2 && y * 2 + sy < h; ++sy) {
                for (int sx = 0; sx < 2 && x * 2 + sx < w; ++sx) {
                    const float* p = &src[(size_t(y * 2 + sy) * w + (x * 2 + sx)) * channels];
                    for (int c = 0; c < channels; ++c) acc[c] += p[c];
                    ++n;
                }
            }
            float* d = &dst[(size_t(y) * dw + x) * channels];
            for (int c = 0; c < channels; ++c) d[c] = acc[c] / float(n);
        }
    }
    return dst;
}

// ── encode ──────────────────────────────────────────────────────────────

// Mirrors ge::TextureEncoder.cpp's astcEncode (anonymous-namespace, not
// exported) — duplicated rather than exposing it, since TextureEncoder.h is
// a stable public surface this cook shouldn't grow into. Already handles
// non-multiple-of-4 dims (verified against texenc's mip chain, which goes
// down to 1x1): astcenc pads internally, block count uses ceil-division.
std::vector<uint8_t> astcEncode4x4(const uint8_t* rgba8, int w, int h) {
    astcenc_config config{};
    astcenc_error status = astcenc_config_init(
        ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_MEDIUM, 0, &config);
    if (status != ASTCENC_SUCCESS) {
        throw std::runtime_error(std::string("astcenc_config_init: ") + astcenc_get_error_string(status));
    }
    astcenc_context* ctx = nullptr;
    status = astcenc_context_alloc(&config, 1, &ctx);
    if (status != ASTCENC_SUCCESS) {
        throw std::runtime_error(std::string("astcenc_context_alloc: ") + astcenc_get_error_string(status));
    }
    uint8_t* slices = const_cast<uint8_t*>(rgba8);
    astcenc_image image{};
    image.dim_x = static_cast<unsigned int>(w);
    image.dim_y = static_cast<unsigned int>(h);
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = reinterpret_cast<void**>(&slices);

    size_t blocksX = (size_t(w) + 3) / 4;
    size_t blocksY = (size_t(h) + 3) / 4;
    std::vector<uint8_t> comp(blocksX * blocksY * 16);

    astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_1};
    status = astcenc_compress_image(ctx, &image, &swizzle, comp.data(), comp.size(), 0);
    astcenc_context_free(ctx);
    if (status != ASTCENC_SUCCESS) {
        throw std::runtime_error(std::string("astcenc_compress_image: ") + astcenc_get_error_string(status));
    }
    return comp;
}

uint8_t quantizeByte(float v) {
    return uint8_t(std::clamp(std::round(v), 0.0f, 255.0f));
}

std::vector<uint8_t> expandToRgba8(const std::vector<float>& mip, int w, int h, int channels) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
        const float* s = &mip[i * channels];
        uint8_t* d = &rgba[i * 4];
        if (channels == 1) {
            d[0] = d[1] = d[2] = quantizeByte(s[0]);
            d[3] = 255;
        } else if (channels == 3) {
            d[0] = quantizeByte(s[0]); d[1] = quantizeByte(s[1]); d[2] = quantizeByte(s[2]);
            d[3] = 255;
        } else { // 4
            d[0] = quantizeByte(s[0]); d[1] = quantizeByte(s[1]);
            d[2] = quantizeByte(s[2]); d[3] = quantizeByte(s[3]);
        }
    }
    return rgba;
}

// Deterministic — depends only on plane config, never on pixel content. This
// is what makes the coarse-to-fine blob layout computable before any actual
// encoding happens (see cookTilePack's layout pass).
uint64_t mipBlobSize(int w, int h, GeTilePlaneEncoding enc) {
    if (enc == GeTilePlaneEncoding::R8) return uint64_t(w) * h;
    if (enc == GeTilePlaneEncoding::Rg8) return uint64_t(w) * h * 2;
    return uint64_t((w + 3) / 4) * uint64_t((h + 3) / 4) * 16; // Astc4x4
}

uint64_t computeBlobSize(const PlaneCfg& p) {
    int storedEdge = p.tileSize + 2 * p.gutter;
    uint64_t total = uint64_t(p.mipCount) * 4; // mipSizes[] header
    int w = storedEdge, h = storedEdge;
    for (int m = 0; m < p.mipCount; ++m) {
        total += mipBlobSize(w, h, p.encoding);
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return total;
}

// Builds one tile's full blob (mipSizes[] + concatenated mip payloads) from
// its stored canvas.
std::vector<uint8_t> encodeTileBlob(const std::vector<float>& canvas0, int storedEdge,
                                    int channels, const PlaneCfg& p) {
    std::vector<std::vector<uint8_t>> mipBytes;
    mipBytes.reserve(p.mipCount);
    std::vector<float> mip = canvas0;
    int w = storedEdge, h = storedEdge;
    for (int m = 0; m < p.mipCount; ++m) {
        if (p.encoding == GeTilePlaneEncoding::R8) {
            std::vector<uint8_t> bytes(size_t(w) * h);
            for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = quantizeByte(mip[i]);
            mipBytes.push_back(std::move(bytes));
        } else if (p.encoding == GeTilePlaneEncoding::Rg8) {
            std::vector<uint8_t> bytes(size_t(w) * h * 2);
            for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
                uint32_t v = uint32_t(std::clamp(std::round(mip[i]), 0.0f, 65535.0f));
                bytes[i * 2]     = uint8_t(v & 0xff);
                bytes[i * 2 + 1] = uint8_t(v >> 8);
            }
            mipBytes.push_back(std::move(bytes));
        } else {
            auto rgba = expandToRgba8(mip, w, h, channels);
            mipBytes.push_back(astcEncode4x4(rgba.data(), w, h));
        }
        if (m + 1 < p.mipCount) {
            mip = boxDownscale2xGeneric(mip, w, h, channels);
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
    }
    std::vector<uint8_t> blob(size_t(p.mipCount) * 4);
    for (int m = 0; m < p.mipCount; ++m) {
        uint32_t sz = static_cast<uint32_t>(mipBytes[m].size());
        std::memcpy(blob.data() + m * 4, &sz, 4);
    }
    for (auto& mb : mipBytes) blob.insert(blob.end(), mb.begin(), mb.end());
    return blob;
}

} // namespace

// ── top-level cook ──────────────────────────────────────────────────────
//
// Layout (header, plane descs, per-plane index arrays, entry offsets) is
// computed unconditionally for every plane's FULL configured `levels` in
// one deterministic pass, before any blob bytes are written — offsets and
// sizes depend only on tileSize/gutter/mipCount/encoding (see
// computeBlobSize), never on levelCapOverride or actual pixel content. A
// capped cook then simply stops emitting blob bytes once it passes the
// capped level, leaving that plane's higher-level index entries populated
// but dangling (no bytes at that offset in THIS file — resolved by the
// caller passing the same cap to TilePyramidOptions, or by a CDN serving a
// physically byte-truncated file). This is what makes "cook with
// levelCapOverride=K" byte-identical to "truncate the fully-cooked file
// after level K's blobs" — required by the round-trip test's truncation
// check, and the mechanism the header comment calls out ("a byte-prefix
// truncated at levelTruncOffset[k] is a valid shallower pack").
bool cookTilePack(const nlohmann::json& config, std::string& err, const std::string& baseDir) {
    try {
        std::string outputPath = resolvePath(baseDir, config.at("output").get<std::string>());

        std::vector<PlaneCfg> planes;
        for (const auto& pj : config.at("planes")) planes.push_back(parsePlaneCfg(pj));
        if (planes.empty()) throw std::runtime_error("config has no planes");

        int cap = -1;
        if (config.contains("levelCapOverride") && !config.at("levelCapOverride").is_null()) {
            cap = config.at("levelCapOverride").get<int>();
        }

        // 1. Load sources.
        std::vector<EquirectSource> sources;
        sources.reserve(planes.size());
        for (auto& p : planes) {
            std::string resolved = resolvePath(baseDir, p.inputPath);
            SPDLOG_INFO("ge-texpack: loading plane '{}' from {}", p.name, resolved);
            sources.push_back(loadEquirectSource(p, resolved));
        }

        // 2. Deterministic per-plane blob size.
        std::vector<uint64_t> blobSize(planes.size());
        for (size_t i = 0; i < planes.size(); ++i) blobSize[i] = computeBlobSize(planes[i]);

        // 3. Header + desc layout.
        GeTilePackHeader header{};
        std::memcpy(header.magic, kGeTilePackMagic, 4);
        header.version = kGeTilePackVersion;
        header.planeCount = static_cast<uint16_t>(planes.size());

        std::vector<GeTilePlaneDesc> descs(planes.size());
        uint64_t cursor = sizeof(GeTilePackHeader) + planes.size() * sizeof(GeTilePlaneDesc);
        std::vector<uint64_t> tileCounts(planes.size());
        for (size_t i = 0; i < planes.size(); ++i) {
            const auto& p = planes[i];
            GeTilePlaneDesc& d = descs[i];
            std::memset(&d, 0, sizeof(d));
            std::memcpy(d.name, p.name.c_str(), p.name.size());
            d.encoding = static_cast<uint16_t>(p.encoding);
            d.tileSize = static_cast<uint16_t>(p.tileSize);
            d.gutter = static_cast<uint16_t>(p.gutter);
            d.mipCount = static_cast<uint16_t>(p.mipCount);
            d.levelCount = static_cast<uint16_t>(p.levelCount);
            d.filterNearest = p.filterNearest ? 1 : 0;
            d.indexOffset = cursor;
            tileCounts[i] = geTilePlaneTileCount(p.levelCount);
            cursor += tileCounts[i] * sizeof(GeTileEntry);
        }
        uint64_t blobRegionStart = cursor;

        // 4. Deterministic entry layout across all planes/levels (coarse to
        // fine, planes interleaved per level) + the byte offset marking the
        // end of level `cap` (used as this run's fileSize when capped).
        int maxLevels = 0;
        for (auto& p : planes) maxLevels = std::max(maxLevels, p.levelCount);

        std::vector<std::vector<GeTileEntry>> entries(planes.size());
        for (size_t i = 0; i < planes.size(); ++i) entries[i].assign(tileCounts[i], GeTileEntry{0, 0, 0});

        uint64_t running = blobRegionStart;
        uint64_t cutoff = blobRegionStart;
        for (int level = 0; level < maxLevels; ++level) {
            for (size_t pi = 0; pi < planes.size(); ++pi) {
                const auto& p = planes[pi];
                if (level >= p.levelCount) continue;
                int side = 1 << level;
                for (int face = 0; face < kCubeFaceCount; ++face) {
                    for (int ty = 0; ty < side; ++ty) {
                        for (int tx = 0; tx < side; ++tx) {
                            uint32_t idx = geTileEntryIndex(uint16_t(level), uint8_t(face),
                                                            uint16_t(tx), uint16_t(ty));
                            entries[pi][idx] = {running, uint32_t(blobSize[pi]), 0};
                            running += blobSize[pi];
                        }
                    }
                }
            }
            if (cap >= 0 && level == cap) cutoff = running;
        }
        if (cap < 0 || cap >= maxLevels - 1) cutoff = running;
        header.fileSize = cutoff;

        // 5. Write header + descs + index arrays (identical regardless of cap).
        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open output " + outputPath + " for writing");
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(descs.data()), std::streamsize(descs.size() * sizeof(GeTilePlaneDesc)));
        for (auto& e : entries) {
            out.write(reinterpret_cast<const char*>(e.data()), std::streamsize(e.size() * sizeof(GeTileEntry)));
        }

        // 6. Encode + write blobs, coarse to fine, stopping after level `cap`.
        // Mosaic pyramids (linear planes only) are always built through the
        // plane's true leaf level regardless of cap — see the function
        // comment on buildMosaicPyramid for why that's load-bearing.
        std::vector<std::vector<std::array<FaceMosaic, kCubeFaceCount>>> mosaics(planes.size());
        for (size_t pi = 0; pi < planes.size(); ++pi) {
            if (!planes[pi].filterNearest) mosaics[pi] = buildMosaicPyramid(planes[pi], sources[pi]);
        }

        for (int level = 0; level < maxLevels; ++level) {
            if (cap >= 0 && level > cap) break;
            for (size_t pi = 0; pi < planes.size(); ++pi) {
                const auto& p = planes[pi];
                if (level >= p.levelCount) continue;
                int side = 1 << level;
                int storedEdge = p.tileSize + 2 * p.gutter;
                int ssN = supersampleN(level, p.tileSize, sources[pi].width);
                bool isLeaf = level == p.levelCount - 1;
                bool haveMosaic = !p.filterNearest;

                int tileCount = kCubeFaceCount * side * side;
                std::vector<std::vector<uint8_t>> blobs(tileCount);
                parallelFor(tileCount, 1, [&](int flat) {
                    int face = flat / (side * side);
                    int rem = flat % (side * side);
                    int ty = rem / side;
                    int tx = rem % side;
                    std::vector<float> canvas(size_t(storedEdge) * storedEdge * sources[pi].channels, 0.0f);
                    bool skipPayload = haveMosaic; // payload comes from the mosaic instead
                    directSampleStoredTile(static_cast<CubeFace>(face), uint8_t(level),
                                           uint16_t(tx), uint16_t(ty), p.tileSize, p.gutter,
                                           sources[pi].channels, sources[pi], p.filterNearest, ssN,
                                           skipPayload, canvas);
                    if (haveMosaic) {
                        cropMosaicPayload(mosaics[pi][level][face], tx, ty, p.tileSize, p.gutter,
                                          sources[pi].channels, canvas);
                    }
                    blobs[flat] = encodeTileBlob(canvas, storedEdge, sources[pi].channels, p);
                });

                for (auto& b : blobs) {
                    if (b.size() != blobSize[pi]) {
                        throw std::runtime_error("internal error: blob size mismatch for plane '" +
                            p.name + "' (layout said " + std::to_string(blobSize[pi]) +
                            ", encoded " + std::to_string(b.size()) + ")");
                    }
                    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
                }
                (void)isLeaf;
                SPDLOG_INFO("ge-texpack: plane '{}' level {} ({} tiles) written", p.name, level, tileCount);
            }
        }

        if (!out.good()) throw std::runtime_error("write failure on " + outputPath);
        out.close();
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace ge
