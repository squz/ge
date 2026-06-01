// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T92 — dev-only bidirectional MessagePack-RPC channel to spyder's
// app_channel_* tools. The structured sibling of the T83 text log sink.
//
// Activated when LOG_TARGET is "appchannel://host:port" (a bare
// "host:port" keeps the T83 text NetworkLogSink unchanged). On connect the
// app sends a `hello` request advertising its app_name / app_version and
// the method names it has handlers for, and awaits spyder's
// {spyder_version, accepted_methods} before the channel is live.
//
// Wire format (spyder T75): length-prefixed frames — [4-byte LE length]
// [MessagePack body], max 16 MB — with a JSON-RPC-shaped envelope:
//   request : {id, method, params}      (either direction)
//   response: {id, result} | {id, error:{code,message,data?}}
//   push    : {method, params}          (id omitted; no response)
// MessagePack encode/decode is nlohmann::json::to_msgpack/from_msgpack
// (already vendored) — no hand-rolled MessagePack, no new dependency.
//
// The ENTIRE feature is compiled out under NDEBUG (same gate as T83): in a
// release build there is no socket, no msgpack, no handler table, and the
// functions below are empty no-ops.

#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace ge::appchannel {

// A request handler: receives the request's `params` and returns the
// `result` json. Throw ge::appchannel::Error to send a JSON-RPC error.
using Handler = std::function<nlohmann::json(const nlohmann::json& params)>;

// Thrown by a Handler to produce {id, error:{code, message}}.
struct Error {
    int code;
    std::string message;
};

// Register a request handler. Must be called BEFORE installFromEnv so the
// method is advertised in `hello`. No-op in release builds.
void registerMethod(std::string method, Handler handler);

// If LOG_TARGET names an "appchannel://host:port" listener, dial it on a
// background thread and perform the hello handshake; otherwise no-op.
// Idempotent. `appName` / `appVersion` are advertised in hello.
void installFromEnv(const std::string& appName, const std::string& appVersion);

// Send an async push (no response): {method, params}. Used by the log /
// perf push paths (T92.4). Safe to call from any thread; no-op when the
// channel isn't active.
void push(std::string method, nlohmann::json params);

// True once the hello handshake has completed and the channel is live.
bool active();

// 🎯T92.2 Dev time-control. The SessionHost run loop calls this once per
// frame with the real (clamped) frame dt; it returns the dt to hand to
// onUpdate after applying the pause / resume / step / speed state driven by
// spyder's app_pause/app_resume/app_step/app_speed. When paused it returns
// 0 (render + input keep running so the app stays responsive); a pending
// `step` advances exactly one frame at a fixed nominal dt and is consumed
// here. Returns realDt unchanged when no channel is active, and is a no-op
// pass-through in release builds.
float applyTimeControl(float realDt);

} // namespace ge::appchannel
