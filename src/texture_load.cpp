// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/texture.h>

#include <ge/FileIO.h>
#include <ge/GeTexFormat.h>
#include <ge/Resource.h>
#include <ge/png.h>

#include "sokol_gfx.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_image/SDL_image.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace ge {

namespace {

std::string stripKnownExt(const std::string& p) {
    static const char* kExts[] = {".astc.getex", ".etc2.getex", ".png.getex",
                                  ".astc",       ".png",        ".jpg",
                                  ".jpeg",       ".JPG",        ".JPEG"};
    for (const char* e : kExts) {
        const size_t n = std::strlen(e);
        if (p.size() >= n && p.compare(p.size() - n, n, e) == 0)
            return p.substr(0, p.size() - n);
    }
    return p;
}

bool defaultExists(const std::string& path) {
    const std::string resolved = ge::resource(path);
    auto f = ge::openFile(path, true);
    if (f && *f) return true;
    // openFile may use resource internally; also try direct open of resolved
    std::ifstream in(resolved, std::ios::binary);
    return in.good();
}

struct GetexDecoded {
    ge::GeTexHeader        hdr{};
    std::vector<uint32_t>  levelSizes;
    std::vector<uint8_t>   file;   // full file; levels point into this
    std::vector<sg_range>  levels; // ptr into file
    bool                   ok = false;
};

GetexDecoded decodeGetexBytes(std::vector<uint8_t> file, const char* label) {
    GetexDecoded out;
    out.file = std::move(file);
    if (out.file.size() < sizeof(ge::GeTexHeader)) {
        SPDLOG_ERROR("{}: truncated getex header", label);
        return out;
    }
    std::memcpy(&out.hdr, out.file.data(), sizeof(out.hdr));
    if (std::memcmp(out.hdr.magic, ge::kGeTexMagic, 4) != 0) {
        SPDLOG_ERROR("{}: bad getex magic", label);
        return out;
    }
    if (out.hdr.mipCount == 0 || out.hdr.mipCount > SG_MAX_MIPMAPS) {
        SPDLOG_ERROR("{}: bad mipCount {}", label, out.hdr.mipCount);
        return out;
    }
    const size_t sizesOff = sizeof(ge::GeTexHeader);
    const size_t sizesBytes = size_t(out.hdr.mipCount) * sizeof(uint32_t);
    if (out.file.size() < sizesOff + sizesBytes) {
        SPDLOG_ERROR("{}: truncated mip sizes", label);
        return out;
    }
    out.levelSizes.resize(out.hdr.mipCount);
    std::memcpy(out.levelSizes.data(), out.file.data() + sizesOff, sizesBytes);
    size_t off = sizesOff + sizesBytes;
    out.levels.resize(out.hdr.mipCount);
    for (uint16_t i = 0; i < out.hdr.mipCount; ++i) {
        if (off + out.levelSizes[i] > out.file.size()) {
            SPDLOG_ERROR("{}: truncated level {}", label, i);
            return out;
        }
        out.levels[i] = sg_range{out.file.data() + off, out.levelSizes[i]};
        off += out.levelSizes[i];
    }
    out.ok = true;
    return out;
}

GetexDecoded loadGetexFile(const std::string& path) {
    auto stream = ge::openFile(path, true);
    if (!stream || !*stream) {
        SPDLOG_ERROR("open getex {}: not found", path);
        return {};
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(*stream)),
                              std::istreambuf_iterator<char>());
    return decodeGetexBytes(std::move(file), path.c_str());
}

sg_pixel_format formatForEncoding(uint16_t enc) {
    switch (static_cast<ge::GeTexEncoding>(enc)) {
    case ge::GeTexEncoding::Astc4x4:
        return SG_PIXELFORMAT_ASTC_4x4_RGBA;
    case ge::GeTexEncoding::Etc2Rgba8:
        return SG_PIXELFORMAT_ETC2_RGBA8;
    case ge::GeTexEncoding::Png:
        return SG_PIXELFORMAT_RGBA8; // after decode
    }
    return SG_PIXELFORMAT_NONE;
}

