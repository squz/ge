// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/text.h>

#ifndef GE_PLAYER_NO_SOKOL
#include <ge/CmdStream.h>
#include "sokol_gfx.h"
#endif
#include <fstream>
#include <mutex>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ge {

namespace {

// ── 🎯T176 FreeType threading (contract per freetype.h, FT_Library /
// FT_Face docs, FreeType >= 2.5.6) ─────────────────────────────────────
//
//  - ONE FT_Library is shared by every thread. The library needs a lock
//    ONLY around face creation/destruction (FT_New_*_Face / FT_Done_Face)
//    — that is g_ftFaceMu below.
//  - FT_Load_Glyph and siblings are documented thread-safe WITHOUT any
//    lock, provided a given FT_Face is used by one thread at a time. We
//    create a face per rasterize call, so glyph rendering — the expensive
//    part — runs fully in parallel across threads.
//  - An FT_Face can never be a shared immutable artifact: it is a mutable
//    scratchpad by design (the glyph slot is reused per FT_Load_Glyph;
//    size + hinter/interpreter state live in it). The immutable, shared
//    artifact is the FONT BYTES: FT_New_Memory_Face does not copy its
//    buffer ("you must not deallocate the memory before FT_Done_Face"),
//    so every face — on any thread — sits over one write-once entry of
//    cachedFontBytes. Table parsing is lazy per face, so per-call face
//    creation costs microseconds.

std::once_flag g_ftInitOnce;
FT_Library     g_ftLib = nullptr;
std::mutex     g_ftFaceMu;  // guards FT_New_*_Face / FT_Done_Face only

FT_Library ftLibrary() {
    std::call_once(g_ftInitOnce, [] {
        FT_Error err = FT_Init_FreeType(&g_ftLib);
        if (err != 0) {
            spdlog::error("ge::rasterizeText: FT_Init_FreeType failed (error {})", err);
            g_ftLib = nullptr;
        }
    });
    return g_ftLib;
}

// Open a face over an in-memory font (no copy — the bytes must outlive
// the face). Returns nullptr on failure, with the error logged.
FT_Face newMemoryFace(const void* bytes, size_t n, int faceIndex) {
    FT_Library lib = ftLibrary();
    if (lib == nullptr || bytes == nullptr || n == 0) return nullptr;
    std::lock_guard<std::mutex> lk(g_ftFaceMu);
    FT_Face face = nullptr;
    FT_Error err = FT_New_Memory_Face(
        lib, static_cast<const FT_Byte*>(bytes), static_cast<FT_Long>(n),
        static_cast<FT_Long>(faceIndex), &face);
    if (err != 0) {
        spdlog::error("ge::rasterizeText: FT_New_Memory_Face failed (error {})", err);
        return nullptr;
    }
    return face;
}

void doneFace(FT_Face face) {
    if (!face) return;
    std::lock_guard<std::mutex> lk(g_ftFaceMu);
    FT_Done_Face(face);
}

} // namespace

namespace {

TextPixels rasterizeTextFace(const std::string& text,
                             FT_Face face,
                             float sizePt,
                             la::float4 color) {
    TextPixels out;
    if (!face || text.empty()) return out;

    const FT_F26Dot6 sizeFixed = static_cast<FT_F26Dot6>(sizePt * 64.0f + 0.5f);
    FT_Error err = FT_Set_Char_Size(face, 0, sizeFixed, 72, 72);
    if (err != 0) {
        spdlog::error("ge::rasterizeText: FT_Set_Char_Size failed (error {})", err);
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
    return out;
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

// 🎯T169/T176: one write-once copy of each font file, shared by every
// face on every thread. Only the map access locks: entries are never
// erased and never mutated after insert, and unordered_map references
// survive rehash — so the returned reference is safe to read unlocked
// for the life of the process (FT_New_Memory_Face requires exactly
// that: the buffer must outlive its faces).
std::mutex g_fontBytesMu;
const std::vector<uint8_t>& cachedFontBytes(const std::string& path) {
    static std::unordered_map<std::string, std::vector<uint8_t>> cache;
    std::lock_guard<std::mutex> lk(g_fontBytesMu);
    auto it = cache.find(path);
    if (it == cache.end()) it = cache.emplace(path, readFileBytes(path)).first;
    return it->second;
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

    // T176: faces open over the shared write-once byte cache (one disk
    // read per font per process), via the mutexed face create/destroy.
    const auto& bytes = cachedFontBytes(font.path);
    if (bytes.empty()) {
        spdlog::error("ge::rasterizeText: cannot read font '{}'", font.path);
        return out;
    }
    FT_Face face = newMemoryFace(bytes.data(), bytes.size(), font.faceIndex);
    if (!face) {
        spdlog::error("ge::rasterizeText: face open failed for '{}' face {}",
                      font.path, font.faceIndex);
        return out;
    }
    out = rasterizeTextFace(text, face, sizePt, color);
    doneFace(face);
    return out;
}

TextPixels rasterizeTextToPixelsFromMemory(const std::string& text,
                                           const void* fontBytes, size_t fontN,
                                           int faceIndex,
                                           float sizePt,
                                           la::float4 color) {
    TextPixels out;
    if (text.empty() || !fontBytes || fontN == 0) return out;

    FT_Face face = newMemoryFace(fontBytes, fontN, faceIndex);
    if (!face) return out;
    out = rasterizeTextFace(text, face, sizePt, color);
    doneFace(face);
    return out;
}

Sprite rasterizeText(const std::string& text,
                     const FontRef& font,
                     float sizePt,
                     la::float4 color) {
#ifdef GE_PLAYER_NO_SOKOL
    (void)text; (void)font; (void)sizePt; (void)color;
    return Sprite{};
#else
    auto pixels = rasterizeTextToPixels(text, font, sizePt, color);
    if (pixels.isNull()) return Sprite{};

    // T176: thin composition — CPU pixels (any thread) + the promoted
    // pixels->Sprite upload (game thread only).
    Sprite out = spriteFromRgba(pixels.width, pixels.height,
                                pixels.rgba.data(), "ge.text.sprite");

    // 🎯T128.7: ship font + string recipe (not RGBA) when streaming.
    // 🎯T169: recipes are stream-only; skip entirely (including the font
    // file read) in direct mode, and cache font bytes per path — reading
    // and copying a ~2.3 MB TTF per rasterized string leaked a frame's
    // worth of memory for every HUD update.
    if (out.tex.id != SG_INVALID_ID && cmdstream::imageRecipesEnabled()) {
        const auto& fontBytes = cachedFontBytes(font.path);
        if (!fontBytes.empty()) {
            cmdstream::registerImageText(
                out.tex.id, text,
                fontBytes.data(), fontBytes.size(), font.faceIndex,
                sizePt, color.x, color.y, color.z, color.w,
                static_cast<uint16_t>(out.width),
                static_cast<uint16_t>(out.height));
        } else {
            cmdstream::registerImagePixels(
                out.tex.id,
                static_cast<uint16_t>(out.width),
                static_cast<uint16_t>(out.height),
                pixels.rgba.data(), pixels.rgba.size());
        }
    }
    return out;
#endif
}

} // namespace ge
