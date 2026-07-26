// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge-texpack — cook a cube-sphere tile-pack (.getp) from a JSON config.
// Part of the T168.1 tile-pyramid pipeline (cook half); the runtime loader
// is ge::TilePyramid (TilePack.h).
//
// Usage:
//   ge-texpack <config.json>
//
// Config schema (relative "output" / plane "input.path" resolve against the
// config file's directory):
//
//   {
//     "output": "world.getp",
//     "levelCapOverride": null,          // or an int: cook only levels <= N
//     "planes": [
//       {
//         "name": "rgb", "encoding": "astc4x4",
//         "tileSize": 1280, "gutter": 4, "mips": 5, "levels": 4,
//         "input": {
//           "path": "world.noice.3x86400x43200.bin.gz",
//           "width": 86400, "height": 43200, "channels": 3,
//           "filter": "linear"
//         }
//       },
//       {
//         "name": "id", "encoding": "r8",
//         "tileSize": 1280, "gutter": 4, "mips": 1, "levels": 2,
//         "input": { "path": "id.86400x43200.bin.gz",
//                    "width": 86400, "height": 43200, "channels": 1,
//                    "filter": "nearest" }
//       },
//       {
//         "name": "sdf", "encoding": "astc4x4",
//         "tileSize": 1280, "gutter": 4, "mips": 3, "levels": 3,
//         "input": { "path": "sdf.png", "filter": "linear" }
//       }
//     ]
//   }
//
// Input kinds (by "input.path" extension):
//   .bin / .bin.gz — raw pixels, row-major, "width"/"height"/"channels"
//                    required (gunzip via `zcat` subprocess — see
//                    TilePackWriter.cpp's readMaybeGz).
//   .png           — decoded via stb_image.
// All inputs are equirectangular: row 0 = north (lat +90 -> -90 top to
// bottom), x = lon -180 -> +180 left to right. Direction convention matches
// ge::dirForLonLat (lon 0 at +X, Z up): u_eq = (lon+pi)/(2*pi),
// v_eq = (pi/2-lat)/pi.
//
// "eac_r11" encoding is accepted by the schema but rejected at cook time —
// reserved for a future SDF-plane path, not implemented in v1.

#include "TilePackWriter.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
        return 2;
    }
    const char* configPath = argv[1];

    std::ifstream in(configPath);
    if (!in) {
        SPDLOG_ERROR("ge-texpack: cannot open config {}", configPath);
        return 1;
    }
    nlohmann::json config;
    try {
        in >> config;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("ge-texpack: malformed JSON in {}: {}", configPath, e.what());
        return 1;
    }

    std::string baseDir = std::filesystem::path(configPath).parent_path().string();

    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    bool ok = ge::cookTilePack(config, err, baseDir);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!ok) {
        SPDLOG_ERROR("ge-texpack: cook failed: {}", err);
        return 1;
    }
    SPDLOG_INFO("ge-texpack: cooked {} in {:.1f} ms", configPath, ms);
    return 0;
}
