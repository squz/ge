// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T154: drive the shipped durableDbPathForHost policy (StreamHostPolicy.h).

#include <doctest.h>
#include <ge/StreamHostPolicy.h>

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
