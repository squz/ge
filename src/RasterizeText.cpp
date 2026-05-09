// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/RasterizeText.h>

#include <bgfx/bgfx.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ge {

namespace {

// FreeType library handle. Initialized once per process. Not thread-safe to
// initialize concurrently, but ge's engine has a single rasterize path.
FT_Library& ftLibrary() {
    static FT_Library lib = nullptr;
    if (lib == nullptr) {
        FT_Error err = FT_Init_FreeType(&lib);
        if (err != 0) {
            spdlog::error("ge::rasterizeText: FT_Init_FreeType failed (error {})", err);
            lib = nullptr;
        }
    }
    return lib;
}

} // namespace

TextPixels rasterizeTextToPixels(const std::string& text,
                                 const FontRef& font,
                                 float sizePt,
                                 la::float4 color) {
    TextPixels out;

    if (text.empty()) return out;
    if (font.path.empty()) {
        spdlog::error("ge::rasterizeText: FontRef has empty path");
        return out;
    }

    FT_Library lib = ftLibrary();
    if (lib == nullptr) return out;

    FT_Face face = nullptr;
    FT_Error err = FT_New_Face(lib, font.path.c_str(),
                               static_cast<FT_Long>(font.faceIndex), &face);
    if (err != 0) {
        spdlog::error("ge::rasterizeText: FT_New_Face failed for '{}' face {} (error {})",
                      font.path, font.faceIndex, err);
        return out;
    }

    // Set size: sizePt points at 72 DPI → 1 pixel per point.
    const FT_F26Dot6 sizeFixed = static_cast<FT_F26Dot6>(sizePt * 64.0f + 0.5f);
    err = FT_Set_Char_Size(face, 0, sizeFixed, 72, 72);
    if (err != 0) {
        spdlog::error("ge::rasterizeText: FT_Set_Char_Size failed (error {})", err);
        FT_Done_Face(face);
        return out;
    }

    // --- Pass 1: measure total width and ascent/descent bounds ---

    int totalAdvance = 0;   // sum of glyph horizontal advances in pixels
    int maxAscent    = 0;   // max bearing Y (pixels above baseline)
    int maxDescent   = 0;   // max (bearingY - rows), i.e. pixels below baseline

    for (unsigned char ch : text) {
        FT_UInt glyphIdx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        err = FT_Load_Glyph(face, glyphIdx, FT_LOAD_RENDER);
        if (err != 0) continue;

        FT_GlyphSlot slot = face->glyph;
        int bearingY = static_cast<int>(slot->bitmap_top);
        int descent  = static_cast<int>(slot->bitmap.rows) - bearingY;
        maxAscent  = std::max(maxAscent,  bearingY);
        maxDescent = std::max(maxDescent, descent);
        totalAdvance += static_cast<int>(slot->advance.x >> 6);
    }

    // For the last glyph, the actual visible width may be less than the
    // advance. Account for the last glyph's bearing+width vs advance by
    // also tracking the right edge of the last rendered glyph.
    // We re-measure in pass 2, but for the canvas we use totalAdvance.
    if (totalAdvance <= 0 || maxAscent + maxDescent <= 0) {
        spdlog::error("ge::rasterizeText: measured empty glyph metrics for '{}'", text);
        FT_Done_Face(face);
        return out;
    }

    const int canvasW = totalAdvance;
    const int canvasH = maxAscent + maxDescent;

    out.rgba.resize(static_cast<size_t>(canvasW) * static_cast<size_t>(canvasH) * 4, 0);
    out.width  = canvasW;
    out.height = canvasH;

    // Pre-clamp color to [0, 1] and extract components.
    const float cr = std::max(0.0f, std::min(1.0f, color.x));
    const float cg = std::max(0.0f, std::min(1.0f, color.y));
    const float cb = std::max(0.0f, std::min(1.0f, color.z));
    const float ca = std::max(0.0f, std::min(1.0f, color.w));

    // --- Pass 2: rasterize each glyph into the canvas ---

    int penX = 0; // pixel cursor

    for (unsigned char ch : text) {
        FT_UInt glyphIdx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        err = FT_Load_Glyph(face, glyphIdx, FT_LOAD_RENDER);
        if (err != 0) {
            // Skip unrenderable glyphs; advance cursor by a fixed width.
            penX += static_cast<int>(sizeFixed >> 6) / 2;
            continue;
        }

        FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bm = slot->bitmap;

        // FreeType gives 8-bit grayscale (FT_PIXEL_MODE_GRAY, 256 levels).
        // blit_x/y: top-left corner of this glyph in the canvas.
        const int blitX = penX + static_cast<int>(slot->bitmap_left);
        const int blitY = maxAscent - static_cast<int>(slot->bitmap_top);

        for (int row = 0; row < static_cast<int>(bm.rows); ++row) {
            const int dstY = blitY + row;
            if (dstY < 0 || dstY >= canvasH) continue;

            for (int col = 0; col < static_cast<int>(bm.width); ++col) {
                const int dstX = blitX + col;
                if (dstX < 0 || dstX >= canvasW) continue;

                // FreeType may use negative pitch (top-down vs bottom-up).
                const uint8_t* srcRow = (bm.pitch >= 0)
                    ? bm.buffer + static_cast<size_t>(row) * static_cast<size_t>(bm.pitch)
                    : bm.buffer + static_cast<size_t>(bm.rows - 1 - row) * static_cast<size_t>(-bm.pitch);

                const float alpha = static_cast<float>(srcRow[col]) / 255.0f;
                // Premultiplied RGBA8: out = color * alpha_glyph.
                const float combinedAlpha = ca * alpha;
                uint8_t* dst = out.rgba.data() +
                    (static_cast<size_t>(dstY) * static_cast<size_t>(canvasW) + static_cast<size_t>(dstX)) * 4;
                dst[0] = static_cast<uint8_t>(cr * combinedAlpha * 255.0f + 0.5f);
                dst[1] = static_cast<uint8_t>(cg * combinedAlpha * 255.0f + 0.5f);
                dst[2] = static_cast<uint8_t>(cb * combinedAlpha * 255.0f + 0.5f);
                dst[3] = static_cast<uint8_t>(combinedAlpha * 255.0f + 0.5f);
            }
        }

        penX += static_cast<int>(slot->advance.x >> 6);
    }

    FT_Done_Face(face);
    return out;
}

bgfx::TextureHandle rasterizeText(const std::string& text,
                                  const FontRef& font,
                                  float sizePt,
                                  la::float4 color) {
    auto pixels = rasterizeTextToPixels(text, font, sizePt, color);
    if (pixels.isNull()) {
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem = bgfx::copy(
        pixels.rgba.data(),
        static_cast<uint32_t>(pixels.rgba.size()));
    return bgfx::createTexture2D(
        static_cast<uint16_t>(pixels.width),
        static_cast<uint16_t>(pixels.height),
        false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_NONE,
        mem);
}

} // namespace ge