// Decode PNG levels from getex (encoding Png) into contiguous RGBA mip chain
// stored in `rgbaOut` (level ranges relative to that buffer).
bool expandPngGetex(const GetexDecoded& g, std::vector<uint8_t>& rgbaOut,
                    std::vector<sg_range>& levelRanges, int& w0, int& h0) {
    // PNG-in-getex: each level is a PNG blob. Decode via SDL_image.
    w0 = int(g.hdr.width);
    h0 = int(g.hdr.height);
    levelRanges.clear();
    rgbaOut.clear();
    int w = w0, h = h0;
    for (uint16_t i = 0; i < g.hdr.mipCount; ++i) {
        SDL_IOStream* io = SDL_IOFromConstMem(g.levels[i].ptr, int(g.levels[i].size));
        if (!io) return false;
        SDL_Surface* s = IMG_Load_IO(io, true);
        if (!s) return false;
        SDL_Surface* rgba = s;
        if (s->format != SDL_PIXELFORMAT_RGBA32) {
            rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(s);
            if (!rgba) return false;
        }
        if (rgba->w != w || rgba->h != h) {
            SDL_DestroySurface(rgba);
            return false;
        }
        const size_t nbytes = size_t(w) * size_t(h) * 4u;
        const size_t base = rgbaOut.size();
        rgbaOut.resize(base + nbytes);
        // Copy with pitch
        const uint8_t* src = static_cast<const uint8_t*>(rgba->pixels);
        for (int y = 0; y < h; ++y) {
            std::memcpy(rgbaOut.data() + base + size_t(y) * w * 4,
                        src + size_t(y) * rgba->pitch, size_t(w) * 4);
        }
        SDL_DestroySurface(rgba);
        levelRanges.push_back(sg_range{rgbaOut.data() + base, nbytes});
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return true;
}

GpuTexture upload2dGetex(const GetexDecoded& g, const char* label) {
    GpuTexture out;
    if (!g.ok) return out;

    if (static_cast<ge::GeTexEncoding>(g.hdr.encoding) == ge::GeTexEncoding::Png) {
        std::vector<uint8_t> rgba;
        std::vector<sg_range> ranges;
        int w = 0, h = 0;
        if (!expandPngGetex(g, rgba, ranges, w, h)) {
            SPDLOG_ERROR("{}: PNG getex decode failed", label);
            return out;
        }
        sg_image_desc d{};
        d.width = w;
        d.height = h;
        d.num_mipmaps = int(ranges.size());
        d.pixel_format = SG_PIXELFORMAT_RGBA8;
        d.label = label;
        for (size_t i = 0; i < ranges.size(); ++i)
            d.data.mip_levels[i] = ranges[i];
        out.image = sg_make_image(&d);
        out.width = w;
        out.height = h;
        out.numMips = int(ranges.size());
        out.format = SG_PIXELFORMAT_RGBA8;
    } else {
        const sg_pixel_format fmt = formatForEncoding(g.hdr.encoding);
        if (fmt == SG_PIXELFORMAT_NONE || !sg_query_pixelformat(fmt).sample) {
            SPDLOG_ERROR("{}: pixel format not sampleable", label);
            return out;
        }
        sg_image_desc d{};
        d.width = int(g.hdr.width);
        d.height = int(g.hdr.height);
        d.num_mipmaps = int(g.hdr.mipCount);
        d.pixel_format = fmt;
        d.label = label;
        for (uint16_t i = 0; i < g.hdr.mipCount; ++i)
            d.data.mip_levels[i] = g.levels[i];
        out.image = sg_make_image(&d);
        out.width = int(g.hdr.width);
        out.height = int(g.hdr.height);
        out.numMips = int(g.hdr.mipCount);
        out.format = fmt;
    }
    if (out.image.id == SG_INVALID_ID) {
        SPDLOG_ERROR("{}: sg_make_image failed", label);
        return {};
    }
    sg_view_desc vd{};
    vd.texture.image = out.image;
    vd.label = label;
    out.view = sg_make_view(&vd);
    out.type = SG_IMAGETYPE_2D;
    return out;
}

bool isGetexPath(const std::string& p) {
    return p.size() >= 6 && p.compare(p.size() - 6, 6, ".getex") == 0;
}

} // namespace

