// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T116 — hello slice-descriptor encoding. Mirrors spyder's
// internal/appchannel/slice_descriptor_test.go TestSliceDescriptor_* against
// the ge encoder (detail::buildSliceDescriptors): the to_msgpack wire shape is
// the mixed bare-string / {name, example} form spyder's SliceDescriptor.
// DecodeMsgpack accepts (PeekCode → IsString ? string : map).

#include "appchannel_internal.h"

#include <doctest.h>

#include <nlohmann/json.hpp>

#include <cstdint>

using nlohmann::json;
using ge::appchannel::detail::buildSliceDescriptors;

namespace {
// Round-trip a descriptor array through MessagePack, as it rides the wire.
json viaMsgpack(const json& j) {
    return json::from_msgpack(json::to_msgpack(j));
}
} // namespace

TEST_CASE("buildSliceDescriptors: name-only slices encode as bare strings") {
    // spyder TestSliceDescriptor_DecodeBareString — pre-T81 wire form.
    const auto arr = buildSliceDescriptors({
        {"scene", nullptr}, {"physics", nullptr}, {"hud", nullptr},
    });
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 3);
    const char* names[] = {"scene", "physics", "hud"};
    for (int i = 0; i < 3; ++i) {
        CHECK(arr[i].is_string());            // bare string, not a map
        CHECK(arr[i] == names[i]);
    }
    CHECK(viaMsgpack(arr) == arr);            // survives the wire unchanged

    // On the wire the element is a msgpack fixstr → spyder PeekCode = IsString.
    const auto bytes = json::to_msgpack(json::array({"scene"}));
    REQUIRE(bytes.size() >= 2);              // [0] = array header, [1] = element
    CHECK((bytes[1] & 0xe0) == 0xa0);        // 0xa_ = fixstr
}

TEST_CASE("buildSliceDescriptors: a volunteered example encodes as {name, example}") {
    // spyder TestSliceDescriptor_DecodeStructForm.
    const json example = {
        {"marble", {{"position", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}}}},
    };
    const auto arr = buildSliceDescriptors({{"physics", example}});
    REQUIRE(arr.size() == 1);
    CHECK(arr[0].is_object());
    CHECK(arr[0]["name"] == "physics");
    CHECK(arr[0]["example"]["marble"]["position"]["x"] == 0.0);

    const auto rt = viaMsgpack(arr);         // nested example survives the wire
    CHECK(rt[0]["name"] == "physics");
    CHECK(rt[0]["example"].contains("marble"));

    // The element is a msgpack map → spyder PeekCode falls through to DecodeMap.
    const auto bytes = json::to_msgpack(arr[0]);
    CHECK((bytes[0] & 0xf0) == 0x80);        // 0x8_ = fixmap
}

TEST_CASE("buildHitTargetsPayload: buttons get center_norm from surface") {
    using ge::appchannel::detail::HitTargetExport;
    using ge::appchannel::detail::buildHitTargetsPayload;
    const std::vector<HitTargetExport> items = {
        {.id = "reset", .kind = "button", .role = "reset", .label = "TiltBuggy",
         .x = 100, .y = 0, .w = 200, .h = 40, .enabled = true},
    };
    const auto j = buildHitTargetsPayload(400, 800, items);
    REQUIRE(j["targets"].size() == 1);
    CHECK(j["targets"][0]["id"] == "reset");
    CHECK(j["targets"][0]["center_norm"][0] == doctest::Approx(0.5));
    CHECK(j["targets"][0]["center_norm"][1] == doctest::Approx(0.025));
    CHECK(j["targets"][0]["space"] == "pts");
}

TEST_CASE("buildHitTargetsPayload: empty id or zero size is omitted") {
    using ge::appchannel::detail::HitTargetExport;
    using ge::appchannel::detail::buildHitTargetsPayload;
    const std::vector<HitTargetExport> items = {
        {.id = "", .x = 0, .y = 0, .w = 10, .h = 10},
        {.id = "x", .x = 0, .y = 0, .w = 0, .h = 10},
    };
    const auto j = buildHitTargetsPayload(100, 100, items);
    CHECK(j["targets"].empty());
}

