// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T167 Pure selection tests for the public texture path resolver.
// GPU upload is not exercised here (needs live sokol); cook is covered by
// ge-texenc smoke + Module.mk patterns.

#include <ge/texture.h>

#include <doctest.h>

#include <algorithm>
#include <string>
#include <unordered_set>

using ge::resolveTexturePath;
using ge::textureCandidatePaths;

TEST_CASE("textureCandidatePaths prefers ASTC then ETC2 then PNG (🎯T167)") {
    auto c = textureCandidatePaths("data/tex/globe", /*hasAstc=*/true,
                                   /*hasEtc2=*/true);
    REQUIRE(c.size() >= 3);
    CHECK(c[0] == "data/tex/globe.astc.getex");
    CHECK(c[1] == "data/tex/globe.etc2.getex");
    CHECK(c[2] == "data/tex/globe.png.getex");
    // Raw PNG still a fallback
    CHECK(std::find(c.begin(), c.end(), "data/tex/globe.png") != c.end());
}

TEST_CASE("textureCandidatePaths without ASTC prefers ETC2 (sim/web-ish)") {
    auto c = textureCandidatePaths("assets/icon", false, true);
    REQUIRE_FALSE(c.empty());
    CHECK(c[0] == "assets/icon.etc2.getex");
    CHECK(c[1] == "assets/icon.png.getex");
}

TEST_CASE("textureCandidatePaths concrete .png still lists compressed siblings") {
    auto c = textureCandidatePaths("data/face_0.png", true, false);
    REQUIRE_FALSE(c.empty());
    CHECK(c[0] == "data/face_0.png");
    CHECK(std::find(c.begin(), c.end(), "data/face_0.astc.getex") != c.end());
}

TEST_CASE("resolveTexturePath picks first existing candidate") {
    std::unordered_set<std::string> have = {
        "data/tex/globe.etc2.getex",
        "data/tex/globe.png",
    };
    auto exists = [&](const std::string& p) { return have.count(p) > 0; };
    // Has ASTC capability but file missing → ETC2
    auto p = resolveTexturePath("data/tex/globe", true, true, exists);
    CHECK(p == "data/tex/globe.etc2.getex");
    // No compressed → PNG
    have = {"data/tex/globe.png"};
    p = resolveTexturePath("data/tex/globe", true, true, exists);
    CHECK(p == "data/tex/globe.png");
    // Nothing
    have.clear();
    p = resolveTexturePath("data/tex/globe", true, true, exists);
    CHECK(p.empty());
}

TEST_CASE("resolveTexturePath concrete astc path when present") {
    std::unordered_set<std::string> have = {"data/cubemap/face_0.astc.getex"};
    auto exists = [&](const std::string& p) { return have.count(p) > 0; };
    auto p = resolveTexturePath("data/cubemap/face_0.astc.getex", true, true,
                                exists);
    CHECK(p == "data/cubemap/face_0.astc.getex");
}
