// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/png.h>

#include <ge/Resource.h>

#include "sokol_gfx.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_image/SDL_image.h>
#include <spdlog/spdlog.h>

#include <cstdint>

namespace ge {

namespace {

// Premultiply alpha in-place on an RGBA8 surface. SDL_image does not
// premultiply by default; ge's render pipeline (premultiplied-alpha
// blend) requires it. Caller has already ensured `surface->format ==
// SDL_PIXELFORMAT_RGBA32`.
void premultiplyRgba8InPlace(SDL_Surface* surface) {
    const int w = surface->w;
    const int h = surface->h;
    const int pitch = surface->pitch;
    uint8_t* pixels = static_cast<uint8_t*>(surface->pixels);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = pixels + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < w; ++x) {
            uint8_t* p = row + x * 4;
            const uint32_t a = p[3];
            p[0] = static_cast<uint8_t>((p[0] * a + 127u) / 255u);
            p[1] = static_cast<uint8_t>((p[1] * a + 127u) / 255u);
            p[2] = static_cast<uint8_t>((p[2] * a + 127u) / 255u);
        }
    }
}

} // namespace

Sprite imageFromSurface(SDL_Surface* surface) {
    if (surface == nullptr) {
        return Sprite{};
    }

    SDL_Surface* rgba = surface;
    bool ownedRgba = false;
    if (surface->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surface);
        if (rgba == nullptr) {
            spdlog::error("ge::imageFromSurface: SDL_ConvertSurface failed ({})",
                          SDL_GetError());
            return Sprite{};
        }
        ownedRgba = true;
    }

    premultiplyRgba8InPlace(rgba);

    const int w = rgba->w;
    const int h = rgba->h;
    const size_t dataSize = static_cast<size_t>(w) *
                            static_cast<size_t>(h) * 4u;

    sg_image_desc desc{};
    desc.width        = w;
    desc.height       = h;
    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    desc.data.mip_levels[0] = (sg_range){
        .ptr  = rgba->pixels,
        .size = dataSize,
    };
    desc.label = "ge.image.sprite";

    Sprite out;
    out.tex = sg_make_image(&desc);

    sg_view_desc vd{};
    vd.texture.image = out.tex;
    vd.label = "ge.image.sprite.view";
    out.view = sg_make_view(&vd);

    // sg_make_image consumes the initial-data bytes synchronously, so the
    // surface is safe to free now.
    if (ownedRgba) {
        SDL_DestroySurface(rgba);
    } else {
        SDL_DestroySurface(surface);
    }

    out.width  = w;
    out.height = h;
    return out;
}

Sprite loadImage(const std::string& path) {
    std::string resolved = ge::resource(path);

    SDL_IOStream* io = SDL_IOFromFile(resolved.c_str(), "rb");
    if (io == nullptr) {
        spdlog::error("ge::loadImage: cannot open \"{}\" ({})", path, SDL_GetError());
        return Sprite{};
    }

    SDL_Surface* raw = IMG_Load_IO(io, true);
    if (raw == nullptr) {
        spdlog::error("ge::loadImage: IMG_Load_IO failed for \"{}\" ({})", path, SDL_GetError());
        return Sprite{};
    }

    return imageFromSurface(raw);
}

} // namespace ge
