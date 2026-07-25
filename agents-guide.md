# ge agent guide

**ge** is a reusable C++ rendering and streaming engine built on sokol_gfx + SDL3 (migrated from bgfx — 🎯T38), consumed as a git
submodule via `-include $(ge)/Module.mk`. It targets pre-1.0, so the interaction surface is still
settling — check `STABILITY.md` for stability annotations before modifying public headers.

The engine supports two operating modalities. In **brokered (server/player) mode** the app renders
headless via sokol_gfx, encodes frames as H.264 (VideoToolbox on Apple, FFmpeg on
Android). Optional **server-mode** builds encode H.264 and transmit to a player over a WebSocket
managed by **spyder's stream relay**. The player
decodes the stream, displays it in an SDL window, and relays input back to the server over the same
channel. In **direct mode** the app owns a real SDL window; sokol_gfx draws straight to it with no
encoding and no stream relay. Both modes share the same `ge::run()` entry point — the `RenderHost`
abstraction hides which modality is active.

`ge::run()` blocks until SIGINT or the last session ends. On each new player connect, it calls a
user-supplied **factory** with a `ge::Context` (dimensions, device class, engine DB), which returns a
`ge::RunConfig` (four `std::function` callbacks: `onUpdate`, `onRender`, `onEvent`, `onShutdown`).
State that must survive player reconnects lives outside the factory; per-session resources are
created inside it.

## When to change what

