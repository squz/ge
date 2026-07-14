// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Stream host packaging policy used by DirectRenderHost under GE_SERVER_BUILD.
// Kept header-only so unit tests drive the same function as production.

#pragma once

#include <string>

namespace ge {

// Durable Context::db() path (🎯T154).
// serverBuild true  → always :memory: (player owns durable state via GE2T later)
// serverBuild false → PrefPath + "game.db" when org/app/pref provided
inline std::string durableDbPathForHost(bool serverBuild,
                                        const char* orgName,
                                        const char* appName,
                                        const char* prefPathOrNull) {
    if (serverBuild) return ":memory:";
    if (!orgName || !appName || !prefPathOrNull || !*prefPathOrNull)
        return ":memory:";
    return std::string(prefPathOrNull) + "game.db";
}

} // namespace ge
