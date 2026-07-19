// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ge/FontLoader.h>
#include <ge/Linalg.h>
#include <ge/sprite.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ge {

// CPU-side raster output: width * height * 4 bytes of RGBA8 with
// premultiplied alpha. `rgba` is empty on failure.
struct TextPixels {
    std::vector<uint8_t> rgba;
    int width  = 0;
    int height = 0;

    constexpr bool isNull() const { return rgba.empty(); }
};

// Rasterize a UTF-8 string to RGBA8 premultiplied pixels using FreeType.
//
// `font`    — a FontRef obtained from `ge::resolveFont` or constructed directly.
// `sizePt`  — point size (1pt = 1/72 inch at 72 DPI, i.e. 1px per pt).
// `color`   — RGBA in [0, 1]. Alpha is interpreted straight; output is
//             premultiplied: `out_rgb = color.rgb * alpha * glyph_alpha`,
//             `out_a       = color.a * glyph_alpha`.
//
// Single line; no wrapping or kerning. Basic ASCII Latin; behavior for
// codepoints > 127 depends on the font's glyph coverage.
//
// Empty TextPixels on failure with a logged error.
TextPixels rasterizeTextToPixels(const std::string& text,
                                 const FontRef& font,
                                 float sizePt,
                                 la::float4 color);

// Same as rasterizeTextToPixels but opens the face from an in-memory font
// blob (cmdstream MakeText player path — FT_New_Memory_Face).
TextPixels rasterizeTextToPixelsFromMemory(const std::string& text,
                                           const void* fontBytes, size_t fontN,
                                           int faceIndex,
                                           float sizePt,
                                           la::float4 color);

// Rasterize and upload to a `Sprite`. Null Sprite on failure.
// Under cmdstream, registers a MakeText recipe (font bytes + string).
Sprite rasterizeText(const std::string& text,
                     const FontRef& font,
                     float sizePt,
                     la::float4 color);

} // namespace ge
