// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T166 metrics ring: selection, isolation, wrap, dump.

#include <ge/metrics.h>

#include <doctest.h>

#include <string>
#include <vector>

using ge::metrics::Scope;
using ge::metrics::metric;

TEST_CASE("metrics: assignment no-op when unarmed") {
    Scope scope{"t-unarmed"};
    metric<float> dt{scope, "dt"};
    dt = 1.5f;
    scope.endFrame();
    auto d = scope.dump();
    CHECK(d["count"] == 0);
    CHECK(scope.status()["armed"] == false);
}

TEST_CASE("metrics: arm selects series only") {
    Scope scope{"t-select"};
    metric<float> dt{scope, "dt"};
    metric<float> zoom{scope, "zoom"};
    metric<float> mag{scope, "mag"};

    scope.arm({"dt", "zoom"}, 16);
    for (int i = 0; i < 5; ++i) {
        dt = float(i);
        zoom = 10.f + float(i);
        mag = 99.f; // not armed — must not appear
        scope.endFrame();
    }
    auto d = scope.dump();
    REQUIRE(d["count"] == 5);
    REQUIRE(d["series"].size() == 2);
    CHECK(d["series"][0] == "dt");
    CHECK(d["series"][1] == "zoom");
    CHECK(d["frames"][0][0] == doctest::Approx(0.0));
    CHECK(d["frames"][0][1] == doctest::Approx(10.0));
    CHECK(d["frames"][4][0] == doctest::Approx(4.0));
    CHECK(d["frames"][4][1] == doctest::Approx(14.0));
    // mag not in dump columns
    CHECK(d["frames"][0].size() == 2);
}

TEST_CASE("metrics: two scopes are isolated") {
    Scope a{"inst-a"};
    Scope b{"inst-b"};
    metric<float> aDt{a, "dt"};
    metric<float> bDt{b, "dt"};

    a.arm({"dt"}, 8);
    b.arm({"dt"}, 8);
    aDt = 1.f;
    bDt = 2.f;
    a.endFrame();
    b.endFrame();
    aDt = 3.f;
    a.endFrame();

    auto da = a.dump();
    auto db = b.dump();
    REQUIRE(da["count"] == 2);
    REQUIRE(db["count"] == 1);
    CHECK(da["frames"][0][0] == doctest::Approx(1.0));
    CHECK(da["frames"][1][0] == doctest::Approx(3.0));
    CHECK(db["frames"][0][0] == doctest::Approx(2.0));
    CHECK(da["instance"] == "inst-a");
    CHECK(db["instance"] == "inst-b");

    // Disarm a must not clear b
    a.disarm();
    CHECK(a.dump()["count"] == 0);
    CHECK(b.dump()["count"] == 1);
}

TEST_CASE("metrics: ring wraps oldest first") {
    Scope scope{"t-wrap"};
    metric<float> dt{scope, "dt"};
    scope.arm({"dt"}, 3);
    for (int i = 0; i < 5; ++i) {
        dt = float(i);
        scope.endFrame();
    }
    auto d = scope.dump();
    REQUIRE(d["count"] == 3);
    // frames 2,3,4 remain
    CHECK(d["frames"][0][0] == doctest::Approx(2.0));
    CHECK(d["frames"][1][0] == doctest::Approx(3.0));
    CHECK(d["frames"][2][0] == doctest::Approx(4.0));
}

TEST_CASE("metrics: list advertises registered series") {
    Scope scope{"t-list"};
    metric<float> dt{scope, "dt"};
    metric<std::uint16_t> sel{scope, "sel"};
    auto L = scope.list();
    CHECK(L["instance"] == "t-list");
    REQUIRE(L["series"].size() == 2);
    CHECK(L["series"][0]["name"] == "dt");
    CHECK(L["series"][0]["kind"] == "float");
    CHECK(L["series"][1]["name"] == "sel");
    CHECK(L["series"][1]["kind"] == "u16");
}

TEST_CASE("metrics: find and all registry") {
    Scope a{"find-a"};
    Scope b{"find-b"};
    CHECK(Scope::find("find-a") == &a);
    CHECK(Scope::find("find-b") == &b);
    CHECK(Scope::find("missing") == nullptr);
    auto all = Scope::all();
    CHECK(all.size() >= 2);
}
