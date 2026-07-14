// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Stream host packaging policy used by DirectRenderHost under GE_SERVER_BUILD
// and by the player for durable GE2T state (🎯T154).
// Kept header-only so unit tests drive the same functions as production.

#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace ge {

// Durable Context::db() path on the *game host* process.
// serverBuild true  → always :memory: (working set only; player is authority)
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

// Durable path for the *player* (glass) process — player-authoritative store
// under stream (GE2T). Same layout as direct PrefPath game.db when a base
// path is provided.
inline std::string durableDbPathForPlayer(const char* orgName,
                                          const char* appName,
                                          const char* prefPathOrNull) {
    if (!orgName || !appName || !prefPathOrNull || !*prefPathOrNull)
        return ":memory:";
    return std::string(prefPathOrNull) + "game.db";
}

// ── GE2T snapshot helpers (full SQLite serialize of "main") ──────────
// Wire payload after MessageHeader{kSqlpipeMsgMagic}: raw sqlite3_serialize
// bytes of the main schema. Player holds durable file; server applies into
// :memory: on attach and pushes back on detach so reconnect restores.

inline bool dumpSqliteMain(sqlite3* db, std::vector<uint8_t>& out) {
    out.clear();
    if (!db) return false;
    sqlite3_int64 sz = 0;
    unsigned char* p = sqlite3_serialize(db, "main", &sz, 0);
    if (!p || sz <= 0) {
        if (p) sqlite3_free(p);
        return false;
    }
    out.assign(p, p + static_cast<size_t>(sz));
    sqlite3_free(p);
    return true;
}

// Replace "main" on an open connection with a serialize blob.
// On success the connection owns a copy of the data.
inline bool loadSqliteMain(sqlite3* db, std::span<const uint8_t> blob) {
    if (!db || blob.empty()) return false;
    // Copy for SQLite ownership (FREEONCLOSE).
    auto* copy = static_cast<unsigned char*>(sqlite3_malloc64(blob.size()));
    if (!copy) return false;
    std::memcpy(copy, blob.data(), blob.size());
    const int rc = sqlite3_deserialize(
        db, "main", copy, static_cast<sqlite3_int64>(blob.size()),
        static_cast<sqlite3_int64>(blob.size()),
        SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
    if (rc != SQLITE_OK) {
        sqlite3_free(copy);
        return false;
    }
    return true;
}

} // namespace ge
