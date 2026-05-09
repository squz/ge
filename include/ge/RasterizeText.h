// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ge/FontLoader.h>
#include <ge/Linalg.h>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ge {

// CPU-side raster output: width * height * 4 bytes of RGBA8 with
// premultiplied alpha. `rgba` is empty on failure. Errors are logged via
// spdlog at the point of failure.
struct TextPixels {
    std::vector<uint8_t> rgba;
    int width  = 0;
    int height = 0;

    bool isNull() const { return rgba.empty(); }
};

// Rasterize a UTF-8 string to RGBA8 premultiplied pixels using FreeType.
//
// `font`    — a FontRef obtained from ge::resolveFont() or constructed directly.
// `sizePt`  — point size (1pt = 1/72 inch at 72 DPI, i.e. 1px per pt).
// `color`   — RGBA color in [0, 1]. Alpha is interpreted straight; the output
//             pixels are premultiplied: out_rgb = color.rgb * alpha * glyph_alpha,
//             out_a = color.a * glyph_alpha.
//
// Single line only (no wrapping). Supports basic ASCII Latin; behaviour for
// codepoints > 127 depends on the font's glyph coverage.
//
// Returns {empty, 0, 0} on failure with spdlog::error.
TextPixels rasterizeTextToPixels(const std::string& text,
                                 const FontRef& font,
                                 float sizePt,
                                 la::float4 color);

// Rasterize and upload to a bgfx texture in one call. Returns
// `BGFX_INVALID_HANDLE` on failure. The texture is RGBA8 with premultiplied
// alpha and no mips. Caller owns the returned handle and must destroy it.
//
// bgfx must be initialized before calling this (via ge::run or BgfxContext).
bgfx::TextureHandle rasterizeText(const std::string& text,
                                  const FontRef& font,
                                  float sizePt,
                                  la::float4 color);

} // namespace ge
