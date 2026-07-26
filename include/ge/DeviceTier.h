// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.3 Device tier: how deep a tile pyramid this device should load.
// Games call queryDeviceTier() once after sokol setup and pass
// tier.levelCap(pack levels) to TilePyramidOptions — no game-side
// branching on RAM or GL extensions.

#pragma once

#include <cstdint>

namespace ge {

enum class TextureTier : uint8_t {
    Full,   // >= kFullTierMinRamBytes physical RAM, ASTC sampleable:
            // load every pyramid level.
    Capped, // ASTC sampleable, low RAM: skip the leaf level.
    Legacy, // no ASTC: tile packs unsupported; use legacy textures.
};

struct DeviceTierInfo {
    TextureTier tier         = TextureTier::Legacy;
    uint64_t    physRamBytes = 0;
    bool        astc         = false;
    bool        etc2         = false;

    // TilePyramidOptions.levelCap for a pack with `levelCount` levels:
    // Full → -1 (all), Capped → drop the leaf, Legacy → 0 (caller should
    // not open packs at all; provided for defensive completeness).
    int levelCap(int levelCount) const;
};

// Nominal "6 GB device" threshold. Devices under-report physical RAM
// (carve-outs), so the boundary sits below 6 GiB.
constexpr uint64_t kFullTierMinRamBytes = 5ull * 1024 * 1024 * 1024 + 512ull * 1024 * 1024;

// Pure decision rule, exposed for unit testing without a sokol context:
// !astc → Legacy; ramBytes >= kFullTierMinRamBytes → Full; else Capped.
// queryDeviceTier() is this plus the platform RAM query, the live sokol
// ASTC/ETC2 check, logging, and app-channel reporting.
TextureTier tierFor(uint64_t ramBytes, bool astc);

// Queries platform RAM + live sokol backend format support, logs the
// decision once, and reports it on the app channel when connected.
// Requires an initialized sokol context (else returns Legacy + logs).
DeviceTierInfo queryDeviceTier();

} // namespace ge
