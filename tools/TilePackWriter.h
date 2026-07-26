// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.1 Library entry point for the .getp tile-pack cook, factored out of
// tools/texpack.cpp's main() so the round-trip test (src/TilePackFormat_test.cpp)
// can invoke the cook directly instead of shelling out to the CLI.
//
// Deliberately NOT under include/ge/ — this is a build-time tool surface, not
// part of the engine's runtime public API (consumers never link against it;
// see tools/ge-sources.mk's opt-in convention for build-time-only sources).

#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace ge {

// Cooks a .getp tile pack from `config` (schema documented at the top of
// tools/texpack.cpp). Relative "output" and per-plane "input.path" strings
// resolve against `baseDir` (pass "" — the default — when `config` already
// carries absolute/CWD-relative paths, e.g. from a test).
//
// Returns true on success. Returns false and fills `err` with a human-
// readable message on any failure (bad config, unreadable input, encode
// error, ...); never throws.
bool cookTilePack(const nlohmann::json& config, std::string& err,
                   const std::string& baseDir = "");

} // namespace ge
