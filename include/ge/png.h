// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ge/sprite.h>

#include <cstdint>
#include <string>

struct SDL_Surface;

namespace ge {

// Load a PNG / JPEG / BMP / etc. image from a path and upload it as a
// `Sprite`. Path is resolved via `ge::resource` so iOS bundle and Android
// APK assets work uniformly.
//
// The image is converted to RGBA8 with premultiplied alpha before upload
// (SDL_image does not premultiply by default; ge's render pipeline
// expects premultiplied).
//
// On failure (file missing, unsupported format, OOM), returns a null
// `Sprite` and logs the error via `spdlog::error`. `bgfx` must be
// initialized before calling.
Sprite loadImage(const std::string& path);

// Same as `loadImage` but starts from an in-memory SDL_Surface — useful
// when the surface comes from a non-file source (an SDL_Surface
// produced by SDL_image's IO helpers, an SVG rasterizer's output, an
// embedded asset decoded at runtime, image data fetched over the wire).
//
// Converts to RGBA8 if needed, premultiplies alpha in-place, and
// uploads as a bgfx texture. *Takes ownership* of `surface` —
// destroys it before returning, regardless of success.
//
// `nullptr` input returns a null `Sprite` without crashing. Other
// failure modes log via `spdlog::error` and return null. `bgfx` must
// be initialized before calling.
Sprite imageFromSurface(SDL_Surface* surface);

// 🎯T124 Write RGBA8 pixels (top-down, tightly packed, `w*h*4` bytes) to a PNG
// file at `path`. Returns false on bad args or write failure (logged). Used by
// the headless render-state path; needs no GPU context.
bool writePng(const std::string& path, const std::uint8_t* rgba, int w, int h);

} // namespace ge
