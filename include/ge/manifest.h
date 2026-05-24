// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge/manifest.h — 🎯T64.5 build manifest reader
//
// Provides runtime access to the build provenance embedded by the ship
// pipeline into the iOS bundle / Android APK / macOS app bundle.
//
// Usage:
//   #include <ge/manifest.h>
//
//   if (auto m = ge::readBuildManifest()) {
//       SPDLOG_INFO("Build: {} ({}) by {}", m->tag, m->git_sha, m->builder);
//   }
//
// The manifest is a JSON file written by tools/ship/manifest.sh during the
// ship build and embedded as:
//   iOS:     {bundle}/Assets.xcassets/manifest.dataset/manifest.json
//   Android: APK res/raw/manifest.json  (read via ge::openFile)
//   macOS:   {bundle}/Resources/manifest.json  (ge::resource path)
//
// Returns nullopt if the file is absent (dev builds, simulator) or cannot
// be parsed — the caller decides how to surface that (log, debug screen, etc.).

#pragma once

#include <optional>
#include <string>

namespace ge {

struct BuildManifest {
    std::string tag;                  // e.g. "v0.31.0" or "alpha-42"
    std::string git_sha;              // full 40-char commit hash
    std::string git_branch;           // branch at build time, or "HEAD"
    std::string build_timestamp_utc;  // ISO-8601, e.g. "2026-05-24T19:00:00Z"
    std::string ge_version;           // semver string, e.g. "0.31.0"
    std::string lane;                 // "alpha" | "beta" | "release"
    std::string builder;              // "user@hostname"
};

// readBuildManifest() — parse the embedded manifest.json.
//
// Platform lookup order:
//   1. Checks ge::resource("manifest.json") — works on iOS bundle and macOS.
//   2. Checks ge::resource("manifest.dataset/manifest.json") — xcassets dataset path.
//   3. Returns nullopt if neither path resolves or parsing fails.
//
// Thread-safe: result is computed once and cached after the first call.
std::optional<BuildManifest> readBuildManifest();

} // namespace ge
