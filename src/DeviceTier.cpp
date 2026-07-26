// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.3 Platform-agnostic device-tier decision. Physical RAM comes from
// platformPhysRamBytes(), defined per platform in DeviceTier_apple.mm
// (sysctlbyname hw.memsize), DeviceTier_android.cpp (sysinfo), and
// DeviceTier_stub.cpp (web / other, returns 0).

#include <ge/DeviceTier.h>

#include <ge/appchannel.h>
#include <ge/texture.h>

#include <spdlog/spdlog.h>

#include "sokol_gfx.h"

namespace ge {

// Declared (not defined) here with external linkage so each platform's .mm/
// .cpp can provide the one definition libge links in for that platform.
uint64_t platformPhysRamBytes();

namespace {

const char* tierName(TextureTier tier) {
    switch (tier) {
        case TextureTier::Full:   return "full";
        case TextureTier::Capped: return "capped";
        case TextureTier::Legacy: return "legacy";
    }
    return "legacy";
}

} // namespace

TextureTier tierFor(uint64_t ramBytes, bool astc) {
    if (!astc) return TextureTier::Legacy;
    return ramBytes >= kFullTierMinRamBytes ? TextureTier::Full : TextureTier::Capped;
}

int DeviceTierInfo::levelCap(int levelCount) const {
    switch (tier) {
        case TextureTier::Full:   return -1;
        case TextureTier::Capped: return levelCount - 2 > 0 ? levelCount - 2 : 0;
        case TextureTier::Legacy: return 0;
    }
    return 0;
}

DeviceTierInfo queryDeviceTier() {
    DeviceTierInfo info;
    if (!sg_isvalid()) {
        SPDLOG_WARN("ge::queryDeviceTier: sokol not initialized; defaulting to Legacy");
        return info; // default-constructed: Legacy, 0 RAM, no ASTC/ETC2.
    }

    info.physRamBytes = platformPhysRamBytes();
    info.astc = textureBackendHasAstc();
    info.etc2 = textureBackendHasEtc2();
    info.tier = tierFor(info.physRamBytes, info.astc);

    const double ramGiB = double(info.physRamBytes) / (1024.0 * 1024.0 * 1024.0);
    SPDLOG_INFO("ge::queryDeviceTier: ram={:.2f}GiB astc={} etc2={} tier={}",
                ramGiB, info.astc, info.etc2, tierName(info.tier));

    ge::appchannel::push("device_tier", nlohmann::json{
        {"ram_gib", ramGiB},
        {"astc",    info.astc},
        {"etc2",    info.etc2},
        {"tier",    tierName(info.tier)},
    });

    return info;
}

} // namespace ge