**ge/** is for reusable engine code — protocol, rendering infrastructure, sokol_gfx/SokolContext lifecycle, player
binaries, SDK headers, Module.mk. If the change would be useful to any app built on ge, it belongs
here.

**Consuming app** code (game state, rendering shaders, scene data, app-specific tweaks) stays in the
app. The canonical in-tree example is `sample/tiltbuggy/`.

If unsure, ask before creating: see `CLAUDE.md` §"When to change what" for the authoritative rule.

## Architecture

### Subsystems

| Directory | Purpose |
|-----------|---------|
| `include/ge/` | Public headers — one per class |
| `src/` | Engine implementation (platform-neutral) |
| `src/render/` | `DirectRenderHost` — SDL window + Metal/Vulkan surface for direct mode |
| `src/bridge/` | `ServerWireBridge` + `PlayerWireBridge` — H.264 encode/decode pipeline |
| `tools/` | Player binary, matrix-cell harness, smoke-test, platform init scripts |
| `tools/ios-template/` | iOS CMake/Xcode template (source of truth for `make ge/ios-init`) |
| `tools/android-template/` | Android Gradle template (source of truth for `make ge/android-init`) |
| `tools/ios/` | ge player iOS Xcode project |
| `tools/android/` | ge player Android Gradle project |
| `sample/tiltbuggy/` | In-tree sample app — canonical test vehicle for `make check` |
| `vendor/github.com/bkaradzic/{bgfx,bx,bimg}/` | Former bgfx libraries (vendored; retained as historical reference — migrated to sokol_gfx in 🎯T38) |

### Key interfaces

- **`RenderHost`** (`include/ge/RenderHost.h`) — abstract boundary between engine and render.
  Concrete implementations: `DirectRenderHost` (direct mode) and `ServerWireBridge` (brokered mode).
- **`PlayerWireBridge`** — player-side counterpart of `ServerWireBridge`. Wraps a `DirectRenderHost`,
  intercepts events for wire transmission, and feeds decoded frames as textures.
- **`SessionHost`** / `ge::run()` — app lifecycle: sokol_gfx init (via `SokolContext`), direct host or
  optional server-mode stream via spyder relay, frame loop, graceful shutdown.

### sokol_gfx backends (🎯T38 / 🎯T107)

- **Apple (macOS, iOS)**: Metal via `CAMetalLayer` (`SokolContext.mm`).
- **Android**: Vulkan with GLES3 fallback — runtime-selected per device by `SokolContext_android.cpp`. See the "Android renderer backend: Vulkan with GLES fallback (🎯T107)" section in CLAUDE.md for the full dispatch-shim architecture.

### spyder (dev control plane)

**Not part of this repo.** [spyder](https://github.com/marcelocantos/spyder) owns device inventory,
launch, reservations, app-channel inspect/tweak/logs, dashboard, and the optional H.264 stream relay.
The historical `ged` daemon was removed (🎯T145). Do not look for `make ged` / `bin/ged`.

## Build and run

All recipes assume the consuming app's Makefile includes `ge/Module.mk` and the standard targets are
present. For `sample/tiltbuggy/`, run all commands from that directory.

```bash
# Control plane (once per machine)
brew services start spyder   # or: spyder serve

# Engine and app — direct modality
make                        # build app binary
make run                    # build + run desktop direct mode

# Optional server-stream modality (server-mode build + spyder relay + native player)
make ge/player              # build desktop player binary (bin/player)
# launch server-mode app with stream host pointing at spyder; then:
bin/player --host 127.0.0.1 --port 3030 --name <server-name>

# iOS simulator
make ge/ios-init APP_ID=com.example.myapp APP_NAME=MyApp IOS_DEVELOPMENT_TEAM=XXXXXX
make ge/ios                 # builds ios/build/.../<app>.app
xcrun simctl install booted ios/build/xcode/Debug-iphonesimulator/<app>.app
xcrun simctl launch booted <bundle-id>

# Android emulator
make ge/android-init APP_ID=com.example.myapp APP_NAME=MyApp
make ge/android             # builds android/app/build/.../app-debug.apk
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.example.myapp/.MainActivity

# Tests
make unit-test              # ge unit tests (doctest)
make check                  # 24-cell end-to-end matrix (see below)
```

## Testing: the 24-cell matrix

`make check` runs the end-to-end test matrix defined in `Module.mk` (`ge/CELLS`). Each cell is a
`make cell.<name>` target that delegates to `tools/matrix-cell.sh`.

**24 canonical cells:**

```
desktop-{dist,player}
ios-sim-{phone,tablet}-{dist,player}
ios-device-{phone,tablet}-{dist,player}
android-emu-{phone,tablet}-{dist,player}
android-device-{phone,tablet}-{dist,player}
desktop-debug-{dist,player}
ios-debug-{dist,player}
android-debug-{dist,player}
```

**Per-cell sub-checks** (implemented in `matrix-cell.sh`): cold-launch, startup-flash, 60s soak,
rotation round-trip (simulator/emulator only), reconnect, background/foreground, clean-exit.

**Environment variables:**

| Variable | Purpose |
|----------|---------|
| `CHECK_EXCLUDE` | Space-separated shell globs to skip (e.g. `'android-device-*'`) |
| `GE_IOS_PHONE_DEVICE` | iOS physical device name/UDID for phone cells |
| `GE_IOS_TABLET_DEVICE` | iOS physical device name/UDID for tablet cells |
| `GE_ANDROID_PHONE_DEVICE` | Android device serial for phone cells |
| `GE_ANDROID_TABLET_DEVICE` | Android device serial for tablet cells |
| `GE_ANDROID_TABLET_AVD` | AVD name for tablet emulator cells |

```bash
# Skip all physical-device cells (typical CI)
CHECK_EXCLUDE='ios-device-* android-device-*' make check

# Run a single cell
make cell.ios-sim-tablet-dist
```

## Key abstractions

### `ge::run(factory, config)` — `include/ge/SessionHost.h`

Main entry point. Runs the direct (or server-mode) host; calls `factory(ctx)` for
each, and blocks until SIGINT or all sessions end.

```cpp
ge::run([&](ge::Context ctx) -> ge::RunConfig {
    auto app = std::make_shared<MyApp>(ctx);
    return {
        .onUpdate   = [app](float dt)         { app->update(dt); },
        .onRender   = [app](int w, int h)     { app->render(w, h); },
        .onEvent    = [app](const SDL_Event& e){ app->event(e); },
        .onShutdown = [app]()                 { app->shutdown(); },
    };
});
```

**Key rule:** state that survives reconnects lives *outside* the factory lambda.

### `ge::Context` — `include/ge/SessionHost.h`

Platform context passed to the factory. Provides `width()`, `height()`, `deviceClass()`, and `db()`
(engine-managed sqlpipe database). Cheaply copyable (shared_ptr internals); safe to capture by value.

### `RenderHost` — `include/ge/RenderHost.h`

Abstract interface between engine (draw calls) and render backend. The engine sees only `RenderHost`;
concrete implementations are selected at runtime:

- `DirectRenderHost` — real SDL window, local input. Used for distribution builds and direct mode.
- `ServerWireBridge` — headless framebuffer, H.264 encoder, WebSocket TX/RX. Used for brokered mode.

### `PlayerWireBridge` / `PlayerRender` — `include/ge/PlayerWire{Bridge,Render}.h`

Player-side split: `PlayerWireBridge` owns the wire (WebSocket, H.264 decode) while `PlayerRender`
owns all SDL state (window, texture, display). Not a `RenderHost` — it wraps `DirectRenderHost`.

### Headless render to PNG — `ge::renderToPng` / `ge::renderBatch` (🎯T124)

The hermetic counterpart to the app channel: render a saved **State** to a PNG on the
dev box — no device, no spyder, no window. Where the app channel drives a
*live* app, this replays a *serialized* one. An agent uses it to (a) see what a State
looks like with no device in the loop and (b) run a fast pixel-regression against
committed goldens.

- `ge::renderToPng(factory, config, prepare, outPath)` — builds a hidden
  `DirectRenderHost` (`SokolConfig.hidden` → off-screen drawable, `framebufferOnly = NO`),
  runs the factory once, calls `prepare()` to mutate the game State into the frame you
  want, renders one frame, reads the swapchain back to RGBA, writes a PNG via
  `ge::writePng`. `onRender` is unchanged — the same `c.swapchainPass()` path captures
  instead of presenting.
- `ge::renderBatch(factory, config, items)` — host + factory built **once**, then loops
  `RenderItem{prepare, outPath}` with per-item try/catch (one bad fixture doesn't sink the
  run). Returns the count rendered. The reused host is leak-free: a batch-rendered frame is
  byte-identical to a fresh single render of the same State.
- `config.hidden` and the render size both live on `SessionHostConfig`.

Determinism is a contract: same State + same size ⇒ byte-stable PNG. Kill any
render-liveness animation (frame-counter spins, time wobble) behind a flag the render path
disables — tiltbuggy's `Renderer::setDiagnosticSpin(false)` is the model.

**Consumer CLI.** An app exposes this by parsing argv before `ge::run`. tiltbuggy's
`render` verb is the reference:

```bash
# one State -> one PNG  (State on stdin with --state -)
bin/tiltbuggy render --state fixtures/tilted.json --out /tmp/tilted.png [--size 512x384]
# many in one process (amortised host); --isolate re-execs per fixture for crash isolation
bin/tiltbuggy render --batch fixtures/manifest.json [--isolate]
```

The manifest is a `[{"state": "...", "out": "..."}]` array. State JSON is whatever the app's
`registerStateSerializer` restore understands (tiltbuggy: gravity + buggy pose + pro flag).

**Regression loop.** `sample/tiltbuggy` ships committed `fixtures/*.json` + small
`fixtures/golden/*.png`; `make render-test` batch-renders and `imgdiff`s each vs its golden
(RMS threshold absorbs cross-GPU drift), `make update-render-goldens` re-blesses after a
deliberate visual change.

### `SokolContext` — `src/SokolContext.mm` (Apple) / `src/SokolContext_android.cpp` (Android)

RAII sokol_gfx device lifecycle. Handles Metal (Apple) and Vulkan-or-GLES3 (Android) init, pass
management, and headless vs. windowed mode. Used internally by `SessionHost` and `DirectRenderHost`;
apps issue `sg_*` draw calls through the `ge::Pass` RAII surface.

### Wire protocol — `include/ge/Protocol.h`

Header-only. Key constants: `kProtocolVersion` (bump on breaking changes), magic numbers `kXxxMagic`,
and wire structs `DeviceInfo`, `SessionConfig`, `SafeAreaUpdate`, `AspectLock`, `MessageHeader`.

```cpp
// Example: checking protocol version on player connect
if (header.version != wire::kProtocolVersion) { /* reject */ }
```

### `Tweak<T>` — `include/ge/Tweak.h`

Runtime-tunable parameters. Declare at file scope; spyder's app-channel / dashboard discovers them automatically.

```cpp
static tweak::Tweak<float> speed("buggy.speed", 5.0f);
// Read anywhere: *speed  or  speed.get()
```

Specialized variants: `EnumTweak` (dropdown), `Vec2Tweak`, `AxisTweak`, `Color`. Use
`tweak::loadOverrides(db)` at startup to reapply saved values.

### SVG rendering and measurement — `include/ge/svg.h`

`ge::rasterizeSvg` and `ge::rasterizeSvgToPixels` render SVG strings through
lunasvg, including text when the relevant font faces have been registered with
`ge::registerSvgFontFace`. `ge::renderSvgDocument` renders an existing
`lunasvg::Document`, which is the right shape for interactive SVG controls that
mutate attributes or styles before re-rendering.

`ge::measureSvgBounds` and `ge::measureSvgElementBounds` return post-layout
`SvgBounds` for a whole document or an element id without rasterizing a texture.
Use these for SVG-backed controls whose layout depends on rendered text width:
register the font, load or pass the SVG, then measure the `<text id="...">` or
containing button group. The bounds come from lunasvg's own layout and text
metrics, so they stay consistent with the pixels `rasterizeSvg` will later draw.

### Dev-time log streaming over the app-channel — `include/ge/log.h` (🎯T119)

Apple's unified-logging path (DTX over the RSD tunnel, DDI mount, Developer Mode,
the `os_trace_relay` fallback that silently drops third-party app emissions) has
enough failure modes that you can't rely on it as your primary on-device
diagnostic channel — and `NSLog` is just `os_log` underneath since iOS 14, so it
inherits the same gates. The escape hatch: ge streams every `SPDLOG_INFO/WARN/
ERROR` to spyder over the **app-channel**. `ge::log::install()` (called
automatically from `ge::run`) attaches a structured-log sink whenever
**`SPYDER_APP_CHANNEL=host:port`** is set, so logs ride the same MessagePack RPC
channel as everything else — no Apple-log or `adb logcat` dependency. (Spyder
v0.58.0 removed the old plain-text `log_collect` listener; the app-channel is now
the only log path.)

**Let spyder be the listener.** `app_channel_start` opens a fresh kernel-assigned
port and reports your machine's LAN-reachable `hosts`, so there's no port to pick.
Hand one `host:port` to `SPYDER_APP_CHANNEL` in the launch env, then drain the
structured `{ts, level, subsystem, format}` log pushes with `app_log_get`:

```jsonc
// 1. Open the app-channel listener (spyder picks the port + reports LAN IPs).
app_channel_start { "owner": "tiltbuggy" }
//   → { "listener_id": "…", "port": 49546, "hosts": ["192.168.1.42", …] }

