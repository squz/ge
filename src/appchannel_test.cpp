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
