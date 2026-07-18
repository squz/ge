// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T154: drive the shipped durableDbPath + SP2T dump/load policy.

#include <doctest.h>
#include <ge/StreamHostPolicy.h>
#include <sqlpipe.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

TEST_CASE("stream server durable db is always :memory:") {
    CHECK(ge::durableDbPathForHost(true, "org", "app", "/Users/x/Library/") ==
          ":memory:");
    CHECK(ge::durableDbPathForHost(true, nullptr, nullptr, nullptr) ==
          ":memory:");
}

TEST_CASE("direct app durable db uses PrefPath when available") {
    CHECK(ge::durableDbPathForHost(false, "org", "app", "/tmp/pref/") ==
          "/tmp/pref/game.db");
    CHECK(ge::durableDbPathForHost(false, "org", "app", nullptr) ==
          ":memory:");
    CHECK(ge::durableDbPathForHost(false, nullptr, "app", "/tmp/") ==
          ":memory:");
}

TEST_CASE("player durable db is per-game under PrefPath/games/") {
    CHECK(ge::durableDbPathForPlayer("tiltbuggy", "/var/player/") ==
          "/var/player/games/tiltbuggy.db");
    CHECK(ge::durableDbPathForPlayer("tilt cal", "/var/player/") ==
          "/var/player/games/tilt_cal.db");
    CHECK(ge::durableDbPathForPlayer("tiltbuggy", nullptr) == ":memory:");
    CHECK(ge::durableDbPathForPlayer(nullptr, "/var/player/") == ":memory:");
    CHECK(ge::durableDbPathForPlayer("", "/var/player/") == ":memory:");
}

TEST_CASE("player durable paths for different games do not collide") {
    const auto a = ge::durableDbPathForPlayer("tiltbuggy", "/pref/");
    const auto b = ge::durableDbPathForPlayer("tiltcal", "/pref/");
    CHECK(a != b);
    CHECK(a == "/pref/games/tiltbuggy.db");
    CHECK(b == "/pref/games/tiltcal.db");
}

TEST_CASE("sanitizeGameId strips path separators and dots-only") {
    CHECK(ge::sanitizeGameId("a/b\\c") == "a_b_c");
    CHECK(ge::sanitizeGameId("..") == "");
    CHECK(ge::sanitizeGameId("TiltBuggy-1") == "TiltBuggy-1");
}

TEST_CASE("SP2T dump/load round-trips rows through shipped helpers") {
    // Write on the "player" side durable file (per-game path).
    const auto dir = std::filesystem::temp_directory_path() / "ge-t154-sp2t";
    std::filesystem::create_directories(dir / "games");
    const std::string playerPath =
        ge::durableDbPathForPlayer("tiltbuggy", (dir.string() + "/").c_str());
    REQUIRE(playerPath.find("games/tiltbuggy.db") != std::string::npos);
    std::filesystem::remove(playerPath);

    auto textAt = [](const sqlpipe::QueryResult& r, size_t row, size_t col) -> std::string {
        REQUIRE(row < r.rows.size());
        REQUIRE(col < r.rows[row].size());
        const auto& v = r.rows[row][col];
        if (const auto* s = std::get_if<std::string>(&v)) return *s;
        if (const auto* i = std::get_if<std::int64_t>(&v)) return std::to_string(*i);
        if (const auto* d = std::get_if<double>(&v)) {
            // COUNT(*) may come back as integer; tolerate REAL too.
            return std::to_string(static_cast<long long>(*d));
        }
        return {};
    };

    {
        sqlpipe::Database player(playerPath, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
        REQUIRE(ge::sqliteHasUserTables(player.handle()));
        player.exec("INSERT INTO t(v) VALUES ('from-player')");
        auto rows = player.query("SELECT v FROM t");
        CHECK(rows.rows.size() == 1);
        CHECK(textAt(rows, 0, 0) == "from-player");

        std::vector<uint8_t> dump;
        REQUIRE(ge::dumpSqliteMain(player.handle(), dump));
        REQUIRE(dump.size() > 100);

        // Server working set is :memory:; apply player snapshot (attach).
        sqlpipe::Database server(":memory:", "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
        REQUIRE(ge::loadSqliteMain(server.handle(), dump));
        server.notify();
        auto srows = server.query("SELECT v FROM t");
        REQUIRE(srows.rows.size() == 1);
        CHECK(textAt(srows, 0, 0) == "from-player");

        // Server mutates, dumps back (stream / detach push).
        server.exec("INSERT INTO t(v) VALUES ('from-server')");
        std::vector<uint8_t> dump2;
        REQUIRE(ge::dumpSqliteMain(server.handle(), dump2));

        // Player durable store is the raw serialize image on disk (SP2T body).
        {
            std::ofstream out(playerPath, std::ios::binary | std::ios::trunc);
            REQUIRE(out.good());
            out.write(reinterpret_cast<const char*>(dump2.data()),
                      static_cast<std::streamsize>(dump2.size()));
        }
        sqlpipe::Database playerReload(playerPath);
        auto prow = playerReload.query("SELECT COUNT(*) AS c FROM t");
        REQUIRE(prow.rows.size() == 1);
        CHECK(textAt(prow, 0, 0) == "2");

        // Reconnect: load durable file into a fresh server :memory: working set.
        std::vector<uint8_t> reconnectDump;
        REQUIRE(ge::dumpSqliteMain(playerReload.handle(), reconnectDump));
        sqlpipe::Database server2(":memory:");
        REQUIRE(ge::loadSqliteMain(server2.handle(), reconnectDump));
        auto r2 = server2.query("SELECT v FROM t ORDER BY id");
        REQUIRE(r2.rows.size() == 2);
        CHECK(textAt(r2, 0, 0) == "from-player");
        CHECK(textAt(r2, 1, 0) == "from-server");
    }

    // A second game keeps a separate durable file.
    const std::string otherPath =
        ge::durableDbPathForPlayer("othergame", (dir.string() + "/").c_str());
    CHECK(otherPath != playerPath);
    CHECK(!std::filesystem::exists(otherPath));

    std::filesystem::remove_all(dir);
}

TEST_CASE("sqliteHasUserTables is false on fresh empty db") {
    sqlpipe::Database empty(":memory:");
    CHECK_FALSE(ge::sqliteHasUserTables(empty.handle()));
    empty.exec("CREATE TABLE pose(id INTEGER PRIMARY KEY, x REAL);");
    CHECK(ge::sqliteHasUserTables(empty.handle()));
}