// ── GpuTexture ─────────────────────────────────────────────────────────────

GpuTexture::~GpuTexture() { destroy(); }

GpuTexture::GpuTexture(GpuTexture&& o) noexcept {
    image = o.image;
    view = o.view;
    width = o.width;
    height = o.height;
    numMips = o.numMips;
    type = o.type;
    format = o.format;
    o.image = {};
    o.view = {};
}

GpuTexture& GpuTexture::operator=(GpuTexture&& o) noexcept {
    if (this != &o) {
        destroy();
        image = o.image;
        view = o.view;
        width = o.width;
        height = o.height;
        numMips = o.numMips;
        type = o.type;
        format = o.format;
        o.image = {};
        o.view = {};
    }
    return *this;
}

void GpuTexture::destroy() {
    if (sg_isvalid()) {
        if (view.id != SG_INVALID_ID) sg_destroy_view(view);
        if (image.id != SG_INVALID_ID) sg_destroy_image(image);
    }
    view = {};
    image = {};
    width = height = 0;
    numMips = 1;
    type = SG_IMAGETYPE_2D;
    format = SG_PIXELFORMAT_NONE;
}

// ── Path selection ─────────────────────────────────────────────────────────

std::vector<std::string> textureCandidatePaths(const std::string& pathOrStem,
                                               bool hasAstc, bool hasEtc2) {
    std::vector<std::string> out;
    auto pushUnique = [&](const std::string& p) {
        if (p.empty()) return;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };

    // Concrete file first if it looks like one.
    if (pathOrStem.find('.') != std::string::npos) {
        pushUnique(pathOrStem);
    }

    const std::string stem = stripKnownExt(pathOrStem);
    if (hasAstc) pushUnique(stem + ".astc.getex");
    if (hasEtc2) pushUnique(stem + ".etc2.getex");
    pushUnique(stem + ".png.getex");
    pushUnique(stem + ".png");
    pushUnique(stem + ".jpg");
    pushUnique(stem + ".jpeg");
    // If ASTC/ETC2 not preferred, still list them last so an explicit pack works
    // when the backend later gains support (resolve uses exists only).
    if (!hasAstc) pushUnique(stem + ".astc.getex");
    if (!hasEtc2) pushUnique(stem + ".etc2.getex");
    return out;
}

std::string resolveTexturePath(
    const std::string& pathOrStem, bool hasAstc, bool hasEtc2,
    const std::function<bool(const std::string&)>& exists) {
    for (const auto& c : textureCandidatePaths(pathOrStem, hasAstc, hasEtc2)) {
        if (exists(c)) return c;
    }
    return {};
}

bool textureBackendHasAstc() {
    return sg_isvalid() && sg_query_pixelformat(SG_PIXELFORMAT_ASTC_4x4_RGBA).sample;
}

bool textureBackendHasEtc2() {
    return sg_isvalid() && sg_query_pixelformat(SG_PIXELFORMAT_ETC2_RGBA8).sample;
}

std::string resolveTexturePath(const std::string& pathOrStem) {
    return resolveTexturePath(pathOrStem, textureBackendHasAstc(),
                              textureBackendHasEtc2(), defaultExists);
}

// ── Load ───────────────────────────────────────────────────────────────────

GpuTexture loadTexture(const std::string& pathOrStem) {
    if (!sg_isvalid()) {
        SPDLOG_ERROR("ge::loadTexture: sokol not initialized");
        return {};
    }
    const std::string path = resolveTexturePath(pathOrStem);
    if (path.empty()) {
        SPDLOG_ERROR("ge::loadTexture: no candidate for '{}'", pathOrStem);
        return {};
    }

    if (isGetexPath(path)) {
        GetexDecoded g = loadGetexFile(path);
        GpuTexture t = upload2dGetex(g, path.c_str());
        if (!t.isNull())
            SPDLOG_INFO("ge::loadTexture: {} ({}x{}, {} mips)", path, t.width,
                        t.height, t.numMips);
        return t;
    }

    // Raw image via existing PNG/JPEG path.
    Sprite spr = loadImage(path);
    if (spr.isNull()) return {};
    GpuTexture t;
    t.image = spr.tex;
    t.view = spr.view;
    t.width = spr.width;
    t.height = spr.height;
    t.format = SG_PIXELFORMAT_RGBA8;
    t.type = SG_IMAGETYPE_2D;
    // Detach from Sprite so it doesn't destroy on return.
    spr.tex = {};
    spr.view = {};
    SPDLOG_INFO("ge::loadTexture: {} ({}x{}, rgba)", path, t.width, t.height);
    return t;
}