TEST_CASE("buildHitTargetsPayload: extras merge as regions") {
    using ge::appchannel::detail::HitTargetExport;
    using ge::appchannel::detail::buildHitTargetsPayload;
    const auto extras = nlohmann::json::array({
        {{"id", "playfield"}, {"kind", "region"}, {"role", "playfield"},
         {"bbox", {0, 0, 100, 200}}, {"enabled", true}},
    });
    const auto j = buildHitTargetsPayload(100, 200, {}, extras);
    REQUIRE(j["targets"].size() == 1);
    CHECK(j["targets"][0]["kind"] == "region");
    CHECK(j["targets"][0]["center_norm"][0] == doctest::Approx(0.5));
    CHECK(j["targets"][0]["center_norm"][1] == doctest::Approx(0.5));
}

TEST_CASE("buildSliceDescriptors: mixed bare + example slices in one array") {
    // spyder TestSliceDescriptor_DecodeMixed — apps upgrade slice-by-slice.
    const auto arr = buildSliceDescriptors({
        {"scene",   nullptr},
        {"physics", json{{"k", 1}}},
        {"hud",     nullptr},
    });
    REQUIRE(arr.size() == 3);
    CHECK(arr[0].is_string());  CHECK(arr[0] == "scene");
    CHECK(arr[1].is_object());  CHECK(arr[1]["name"] == "physics");
                                CHECK(arr[1]["example"]["k"] == 1);
    CHECK(arr[2].is_string());  CHECK(arr[2] == "hud");
    CHECK(viaMsgpack(arr) == arr);
}

TEST_CASE("buildSliceDescriptors: empty registry yields an empty array") {
    const auto arr = buildSliceDescriptors({});
    CHECK(arr.is_array());
    CHECK(arr.empty());
}

TEST_CASE("buildMethodDescriptors: bare + rich mixed hello methods") {
    using ge::appchannel::detail::MethodAdvert;
    using ge::appchannel::detail::buildMethodDescriptors;
    const auto arr = buildMethodDescriptors({
        MethodAdvert{.name = "ping"},
        MethodAdvert{
            .name = "select_country",
            .example_params = json{{"id", "FR"}},
            .doc = "Select by ISO id",
        },
        MethodAdvert{.name = "globe_look_at", .example_params = json{{"lon", 0.0}}, .doc = ""},
    });
    REQUIRE(arr.size() == 3);
    CHECK(arr[0].is_string());
    CHECK(arr[0] == "ping");
    CHECK(arr[1].is_object());
    CHECK(arr[1]["name"] == "select_country");
    CHECK(arr[1]["example_params"]["id"] == "FR");
    CHECK(arr[1]["doc"] == "Select by ISO id");
    CHECK(arr[2].is_object());
    CHECK(arr[2]["name"] == "globe_look_at");
    CHECK(arr[2].contains("example_params"));
    CHECK(!arr[2].contains("doc"));
    CHECK(viaMsgpack(arr) == arr);
}

// ── 🎯T119: log-push envelope matches spyder's LogPush wire shape ──────────
// Mirrors src/log.cpp's AppChannelLogSink and spyder's
// internal/appchannel/session.go LogPush (msgpack tags). The timestamp field
// is `ts`, NOT `timestamp` — the latter silently failed to decode on spyder's
// side, which is the regression this guards.
TEST_CASE("log push envelope encodes the {ts, level, subsystem, format} shape") {
    const json params = {
        {"ts",        std::int64_t{1718000000000}},
        {"level",     "info"},
        {"subsystem", "ge"},
        {"format",    "hello world"},
    };
    const json push = {{"method", "log"}, {"params", params}};  // {method, params}, no id

    const json rt = json::from_msgpack(json::to_msgpack(push));  // survives the wire
    CHECK(rt["method"] == "log");
    REQUIRE(rt.contains("params"));
    const auto& p = rt["params"];
    CHECK(p.contains("ts"));               // spyder LogPush is msgpack:"ts"
    CHECK_FALSE(p.contains("timestamp"));  // the pre-T119 field that didn't decode
    CHECK(p["ts"].get<std::int64_t>() == 1718000000000);
    CHECK(p["level"]     == "info");
    CHECK(p["subsystem"] == "ge");
    CHECK(p["format"]    == "hello world");
}