// 2. Launch/deploy with SPYDER_APP_CHANNEL pointing at one of those host:port
//    pairs (a LAN host on device; 127.0.0.1 also works on the iOS Simulator):
deploy_app { "device": "Jevons", "path": "…/TiltBuggy.app",
             "env": { "SPYDER_APP_CHANNEL": "192.168.1.42:49546" } }

// 3. Drain the structured log pushes (capture continues until the app exits):
app_log_get { "session_id": "…" }
```

The wire is MessagePack, not plain text, so a bare `nc` listener no longer works —
spyder (or another app-channel host) must be on the other end. Per-platform launch
env:

```bash
SPYDER_APP_CHANNEL=127.0.0.1:49546 bin/tiltbuggy            # desktop

# iOS Simulator (shares host loopback):
SIMCTL_CHILD_SPYDER_APP_CHANNEL=127.0.0.1:49546 \
    xcrun simctl launch --terminate-running-process booted com.squz.tiltbuggy
# iOS device / Xcode: add SPYDER_APP_CHANNEL=<mac-LAN-ip>:49546 to the scheme env.

# Android: env vars don't reach Activity-launched apps, so spyder passes them as
# Intent string-extras (`am start --es`) and ge.GeActivity bridges each into the
# process env via setenv before the native thread starts (🎯T119) — getenv() then
# works the same as on iOS. launch_app / deploy_app env-passthrough does this:
deploy_app { "device": "Pixel", "path": "app-debug.apk",
             "env": { "SPYDER_APP_CHANNEL": "192.168.1.42:49546" } }
