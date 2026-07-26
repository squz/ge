// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.3 tierFor / levelCap are pure — no sokol init needed.

#include <doctest.h>
#include <ge/DeviceTier.h>

using ge::DeviceTierInfo;
using ge::TextureTier;
using ge::kFullTierMinRamBytes;
using ge::tierFor;

namespace {
constexpr uint64_t kGiB = 1024ull * 1024 * 1024;
}

TEST_CASE("tierFor: no ASTC is always Legacy, regardless of RAM") {
    CHECK(tierFor(0, /*astc=*/false) == TextureTier::Legacy);
    CHECK(tierFor(4 * kGiB, false) == TextureTier::Legacy);
    CHECK(tierFor(kFullTierMinRamBytes, false) == TextureTier::Legacy);
    CHECK(tierFor(8 * kGiB, false) == TextureTier::Legacy);
}

TEST_CASE("tierFor: ASTC + below the RAM floor is Capped") {
    CHECK(tierFor(0, /*astc=*/true) == TextureTier::Capped);
    CHECK(tierFor(4 * kGiB, true) == TextureTier::Capped);
    CHECK(tierFor(kFullTierMinRamBytes - 1, true) == TextureTier::Capped);
}

TEST_CASE("tierFor: ASTC + at or above the RAM floor is Full") {
    CHECK(tierFor(kFullTierMinRamBytes, /*astc=*/true) == TextureTier::Full);
    CHECK(tierFor(8 * kGiB, true) == TextureTier::Full);
}

TEST_CASE("levelCap: Full loads every level regardless of levelCount") {
    DeviceTierInfo info;
    info.tier = TextureTier::Full;
    for (int levelCount = 1; levelCount <= 4; ++levelCount) {
        CHECK(info.levelCap(levelCount) == -1);
    }
}

TEST_CASE("levelCap: Capped drops the leaf, clamped at 0") {
    DeviceTierInfo info;
    info.tier = TextureTier::Capped;
    CHECK(info.levelCap(1) == 0);
    CHECK(info.levelCap(2) == 0);
    CHECK(info.levelCap(3) == 1);
    CHECK(info.levelCap(4) == 2);
}

TEST_CASE("levelCap: Legacy is always 0 regardless of levelCount") {
    DeviceTierInfo info;
    info.tier = TextureTier::Legacy;
    for (int levelCount = 1; levelCount <= 4; ++levelCount) {
        CHECK(info.levelCap(levelCount) == 0);
    }
}
