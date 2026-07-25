// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge-texenc — cook PNG → GPU-ready getex / single-level ASTC/PNG.
//
// Part of the consumer asset-cook pipeline (🎯T167 cook half). Format is
// chosen by the output extension (see ge::textureToFile):
//   .astc.getex  — mipmapped ASTC 4×4 (iOS device / modern Android)
//   .etc2.getex  — mipmapped ETC2 RGBA8 (iOS Simulator baseline)
//   .png.getex   — mipmapped PNG blobs (universal / web fallback pack)
//   .astc / .png — single-level variants
//
// Usage:
//   ge-texenc <input.png> <output.ext>
//
// Make integration: Module.mk pattern rules (%.astc.getex: %.png etc.).

#include <ge/TextureEncoder.h>

#include <spdlog/spdlog.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <exception>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <input.png> <output.getex|astc|png>\n", argv[0]);
        return 2;
    }
    const char* inPath = argv[1];
    const char* outPath = argv[2];

    int w = 0, h = 0, ch = 0;
    unsigned char* img = stbi_load(inPath, &w, &h, &ch, 4);
    if (!img) {
        SPDLOG_ERROR("stbi_load failed: {} ({})", inPath,
                     stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return 1;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(img);
        SPDLOG_ERROR("invalid dimensions {}x{} for {}", w, h, inPath);
        return 1;
    }

    try {
        ge::textureToFile(outPath, img, w, h);
        SPDLOG_INFO("ge-texenc: {} ({}x{}) → {}", inPath, w, h, outPath);
    } catch (const std::exception& e) {
        stbi_image_free(img);
        SPDLOG_ERROR("ge-texenc failed: {}", e.what());
        return 1;
    }
    stbi_image_free(img);
    return 0;
}