GpuTexture loadCube(const std::string& facePrefix) {
    if (!sg_isvalid()) {
        SPDLOG_ERROR("ge::loadCube: sokol not initialized");
        return {};
    }

    const bool hasAstc = textureBackendHasAstc();
    const bool hasEtc2 = textureBackendHasEtc2();

    std::string facePaths[6];
    for (int i = 0; i < 6; ++i) {
        facePaths[i] =
            resolveTexturePath(facePrefix + std::to_string(i), hasAstc, hasEtc2,
                               defaultExists);
        if (facePaths[i].empty()) {
            SPDLOG_ERROR("ge::loadCube: missing face {} for prefix '{}'", i,
                         facePrefix);
            return {};
        }
    }

    // All faces must share the same extension class.
    const bool getex = isGetexPath(facePaths[0]);
    for (int i = 1; i < 6; ++i) {
        if (isGetexPath(facePaths[i]) != getex) {
            SPDLOG_ERROR("ge::loadCube: mixed encodings under '{}'", facePrefix);
            return {};
        }
    }

    if (!getex) {
        // PNG faces — RGBA cube, mip 0 only (matches historical yourworld path).
        constexpr int kFaces = 6;
        std::vector<uint8_t> buf;
        int faceW = 0, faceH = 0;
        for (int i = 0; i < kFaces; ++i) {
            std::string resolved = ge::resource(facePaths[i]);
            SDL_Surface* s = IMG_Load(resolved.c_str());
            if (!s) {
                SPDLOG_ERROR("ge::loadCube: IMG_Load {} failed: {}", facePaths[i],
                             SDL_GetError());
                return {};
            }
            if (s->format != SDL_PIXELFORMAT_RGBA32) {
                SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
                SDL_DestroySurface(s);
                s = c;
                if (!s) return {};
            }
            if (i == 0) {
                faceW = s->w;
                faceH = s->h;
                buf.resize(size_t(kFaces) * faceW * faceH * 4);
            } else if (s->w != faceW || s->h != faceH) {
                SPDLOG_ERROR("ge::loadCube: face size mismatch");
                SDL_DestroySurface(s);
                return {};
            }
            uint8_t* dst = buf.data() + size_t(i) * faceW * faceH * 4;
            const uint8_t* src = static_cast<const uint8_t*>(s->pixels);
            for (int y = 0; y < faceH; ++y) {
                std::memcpy(dst + size_t(y) * faceW * 4,
                            src + size_t(y) * s->pitch, size_t(faceW) * 4);
            }
            SDL_DestroySurface(s);
        }
        sg_image_desc d{};
        d.type = SG_IMAGETYPE_CUBE;
        d.width = faceW;
        d.height = faceH;
        d.num_slices = 6;
        d.num_mipmaps = 1;
        d.pixel_format = SG_PIXELFORMAT_RGBA8;
        d.data.mip_levels[0] = sg_range{buf.data(), buf.size()};
        d.label = "ge.cube";
        GpuTexture out;
        out.image = sg_make_image(&d);
        if (out.image.id == SG_INVALID_ID) return {};
        sg_view_desc vd{};
        vd.texture.image = out.image;
        vd.label = "ge.cube.view";
        out.view = sg_make_view(&vd);
        out.width = faceW;
        out.height = faceH;
        out.type = SG_IMAGETYPE_CUBE;
        out.format = SG_PIXELFORMAT_RGBA8;
        SPDLOG_INFO("ge::loadCube: {}* RGBA {}x{}", facePrefix, faceW, faceH);
        return out;
    }

    // Compressed / PNG-getex faces: decode all six, assemble per-mip face packs.
    GetexDecoded faces[6];
    for (int i = 0; i < 6; ++i) {
        faces[i] = loadGetexFile(facePaths[i]);
        if (!faces[i].ok) return {};
        if (i > 0) {
            if (faces[i].hdr.width != faces[0].hdr.width ||
                faces[i].hdr.height != faces[0].hdr.height ||
                faces[i].hdr.mipCount != faces[0].hdr.mipCount ||
                faces[i].hdr.encoding != faces[0].hdr.encoding) {
                SPDLOG_ERROR("ge::loadCube: face header mismatch");
                return {};
            }
        }
    }

    const auto enc = static_cast<ge::GeTexEncoding>(faces[0].hdr.encoding);
    const int faceW = int(faces[0].hdr.width);
    const int faceH = int(faces[0].hdr.height);
    const int nMips = int(faces[0].hdr.mipCount);

    // Keep ownership of decoded PNG-getex RGBA buffers until sg_make_image returns.
    std::vector<std::vector<uint8_t>> pngRgbaHold;
    std::vector<std::vector<uint8_t>> mipPacks(nMips);

    sg_image_desc d{};
    d.type = SG_IMAGETYPE_CUBE;
    d.width = faceW;
    d.height = faceH;
    d.num_slices = 6;
    d.num_mipmaps = nMips;
    d.label = "ge.cube";

    if (enc == ge::GeTexEncoding::Png) {
        d.pixel_format = SG_PIXELFORMAT_RGBA8;
        // Expand each face to RGBA mips, then pack faces per mip.
        std::vector<sg_range> faceLevels[6];
        std::vector<uint8_t> faceRgba[6];
        for (int f = 0; f < 6; ++f) {
            int w = 0, h = 0;
            if (!expandPngGetex(faces[f], faceRgba[f], faceLevels[f], w, h)) {
                SPDLOG_ERROR("ge::loadCube: PNG expand face {} failed", f);
                return {};
            }
        }
        for (int m = 0; m < nMips; ++m) {
            size_t total = 0;
            for (int f = 0; f < 6; ++f) total += faceLevels[f][m].size;
            mipPacks[m].resize(total);
            size_t off = 0;
            for (int f = 0; f < 6; ++f) {
                std::memcpy(mipPacks[m].data() + off, faceLevels[f][m].ptr,
                            faceLevels[f][m].size);
                off += faceLevels[f][m].size;
            }
            d.data.mip_levels[m] = sg_range{mipPacks[m].data(), mipPacks[m].size()};
        }
    } else {
        d.pixel_format = formatForEncoding(faces[0].hdr.encoding);
        if (!sg_query_pixelformat(d.pixel_format).sample) {
            SPDLOG_ERROR("ge::loadCube: format not sampleable");
            return {};
        }
        for (int m = 0; m < nMips; ++m) {
            size_t total = 0;
            for (int f = 0; f < 6; ++f) total += faces[f].levels[m].size;
            mipPacks[m].resize(total);
            size_t off = 0;
            for (int f = 0; f < 6; ++f) {
                std::memcpy(mipPacks[m].data() + off, faces[f].levels[m].ptr,
                            faces[f].levels[m].size);
                off += faces[f].levels[m].size;
            }
            d.data.mip_levels[m] = sg_range{mipPacks[m].data(), mipPacks[m].size()};
        }
    }

    GpuTexture out;
    out.image = sg_make_image(&d);
    if (out.image.id == SG_INVALID_ID) {
        SPDLOG_ERROR("ge::loadCube: sg_make_image failed");
        return {};
    }
    sg_view_desc vd{};
    vd.texture.image = out.image;
    vd.label = "ge.cube.view";
    out.view = sg_make_view(&vd);
    out.width = faceW;
    out.height = faceH;
    out.numMips = nMips;
    out.type = SG_IMAGETYPE_CUBE;
    out.format = d.pixel_format;
    SPDLOG_INFO("ge::loadCube: {}* {} {}x{} mips={}", facePrefix,
                enc == ge::GeTexEncoding::Astc4x4   ? "ASTC"
                : enc == ge::GeTexEncoding::Etc2Rgba8 ? "ETC2"
                                                      : "PNG",
                faceW, faceH, nMips);
    return out;
}

} // namespace ge
