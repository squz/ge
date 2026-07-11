// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Minimal appchannel stubs for the lean Android player link. The brokered
// player does not open an app-channel; Signal.cpp still references flush on
// the crash path, and player_core optionally emits stream_stats — provide
// no-ops rather than pulling the full channel + SDL_image + tweak graph.

#include <ge/appchannel.h>

#include <string>

namespace ge::appchannel {

void flush(int) {}

void installFromEnv(const std::string&, const std::string&) {}

void perfEmit(const std::string&, double) {}

void push(std::string, nlohmann::json) {}

}  // namespace ge::appchannel