```

(The env arrives on *that* launch — a user-tap relaunch from SpringBoard / the
launcher loses it; re-run `launch_app` / `deploy_app` with the same env to resume.)

**iOS local-network prompt (one-time).** The first dial on a physical iOS device
trips the system "find and connect to devices on your local network" prompt; tap
*Allow* once per (device, app) and the grant persists across launches.

**Compile-gated behind `#ifndef NDEBUG`** — the whole feature (the `getenv`, the
sink, the channel) is compiled out of release builds, so a misconfigured
TestFlight / Play Store binary can never phone home to a developer's LAN; the
Android `setenv` bridge is likewise a release no-op, so a shipped app ignores
launch-Intent extras entirely. The runtime gate (`SPYDER_APP_CHANNEL` unset → no
sink) means even a debug build only connects when explicitly told where. See
`AppChannelLogSink` in `src/log.cpp` and the channel in `src/appchannel.cpp`.

### Agent-drivable app channel — `include/ge/appchannel.h` (🎯T92)

The single dev channel to spyder's `app_*` MCP tools: a bidirectional
MessagePack-RPC connection that carries logs, perf, state, and control over one
socket, so **every ge app is agent-drivable by default** — pause it, single-step
it, inject a tap, query its state, grab a screenshot, quit it cleanly, drain its
logs/perf — without per-app plumbing. Pairs with spyder ≥ v0.53.0 (the channel
host; ≥ v0.59.0 for the `SPYDER_APP_CHANNEL` env name).

**Activation — `SPYDER_APP_CHANNEL`.** Set `SPYDER_APP_CHANNEL=host:port` and
`ge::run` dials the RPC channel automatically (🎯T119 — there is no separate
text-log path or `appchannel://` scheme any more; the app-channel is the one
connection).

```jsonc
// spyder is the listener:
app_channel_start { "owner": "tiltbuggy" }
//   → { "listener_id": "…", "port": 49546, "hosts": ["192.168.1.42", …] }

// Launch with SPYDER_APP_CHANNEL=<host>:<port>:
//   desktop : SPYDER_APP_CHANNEL=127.0.0.1:49546 bin/tiltbuggy
//   iOS sim : SIMCTL_CHILD_SPYDER_APP_CHANNEL=127.0.0.1:49546 xcrun simctl launch …
//   iOS dev : SPYDER_APP_CHANNEL=<mac-LAN-ip>:49546 in the Xcode scheme env
//   Android : spyder passes it as an `am start --es` Intent extra; ge.GeActivity
//             setenv-bridges it (🎯T119). By hand: launch_app with env={…}.

app_channel_list   // → session with app_name / app_version / advertised methods
app_ping           // round-trip liveness (the app echoes its wall-clock ts)
```

