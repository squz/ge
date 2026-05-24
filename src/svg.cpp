// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/svg.h>

#include <ge/FontLoader.h>

#include <bgfx/bgfx.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>
#include <mutex>

namespace ge {

namespace {

// Register one of the platform's logical font URIs (e.g. "system:sans-serif")
// with lunasvg's font cache as the named family. Best-effort: SVG text
// missing a system default should render unstyled, not abort the rasterize.
// Apps that want a hard failure when a specific family is missing should
// call resolveFont + registerSvgFontFace explicitly and let the throw
// propagate.
void registerPlatformFont(const char* family, bool bold, bool italic, const char* uri) {
    FontRef ref;
    try {
        ref = resolveFont(uri);
    } catch (const std::exception& e) {
        spdlog::warn("ge::rasterizeSvg: {} — SVG <text font-family=\"{}\"> "
                     "may render unstyled", e.what(), family);
        return;
    }
    if (!lunasvg_add_font_face_from_file(family, bold, italic, ref.path.c_str())) {
        spdlog::warn("ge::rasterizeSvg: lunasvg rejected font file {} for family {}",
                     ref.path, family);
    }
}

void ensureDefaultFonts() {
    static std::once_flag flag;
    std::call_once(flag, []{
        registerPlatformFont("sans-serif", false, false, "system:sans-serif");
        registerPlatformFont("sans-serif", true,  false, "system:sans-serif-bold");
        registerPlatformFont("serif",      false, false, "system:serif");
        registerPlatformFont("serif",      true,  false, "system:serif-bold");
        registerPlatformFont("monospace",  false, false, "system:monospace");
        registerPlatformFont("monospace",  true,  false, "system:monospace-bold");
    });
}

// lunasvg returns ARGB32 premultiplied. On little-endian (the only target
// ge supports) that's bytes B,G,R,A per pixel in memory.
// bgfx::TextureFormat::RGBA8 expects R,G,B,A. Swap byte 0 and byte 2 in
// each pixel; keep premultiplication.
SvgPixels bitmapToPixels(const lunasvg::Bitmap& bm) {
    SvgPixels out;
    if (bm.isNull()) return out;

    const int w = bm.width();
    const int h = bm.height();
    const int stride = bm.stride();
    out.width  = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

    const uint8_t* src = bm.data();
    uint8_t*       dst = out.rgba.data();
    for (int y = 0; y < h; ++y) {
        const uint8_t* sRow = src + static_cast<size_t>(y) * stride;
        uint8_t*       dRow = dst + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dRow[x * 4 + 0] = sRow[x * 4 + 2];
            dRow[x * 4 + 1] = sRow[x * 4 + 1];
            dRow[x * 4 + 2] = sRow[x * 4 + 0];
            dRow[x * 4 + 3] = sRow[x * 4 + 3];
        }
    }
    return out;
}

Sprite uploadPixels(const SvgPixels& pixels) {
    if (pixels.isNull()) return Sprite{};
    const bgfx::Memory* mem = bgfx::copy(
        pixels.rgba.data(),
        static_cast<uint32_t>(pixels.rgba.size()));
    Sprite out;
    out.tex = bgfx::createTexture2D(
        static_cast<uint16_t>(pixels.width),
        static_cast<uint16_t>(pixels.height),
        false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_NONE,
        mem);
    out.width  = pixels.width;
    out.height = pixels.height;
    return out;
}

} // namespace

bool registerSvgFontFace(const std::string& family, bool bold, bool italic,
                         const FontRef& ref) {
    if (ref.path.empty()) return false;
    return lunasvg_add_font_face_from_file(family.c_str(), bold, italic, ref.path.c_str());
}

SvgPixels rasterizeSvgToPixels(std::string_view svg, int targetW, int targetH) {
    ensureDefaultFonts();

    auto doc = lunasvg::Document::loadFromData(svg.data(), svg.size());
    if (!doc) {
        spdlog::error("ge::rasterizeSvgToPixels: failed to parse SVG ({} bytes)", svg.size());
        return {};
    }

    auto bm = doc->renderToBitmap(targetW, targetH);
    if (bm.isNull()) {
        spdlog::error("ge::rasterizeSvgToPixels: renderToBitmap returned null ({}x{})", targetW, targetH);
        return {};
    }
    return bitmapToPixels(bm);
}

Sprite rasterizeSvg(std::string_view svg, int targetW, int targetH) {
    return uploadPixels(rasterizeSvgToPixels(svg, targetW, targetH));
}

Sprite renderSvgDocument(const lunasvg::Document& doc, int targetW, int targetH) {
    // lunasvg::Document::renderToBitmap is non-const in some versions; the
    // const_cast is safe because renderToBitmap doesn't observably mutate
    // the document beyond updating an internal layout cache.
    auto bm = const_cast<lunasvg::Document&>(doc).renderToBitmap(targetW, targetH);
    if (bm.isNull()) {
        spdlog::error("ge::renderSvgDocument: renderToBitmap returned null ({}x{})", targetW, targetH);
        return Sprite{};
    }
    return uploadPixels(bitmapToPixels(bm));
}

lunasvg::Element hitTestSvgAt(const lunasvg::Document& doc,
                              float x, float y, float radiusPx) {
    // lunasvg::Document::elementFromPoint is non-const in some versions;
    // same const_cast rationale as renderToBitmap above.
    auto& d = const_cast<lunasvg::Document&>(doc);

    // Centre first — a direct hit wins over any ring sample.
    if (auto e = d.elementFromPoint(x, y)) return e;
    if (radiusPx <= 0.0f) return {};

    // 8-point ring at 45° increments. cos/sin computed inline because
    // C++ <cmath> trig isn't constexpr until C++26; the table form
    // (eight entries × two floats) is overkill for code that only runs
    // on a tap.
    constexpr float kSqrt1_2 = 0.70710678118654752440f;  // 1/√2
    constexpr float kOffsets[8][2] = {
        { 1.0f,      0.0f     },  // E
        { kSqrt1_2,  kSqrt1_2 },  // SE
        { 0.0f,      1.0f     },  // S
        {-kSqrt1_2,  kSqrt1_2 },  // SW
        {-1.0f,      0.0f     },  // W
        {-kSqrt1_2, -kSqrt1_2 },  // NW
        { 0.0f,     -1.0f     },  // N
        { kSqrt1_2, -kSqrt1_2 },  // NE
    };
    for (const auto& o : kOffsets) {
        if (auto e = d.elementFromPoint(x + o[0] * radiusPx,
                                        y + o[1] * radiusPx)) {
            return e;
        }
    }
    return {};
}

} // namespace ge
