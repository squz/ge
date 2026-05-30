// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/text.h>

#include "sokol_gfx.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ge {

namespace {

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

    const FT_F26Dot6 sizeFixed = static_cast<FT_F26Dot6>(sizePt * 64.0f + 0.5f);
    err = FT_Set_Char_Size(face, 0, sizeFixed, 72, 72);
    if (err != 0) {
        spdlog::error("ge::rasterizeText: FT_Set_Char_Size failed (error {})", err);
        FT_Done_Face(face);
        return out;
    }

    int totalAdvance = 0;
    int maxAscent    = 0;
    int maxDescent   = 0;

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

    const float cr = std::max(0.0f, std::min(1.0f, color.x));
    const float cg = std::max(0.0f, std::min(1.0f, color.y));
    const float cb = std::max(0.0f, std::min(1.0f, color.z));
    const float ca = std::max(0.0f, std::min(1.0f, color.w));

    int penX = 0;

    for (unsigned char ch : text) {
        FT_UInt glyphIdx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        err = FT_Load_Glyph(face, glyphIdx, FT_LOAD_RENDER);
        if (err != 0) {
            penX += static_cast<int>(sizeFixed >> 6) / 2;
            continue;
        }

        FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bm = slot->bitmap;

        const int blitX = penX + static_cast<int>(slot->bitmap_left);
        const int blitY = maxAscent - static_cast<int>(slot->bitmap_top);

        for (int row = 0; row < static_cast<int>(bm.rows); ++row) {
            const int dstY = blitY + row;
            if (dstY < 0 || dstY >= canvasH) continue;

            for (int col = 0; col < static_cast<int>(bm.width); ++col) {
                const int dstX = blitX + col;
                if (dstX < 0 || dstX >= canvasW) continue;

                const uint8_t* srcRow = (bm.pitch >= 0)
                    ? bm.buffer + static_cast<size_t>(row) * static_cast<size_t>(bm.pitch)
                    : bm.buffer + static_cast<size_t>(bm.rows - 1 - row) * static_cast<size_t>(-bm.pitch);

                const float alpha         = static_cast<float>(srcRow[col]) / 255.0f;
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

Sprite rasterizeText(const std::string& text,
                     const FontRef& font,
                     float sizePt,
                     la::float4 color) {
    auto pixels = rasterizeTextToPixels(text, font, sizePt, color);
    if (pixels.isNull()) return Sprite{};

    sg_image_desc desc{};
    desc.width        = pixels.width;
    desc.height       = pixels.height;
    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    desc.data.mip_levels[0] = (sg_range){
        .ptr  = pixels.rgba.data(),
        .size = pixels.rgba.size(),
    };
    desc.label = "ge.text.sprite";

    Sprite out;
    out.tex = sg_make_image(&desc);

    sg_view_desc vd{};
    vd.texture.image = out.tex;
    vd.label = "ge.text.sprite.view";
    out.view = sg_make_view(&vd);

    out.width  = pixels.width;
    out.height = pixels.height;
    return out;
}

} // namespace ge
