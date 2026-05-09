// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>

namespace ge {

// Load a PNG/JPEG/BMP/etc. image from a path and upload it as a bgfx texture.
//
// Path is resolved via ge::resource so iOS bundle and Android APK assets work
// uniformly. The image is converted to RGBA8 with premultiplied alpha before
// upload (SDL_image does not premultiply by default; ge's render pipeline
// expects premultiplied). The texture is RGBA8 with no mips.
//
// On success, returns a valid bgfx::TextureHandle. Caller owns the handle and
// must destroy it with bgfx::destroy when done.
//
// On failure (file not found, unsupported format, OOM), returns
// BGFX_INVALID_HANDLE and logs the error via spdlog::error.
//
// outW / outH are optional — if non-null, filled with the image dimensions in
// pixels.
//
// bgfx must be initialized before calling this (via ge::run or BgfxContext).
bgfx::TextureHandle loadTexture2D(const std::string& path,
                                  int* outW = nullptr,
                                  int* outH = nullptr);

} // namespace ge