**Connect + hello handshake.** On connect the app sends a `hello` request
advertising `{app_name, app_version, methods, slices}` — the method names it has
handlers for and the state slices consumers registered — and awaits spyder's
`{spyder_version}` ack before the channel goes live. Framing is length-prefixed
MessagePack: `[4-byte LE length][MessagePack body]` (≤ 16 MB), with a
JSON-RPC-shaped envelope — `{id, method, params}` requests either direction,
`{id, result|error}` responses, `{method, params}` async pushes. Encode/decode is
`nlohmann::json::to_msgpack`/`from_msgpack` (already vendored — no hand-rolled
MessagePack, no new dependency). A background receiver thread owns socket reads +
request dispatch; a background sender thread drains queued pushes — **the game
thread never blocks on the socket.**

**ge-owned methods** (registered in `appchannel.cpp`'s `registerBuiltins`):

| Method | spyder tool | Effect |
|---|---|---|
| `ping` | `app_ping` | echoes the app's wall-clock `ts` |
| `quit` | `app_quit` | `SDL_PushEvent(SDL_EVENT_QUIT)` → clean exit 0, no macOS crash dialog |
| `flush` | `app_flush` | drains the push sender, then acks (precondition for quit) |
| `backgrounded` / `foregrounded` | `app_background` / `app_foreground` | post the SDL lifecycle events (T7/T88 audio + render gating) |
| `low_memory_warning` | `app_low_memory` | fires `onMemoryWarning(Critical)` via the same atomic the iOS observer / Android `onTrimMemory` uses |
| `pause` / `resume` / `step` / `speed` | `app_pause` / `app_resume` / `app_step` / `app_speed` | time-control: `dt`→0 (render+input continue), restore, advance N frames at a fixed `kStepDt` then re-pause, `dt` multiplier |
| `input_inject` | `app_input` | fabricate + `SDL_PushEvent` a `finger_down/up/motion` (normalized 0–1), `key_down/up`, or `accel` event — lands in `onEvent` indistinguishably from real OS input |
| `state_query` | `app_state` | return a consumer-registered state slice (run on the game thread) |
| `save_state` / `restore_state` | `app_save_state` / `app_restore_state` | round-trip a consumer snapshot; ge MessagePack-encodes it into a `state` bin, spyder base64s into `{state_b64, size}` |
| `screenshot_app` | `app_screenshot` | framebuffer readback → PNG `{format, width, height, data:<bin>}` |
| `metrics_list` / `metrics_arm` / `metrics_disarm` / `metrics_status` / `metrics_dump` | `app_metrics_*` | 🎯T166 per-instance frame-trace ring: list series, arm selected names + capacity, dump full retained history (not latest-only `perfEmit`) |

Time-control rides the run loop via `ge::appchannel::applyTimeControl(dt)` (the
real frame `dt` in, the effective `dt` for `onUpdate` out). `app_pause` keeps
render + input alive, so `app_input` / `app_state` / `app_screenshot` while paused
are **state-correlated** — the killer combo for visual debugging.

**Per-instance metrics DX** (`<ge/metrics.h>`, 🎯T166) — one `Scope` per game
instance; typed producers assign each frame (no-op when unarmed; zero I/O):

```cpp
#include <ge/metrics.h>
struct App {
    ge::metrics::Scope metrics;
    ge::metrics::metric<float> dt{metrics, "dt"};
    ge::metrics::metric<float> zoom{metrics, "zoom"};
    void update(float frameDt) {
        dt = frameDt;
        zoom = cameraZoom;
        metrics.endFrame();
    }
};
// spyder: app_metrics_arm { series: ["dt","zoom"], capacity: 3600 }
//         … drive UX …
//         app_metrics_dump  → { series, frames: [[…], …], count }
```

**Consumer-cooperative surface** (register *before* `ge::run` so it's advertised
in the hello; getters run on the game thread, marshalled by the run loop's
`pumpMainThreadTasks`, so they never tear against the simulation):

```cpp
#include <ge/appchannel.h>

// Queryable read-only slices (spyder app_state{slice}):
ge::appchannel::registerStateSlice("scene", [&]{
    auto p = state.scene->buggyPose();
    return nlohmann::json{{"buggy", {{"x", p.x}, {"y", p.y}}}};
});

// Whole-app save/restore (save returns a JSON snapshot; restore gets it back):
ge::appchannel::registerStateSerializer(
    [&]{ return nlohmann::json{{"pro", ge::iap::owned("pro")}}; },
    [&](const nlohmann::json& j){ ge::iap::testing::setOwned("pro", j.value("pro", false)); });

// Perf counters — emit each frame; ride alongside frame_ms in the ~1 Hz perf push:
ge::appchannel::perfEmit("buggy_x", p.x);
```

The push half carries dev logs + perf over the same channel: a typed `log` push
(`{ts, level, subsystem, format}`) drains via `app_log_get`, and a periodic `perf`
push (`{ts, samples:{frame_ms, …counters}}`, ~1 Hz) drains via `app_perf_get`.
(`ts` is the on-wire field name spyder's `LogPush` / `PerfPush` decode.)

#### State slices — the telemetry / metadata pipe (🎯T115)

The state registry **is** ge's blessed telemetry/metadata contract: the app
*defines* named slices of its own state, spyder *reads* them — no bespoke
transport per game. Slices are entirely app-defined; ge hard-codes none. Register
any number, with any JSON shape, before `ge::run`:

```cpp
ge::appchannel::registerStateSlice("geometry", [&]{ return /* any json */; });
ge::appchannel::registerStateSlice("hud",      [&]{ return /* any json */; });
```

ge advertises the registered names in the `hello`, and three spyder tools drive
the pipe — **no new app-side method is needed beyond `state_query`** (spyder's
capture is a spyder-side poller of it, mirroring `app_log_get` / `app_perf_get`):

| spyder tool | What it does |
|---|---|
| `app_state_slices` | discover the slice names the app advertised in `hello` |
| `app_state {slice}` | pull one slice now (runs the getter on the game thread — a consistent, non-torn snapshot) |
| `app_state_capture_start {slice, interval_ms}` → `app_state_capture_get` / `_stop` | **watch a slice evolve**: spyder polls `state_query` on a background interval (default 100 ms, min 10 ms) into a timestamped buffer, so an agent can run an `app_input` sequence and read state frame-by-frame without a hand-rolled poll loop |
| `app_state_describe {slice}` | one `state_query` walked into a types-only sketch (`{"bodies": [{"vel": ["float"]}]}`) — learn a slice's shape without ingesting the full payload (spyder ≥ v0.57.0; app-agnostic) |

Every readout tool also takes an optional `select` jq expression that spyder
evaluates **server-side** (gojq) and returns only the filtered result — e.g.
`app_state{slice:"geometry", select:".bodies[0].vel"}`. The jq engine lives
entirely in spyder; ge embeds no filter logic.

Because `app_pause` keeps render + input alive, `app_input → app_state` /
`app_state_capture` while paused is **state-correlated** — drive an input, watch
the exact state it produced.

**Volunteering an example (🎯T116, optional).** `registerStateSlice` takes an
optional third argument — a representative example payload — that ge advertises
in the `hello` as a `{name, example}` slice descriptor (spyder ≥ v0.57.0). A
connected agent then gets a filter-writing template the moment it lists slices,
skipping a `state_query` round-trip:

```cpp
ge::appchannel::registerStateSlice("geometry", geometryGetter,
    nlohmann::json{{"units", "metres"},
                   {"bodies", {{{"id","buggy"}, {"pos",{0,0}}, {"vel",{0,0}}, {"angle",0}}}}});
```

Keep it a **one-line snapshot of the shape**, not a multi-screen literal — its
job is to show the keys and value types, not the live data. Slices that omit it
still emit the compact bare-string form, and the agent can fall back to
`app_state_describe`. The example is captured at registration (before `ge::run`),
so it can be a hand-written literal or the getter's initial output.

**Recommended schema convention — `hit_targets` (🎯T109).** Built-in app-channel
slice (registered by `installFromEnv`). Spyder scripts list → find by **id/role**
→ inject at centre without hard-coded coordinates or host CV.

**Buttons (engine path):** put sparse automation metadata on `ge::Button` (visuals
stay separate). `id` empty → not exported. Use `setHitRect(r)` so hit-test and
export bbox stay the same rect; `publishHitTarget(&btn)` once (owner lifetime).

```cpp
btn.id = "restart";
btn.role = "reset";
btn.setHitRect(expandedRect);   // hitTest + hitBounds
btn.onFire = …;
ge::publishHitTarget(&btn);     // once; unpublish in destructor
// SessionHost sets surface size each frame → center_norm / bbox_norm
```

**Non-button regions:** `ge::appchannel::setExtraHitTargets([{id, kind, role, bbox, …}])`
(same field shape; replaced wholesale each call).

Canonical samples: `sample/tiltbuggy` title (`id`/`role` = `reset`) + `playfield`
region; multimaze2 `TopBar` back/restart/home via the same Button path.

```jsonc
{
  "targets": [
    {
      "id": "reset",           // required stable key (prefer over label)
      "kind": "button",        // button | region | svg | …
      "role": "reset",         // optional semantic role
      "label": "TiltBuggy",    // display only — not for addressing
      "enabled": true,
      "space": "pts",          // units for raw bbox (pts y-down full surface)
      "bbox": [x, y, w, h],
      "bbox_norm": [x, y, w, h],
      "center_norm": [cx, cy]  // preferred for app_input
    }
  ]
}
```

Spyder host: `find_hit_target` + `resolve_target` + recipe `l1_tap_hit_target`
(see spyder `agents-guide.md` § Hit targets).

**Recommended schema convention — geometry / physics slices.** So spyder can
render and compare physics state uniformly *across* games, a slice that exposes
geometry should follow this shape (all positions in the game's world units; state
the unit). It is a **convention, not a requirement** — an app is free to ignore
it; it just won't get uniform cross-game tooling.

```jsonc
{
  "units": "metres",
  "bodies": [                         // dynamic + relevant static bodies
    { "id": "buggy", "pos": [x, y], "vel": [vx, vy], "angle": rad }
  ],
  "constraints": [                    // joints / springs / walls, if any
    { "id": "axle", "rest": L0, "current": L }
  ],
  "sensors": [                        // proximity / triggers, if any
    { "id": "goal", "distance_to_nearest": d }
  ],
  "bounds": { "min": [x, y], "max": [x, y] }   // optional world extent
}
```

`bodies` is the only near-universal key; `constraints` / `sensors` appear only
when the game has them.

**box2d consumers get the `bodies` array for free (🎯T117).**
`ge::box2d::worldGeometry(worldId)` (header-only `<ge/box2d_slice.h>`) walks every
body/shape in a `b2World` via `b2World_OverlapAABB` and emits this schema directly
— box2d body names (`b2BodyDef.name`) become the `id`s, each body's shape outlines
ride under a `shapes` key. Drop it into a getter and add the bits box2d can't know
(`units`, `bounds`, app-specific `constraints`/`sensors`):

```cpp
ge::appchannel::registerStateSlice("geometry", [&]{
    auto g = ge::box2d::worldGeometry(state.scene->worldId());
    g["units"] = "metres";
    return g;                                   // or ge::box2d::body(label, id) for one
});
```

`sample/tiltbuggy`'s `geometry` slice is wired this way — its named `arena`
(walls + surface patches) and `buggy` bodies. Validated end-to-end against spyder
v0.57.0 on desktop: `app_state{geometry, select:'.bodies[]|select(.id=="buggy")'}`
returns the buggy's live `pos`/`vel`/`angle`/`shapes` directly — no screenshot, no
pixel parsing.

**iOS gotcha — local-network permission (one-time).** On a physical iOS device
the app dials spyder over the LAN, which trips iOS's Local Network privacy prompt
on **first launch after install**. Until the user taps *Allow* (or it's
pre-granted via `NSLocalNetworkUsageDescription` + a settings toggle), the
app-channel connection silently fails and no session appears in
`app_channel_list`. Accept the prompt once; the grant persists across launches.
Simulator and desktop are unaffected. (Point `SPYDER_APP_CHANNEL` at the Mac's LAN
IP, not `127.0.0.1`, for a device.)

**Per-platform screenshot readback** (`SokolContext::captureNextFrame`, a one-shot
sink fired inside `endFrame` after the GPU render): Apple blits the drawable on
**sokol's own command queue** (`sg_mtl_command_queue`, so the copy is ordered after
the render — no cross-queue race) into a Shared texture, then `getBytes`; dev
builds set `framebufferOnly = NO` so the drawable is a readable blit source.
Android `glReadPixels` after `sg_commit`, before the buffer swap, rows flipped to
top-down. Both deliver RGBA8; the worker thread PNG-encodes (`IMG_SavePNG_IO`) off
the render loop. The handler blocks (≤ 2 s) until `DirectRenderHost::endFrame`
services the capture — see `src/render/ScreenshotBridge.h`.

**Compile-gated behind `#ifndef NDEBUG`** — same gate as T83. In a release build
there is no socket, no msgpack, no handler table, and the public functions are
empty no-ops, so a store binary can never expose a control channel.

Verified end-to-end against spyder v0.53.0 on macOS desktop:
`app_ping → app_pause → app_input(tap) → app_state → app_screenshot → app_quit`
plus `app_log_get` / `app_perf_get`. The Apple Metal readback path is shared by
iOS; the Android GLES path is `glReadPixels`-based.

## Common patterns

### Adding a new wire message type

1. Add a magic constant in `include/ge/Protocol.h` and increment `kProtocolVersion`.
2. Add the wire struct (POD, packed) in `Protocol.h`.
3. Implement TX in `src/bridge/ServerWireBridge.mm` (or `SessionHost.mm` for session-level messages).
4. Implement RX in `tools/player_core.cpp` (or `src/bridge/PlayerWireBridge.cpp`).
5. Update `CLAUDE.md`'s protocol table.

### Adding a new public header

1. Create `include/ge/ClassName.h`.
2. Create `src/ClassName.cpp` (or `.mm` for ObjC++).
3. Add to `ge/SRC` in `Module.mk`.
4. Use pImpl for classes that pull in sokol/SDL/asio headers (keeps consuming app compile times sane).
5. Update `CLAUDE.md` §"Public API".

### Adding a matrix-test cell

Cells are enumerated in `ge/CELLS` inside `Module.mk`. Add a line there, then implement
`run_<cell>_*` sub-checks in `tools/matrix-cell.sh`. The Make rule is auto-generated from the name.

### Mobile project regeneration

**Never hand-edit `sample/*/ios/` or `sample/*/android/`.** They are regenerated from templates:

```bash
# Regenerate from templates (destructive — commits ios/ or android/ first)
make ge/ios-init   APP_ID=com.squz.tiltbuggy APP_NAME=TiltBuggy IOS_DEVELOPMENT_TEAM=XXXXXX
make ge/android-init APP_ID=com.squz.tiltbuggy APP_NAME=TiltBuggy
```

Template sources live in `tools/ios-template/` and `tools/android-template/`. All template changes
go there.

## Gotchas

- **The bgfx fork at `squz/bgfx` is historical only.** ge migrated off bgfx in 🎯T38. The
  vendored `vendor/github.com/bkaradzic/` tree is retained as a reference but is no longer
  compiled into `libge.a`. Do not reintroduce bgfx dependencies.

- **Android dual-renderer: Vulkan preferred, GLES3 fallback.** `SokolContext_android.cpp`
  runtime-selects via `ge_vk_probe`. Vulkan on emulators and capable real devices; GLES3 on
  devices that fail the probe. The old bgfx Vulkan path that silently stalled on Adreno 830 is
  gone — see `docs/papers/adreno-830-bgfx-vulkan-crash.md` (historical) for background.

- **iOS / iPad orientation lock needs BOTH `Info.plist` and `SessionHostConfig.orientation`.**
  iPadOS 26+ ignores plist orientation alone (multitasking treats every iPad app as resizable).
  Narrow `UISupportedInterfaceOrientations` *and* set `SessionHostConfig.orientation` non-zero
  so the engine activates the `prefersInterfaceOrientationLocked` swizzle (Apple TN3192). The
  swizzle's library file is bundled into `libge` from v0.3.0; consumers don't need to add it
  to their build. The specific `wire::kOrientation*` value is currently a boolean
  ("lock yes/no") — *which* orientation gets locked is the plist's job. See `CLAUDE.md`'s
  "iOS orientation lock (iPadOS 26+)" section for the full background.

- **`stream_addr` is consumed-once on Android.** Pass it via `--es stream_addr "host:port"` (legacy: `ged_addr`) to
  `am start`. Subsequent launches without the flag fall through to QR scan. The matrix-cell harness
  passes it automatically; manual `adb shell am start` invocations must include it explicitly.

- **iOS launch param is `-stream_addr "host:port"`** (legacy: `-ged_addr`). Example:
  `xcrun simctl launch booted <bundle-id> -stream_addr localhost:3030`.

- **Physical-device rotation is not automatable.** `matrix-cell.sh` skips the rotation round-trip
  sub-check for `ios-device-*` and `android-device-*` cells; it runs only on simulators/emulators.

- **BSD `sed` on macOS.** All shell scripts must use POSIX character classes (`[[:space:]]` not `\s`,
  `[[:digit:]]` not `\d`). Never introduce GNU-only sed syntax.

- **`ios_sim_is_running` uses retry loops.** iOS drops the app from launchctl mid-orientation
  transition; a single-snapshot check yields false "dead" reports. The smoke-test and matrix-cell
  scripts already handle this — don't short-circuit them.

- **`make -j` is not set in parent Makefiles.** Projects that need parallel builds set `MAKEFLAGS`
  in their own Makefile. Never pass `-j` on the command line.

- **Use spyder, not ged.** The historical `ged` daemon and `-no-open` dashboard flag are gone.
  Start `spyder serve` (or `brew services start spyder`) for the control plane / stream relay.

- **SQLite3 is compiled into `libge.a`.** It comes from `vendor/src/sqlite3.c`. Never add `-lsqlite3`
  to link lines — it's already included and double-linking produces duplicate-symbol errors.

## Smoke testing

After building and deploying, use `tools/smoke-test.sh` before asking a human for visual feedback:

```bash
ge/tools/smoke-test.sh --platform ios-sim --device tablet
ge/tools/smoke-test.sh --platform android-emu --package com.spyder.player
ge/tools/smoke-test.sh --platform desktop
```

With `--install <path>` it performs an atomic terminate → install → launch → verify cycle. Always
prefer `--install` after a rebuild to avoid debugging stale binaries. Only escalate to asking a
human once the smoke test passes and the problem is still unclear.

## See also

- `README.md` — end-user overview.
- `CLAUDE.md` — authoritative rules for agentic contributors.
- `STABILITY.md` — public API stability catalogue and pre-1.0 gaps.
- `docs/targets.yaml` / `docs/targets.md` — active convergence targets (managed via bullseye).
