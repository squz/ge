// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Engine-internal app-channel helpers, split out so the hello slice-descriptor
// encoding (🎯T116) can be unit-tested without a live socket. NOT a public
// header: lives in src/, included only by appchannel.cpp and appchannel_test.cpp.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ge::appchannel::detail {

// Build the `hello` message's `slices` array from registered (name, example)
// pairs, in the mixed form spyder T81+ decodes (SliceDescriptor.DecodeMsgpack):
//
//   - example is null  → a bare JSON string  (msgpack str)   — name-only,
//                        kept compact; pre-T81 spyder builds still see the name.
//   - example non-null → a JSON object {name, example}       — gives a
//                        connected agent an inline filter-writing template.
//
// nlohmann's to_msgpack encodes each element by its JSON type, so one array can
// carry both forms — exactly what spyder accepts. Map key order is irrelevant
// (spyder decodes by key), so this stays wire-compatible regardless of how the
// JSON object orders `name` / `example`.
nlohmann::json buildSliceDescriptors(
    const std::vector<std::pair<std::string, nlohmann::json>>& slices);

// 🎯T109 One exportable hit target (pure DTO for tests + encoder).
struct HitTargetExport {
    std::string id;
    std::string kind = "button";
    std::string role;
    std::string label;
    float       x = 0, y = 0, w = 0, h = 0;  // pts, y-down
    bool        enabled = true;
};

// Build {"targets":[...]} with bbox / bbox_norm / center_norm when surface > 0.
nlohmann::json buildHitTargetsPayload(
    float surfaceW, float surfaceH,
    const std::vector<HitTargetExport>& items,
    const nlohmann::json& extras = nlohmann::json::array());

} // namespace ge::appchannel::detail
