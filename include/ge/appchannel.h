// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T92 — dev-only bidirectional MessagePack-RPC channel to spyder's
// app_channel_* tools. The single dev path for structured logs (🎯T119: the
// T83 plain-text sink is gone — spyder dropped the listener in v0.58.0).
//
// Activated when SPYDER_APP_CHANNEL is set to "host:port" (🎯T119; spyder's
// launch_app / deploy_app inject it). On connect the app sends a `hello`
// request advertising its app_name / app_version and the method names it has
// handlers for, and awaits spyder's {spyder_version, accepted_methods} before
// the channel is live.
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

namespace ge {
class Context;  // T175 session-scoped overloads
}

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
//
// Optional `exampleParams` / `doc` ride in hello as a method descriptor
// ({name, example_params?, doc?}) so spyder's app_methods can surface the
// API without reading game source. Bare name-only when both are empty
// (engine builtins). Game-private commands should pass a one-line example
// object (shape template, not live data).
void registerMethod(std::string method, Handler handler,
                    nlohmann::json exampleParams = nullptr,
                    std::string doc = "");

// If SPYDER_APP_CHANNEL names a "host:port" listener, dial it on a background
// thread and perform the hello handshake; otherwise no-op. Idempotent.
// `appName` / `appVersion` are advertised in hello.
void installFromEnv(const std::string& appName, const std::string& appVersion);

// Send an async push (no response): {method, params}. Used by the log /
// perf push paths (T92.4). Safe to call from any thread; no-op when the
// channel isn't active.
void push(std::string method, nlohmann::json params);

// True once the hello handshake has completed and the channel is live.
bool active();

// 🎯T136 Block up to `timeoutMs` until queued pushes drain to the socket, so a
// last-gasp crash / uncaught-exception log reaches spyder before the process
// exits. Bounded — a crash reporter must never hang. No-op when no channel is
// active and compiled out in release builds.
void flush(int timeoutMs = 200);

// 🎯T92.2 Dev time-control. The SessionHost run loop calls this once per
// frame with the real (clamped) frame dt; it returns the dt to hand to
// onUpdate after applying the pause / resume / step / speed state driven by
// spyder's app_pause/app_resume/app_step/app_speed. When paused it returns
// 0 (render + input keep running so the app stays responsive); a pending
// `step` advances exactly one frame at a fixed nominal dt and is consumed
// here. Returns realDt unchanged when no channel is active, and is a no-op
// pass-through in release builds.
float applyTimeControl(float realDt);
// T175.10 Session-exact form for the run loop; the no-arg form maps to the
// sole live session (else the process-defaults store).
float applyTimeControl(const Context& ctx, float realDt);

// 🎯T92.4 Perf push. perfEmit records the latest value of a named custom
// counter — consumers call it from the game thread (e.g. each frame). perfTick
// is called once per frame by the SessionHost run loop with the real frame
// time in ms; it pushes a {frame_ms, counters} `perf` message roughly once per
// second (averaging frame_ms over the window). Both are no-ops when no channel
// is active and compiled out in release builds. The push itself is enqueued on
// the channel's async sender, so the game thread never blocks on the socket.
void perfEmit(const std::string& name, double value);
void perfEmit(const Context& ctx, const std::string& name, double value);  // T175.4
void perfTick(float frameMs);

// 🎯T92.5 State registry. Register these BEFORE ge::run so the slice names are
// advertised in the hello. The getters / save / restore callbacks run on the
// GAME THREAD (the run loop marshals them) so they observe a consistent
// snapshot, never a torn read against the simulation. All no-ops / empty in
// release builds.
using StateGetter   = std::function<nlohmann::json()>;
using StateRestorer = std::function<void(const nlohmann::json&)>;

// A named, queryable slice of app state — spyder's app_state{slice} routes
// here and returns the getter's JSON.
//
// 🎯T116 `example` (optional) is a small, representative snapshot of the slice
// — its shape, not live data. When non-null it rides in the `hello` as a
// {name, example} slice descriptor (spyder T81+), so a connected agent gets a
// filter-writing template at connect time and skips a `state_query` round-trip
// (it can also fall back to `app_state_describe`). Captured at registration
// (before ge::run) — pass a one-line literal or a getter's initial output, not
// a multi-screen dump. Omit it for the compact name-only descriptor; pre-T81
// spyder builds ignore the example and still see the name.
void registerStateSlice(std::string name, StateGetter getter,
                        nlohmann::json example = nullptr);

// Whole-app save/restore — spyder's app_save_state / app_restore_state. `save`
// returns a JSON snapshot; ge MessagePack+base64-encodes it for the wire and
// hands the decoded JSON back to `restore`.
void registerStateSerializer(StateGetter save, StateRestorer restore);

// T175.2 Session-scoped registrations: use inside a factory (where the
// Context is in scope) so each session owns its slices/serializer. The
// Context-less forms above register process defaults every session
// inherits; a session's own registration shadows a default of the same
// name. RPCs address a session with an optional {"session": <id>} param
// (default: the sole live session).
void registerStateSlice(const Context& ctx, std::string name, StateGetter getter,
                        nlohmann::json example = nullptr);
void registerStateSerializer(const Context& ctx, StateGetter save, StateRestorer restore);

// Drain tasks the state handlers marshalled onto the game thread. The
// SessionHost run loop calls this once per iteration; consumers don't. No-op
// in release builds.
void pumpMainThreadTasks();
void pumpMainThreadTasks(const Context& ctx);  // T175.3 drains ctx's own queue

// 🎯T109 Hit-target export surface (built-in `hit_targets` state slice).
//
// Buttons published via ge::publishHitTarget with non-empty id + hitBounds
// appear automatically. setHitTargetSurfacePts supplies full-surface size so
// the slice can emit bbox_norm / center_norm for app_input 0–1 inject.
// setExtraHitTargets adds non-button descriptors (regions, SVG, …) with the
// same field shape; replaced wholesale each call (pass [] to clear).
//
// installFromEnv registers the slice before the hello handshake. All no-ops
// in release builds.
void setHitTargetSurfacePts(float width, float height);
void setExtraHitTargets(nlohmann::json targetsArray);
void setExtraHitTargets(const Context& ctx, nlohmann::json targetsArray);  // T175.2

} // namespace ge::appchannel
