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

namespace ge {
class Context;  // T175 session-scoped test hooks
}

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

// Build hello.methods mixed array (spyder MethodDescriptor):
//   - no example and empty doc → bare string name
//   - else → {name, example_params?, doc?}
struct MethodAdvert {
    std::string name;
    nlohmann::json example_params = nullptr;  // null = omit
    std::string doc;
};
nlohmann::json buildMethodDescriptors(const std::vector<MethodAdvert>& methods);

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

// ── 🎯T175.12 characterization hooks ────────────────────────────────
// Drive registered app-channel handlers without a socket, so the surfaces
// the T175 session-scoping graph rescopes are locked by tests first.
// Debug builds only (like the rest of the app-channel).

// Ensure builtins are registered, look up `name`, and invoke it. Handlers
// that marshal via runOnGameThread BLOCK until pumpMainThreadTasks() runs —
// invoke those from a worker thread and pump from the test's main thread
// (that blocking is itself characterized behaviour). Throws on unknown name;
// handler Errors propagate.
nlohmann::json invokeMethodForTest(const std::string& name,
                                   const nlohmann::json& params);
bool hasMethodForTest(const std::string& name);

// The perf-window arithmetic behind perfTick, channel-independent:
// accumulate one frame; returns the samples object (frame_ms + counters)
// when the window elapsed and resets it, else null. Production perfTick
// calls this after its active() gate, so tests lock the shipped math.
nlohmann::json perfAccumulate(float frameMs);
nlohmann::json perfCountersSnapshotForTest();
nlohmann::json perfCountersSnapshotForTest(const Context& ctx);  // T175.4

// Reset every session-varying app-channel global (slices, serializers, hit
// surface/extras, time control, perf window, task queue) to construction
// state. Test isolation only.
void resetAppChannelStateForTest();

// ── 🎯T175.1 session addressing ─────────────────────────────────────
// Resolve an RPC's optional {"session": <id>} param to a live session id.
// Absent param: the sole live session (the overwhelmingly common case —
// direct mode, and stream mode under T163 process-per-session). Throws a
// JSON-RPC Error when the named id isn't live, when no session exists, or
// when several exist and none was named. The one resolver every
// session-addressed dev RPC shares (T175.2/3/4/9/10).
uint32_t resolveSessionParam(const nlohmann::json& params);

} // namespace ge::appchannel::detail
