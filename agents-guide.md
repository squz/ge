# ge agent guide

**ge** is a reusable C++ rendering and streaming engine built on bgfx + SDL3, consumed as a git
submodule via `-include $(ge)/Module.mk`. It targets pre-1.0, so the interaction surface is still
settling — check `STABILITY.md` for stability annotations before modifying public headers.

The engine supports two operating modalities. In **brokered (server/player) mode** the app renders
headless into a bgfx framebuffer, encodes frames as H.264 (VideoToolbox on Apple, FFmpeg on
Android), and transmits them to a player over a WebSocket managed by the **ged** daemon. The player
decodes the stream, displays it in an SDL window, and relays input back to the server over the same
channel. In **direct mode** the app owns a real SDL window; bgfx draws straight to it with no
encoding and no ged involvement. Both modes share the same `ge::run()` entry point — the `RenderHost`
abstraction hides which modality is active.

`ge::run()` blocks until SIGINT or the last session ends. On each new player connect, it calls a
user-supplied **factory** with a `ge::Context` (dimensions, device class, engine DB), which returns a
`ge::RunConfig` (four `std::function` callbacks: `onUpdate`, `onRender`, `onEvent`, `onShutdown`).
State that must survive player reconnects lives outside the factory; per-session resources are
created inside it.

## When to change what

**ge/** is for reusable engine code — protocol, rendering infrastructure, bgfx lifecycle, player
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
| `ged/` | Go daemon (broker, dashboard, MCP server) |
| `web/` | ged React/Vite dashboard |
| `vendor/github.com/bkaradzic/{bgfx,bx,bimg}/` | bgfx libraries (vendored) |

### Key interfaces

- **`RenderHost`** (`include/ge/RenderHost.h`) — abstract boundary between engine and render.
  Concrete implementations: `DirectRenderHost` (direct mode) and `ServerWireBridge` (brokered mode).
- **`PlayerWireBridge`** — player-side counterpart of `ServerWireBridge`. Wraps a `DirectRenderHost`,
  intercepts events for wire transmission, and feeds decoded frames as textures.
- **`SessionHost`** / `ge::run()` — server-side lifecycle: bgfx init, ged sideband connection,
  per-session factory invocation, frame loop, graceful shutdown.

### bgfx backends

- **Apple (macOS, iOS)**: Metal via `CAMetalLayer`.
- **Android**: Vulkan (`bgfx::RendererType::Vulkan`). Not GLES — the Apple EGL translator caps at
  GLES 3.0, which breaks shaderc on modern Adreno 830 AVDs.

### ged daemon

Broker between servers and players. Manages WebSocket routing, QR codes, session assignment, server
supersede (SIGINT to old server on name collision), and the React dashboard. Exposes an MCP server
at `/mcp` (streamable HTTP). Launch with `bin/ged`; use `-no-open` to suppress browser auto-open.

## Build and run

All recipes assume the consuming app's Makefile includes `ge/Module.mk` and the standard targets are
present. For `sample/tiltbuggy/`, run all commands from that directory.

```bash
# Engine and app
make                        # build app binary
make run                    # build + run desktop direct mode
make ge/player              # build desktop player binary (bin/player)
make ged && bin/ged         # build + start the daemon

# Brokered mode (three terminals)
bin/ged -no-open &          # terminal 1: daemon
bin/<app>                   # terminal 2: game server
bin/player                  # terminal 3: desktop player

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

# (The brokered ge player iOS/Android make targets were retired in
#  🎯T73.3; the player is dormant pending 🎯T34's rewrite of it as a
#  regular ge app.)

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

Main entry point. Connects to ged, spawns a session per attaching player, calls `factory(ctx)` for
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

### `BgfxContext` — `include/ge/BgfxContext.h`

RAII bgfx device lifecycle. Handles Metal (Apple) and Vulkan (Android) init, frame begin/end, and
headless vs. windowed mode. Used internally by `SessionHost`; apps submit bgfx draw calls directly.

### Wire protocol — `include/ge/Protocol.h`

Header-only. Key constants: `kProtocolVersion` (bump on breaking changes), magic numbers `kXxxMagic`,
and wire structs `DeviceInfo`, `SessionConfig`, `SafeAreaUpdate`, `AspectLock`, `MessageHeader`.

```cpp
// Example: checking protocol version on player connect
if (header.version != wire::kProtocolVersion) { /* reject */ }
```

### `Tweak<T>` — `include/ge/Tweak.h`

Runtime-tunable parameters. Declare at file scope; the ged dashboard discovers them automatically.

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

### Dev-time log streaming over TCP — `include/ge/log.h` (🎯T83)

Apple's unified-logging path (DTX over the RSD tunnel, DDI mount, Developer Mode,
the `os_trace_relay` fallback that silently drops third-party app emissions) has
enough failure modes that you can't rely on it as your primary on-device
diagnostic channel — and `NSLog` is just `os_log` underneath since iOS 14, so it
inherits the same gates. The escape hatch every game dev already knows: open a
socket. `ge::log::install()` (called automatically from `ge::run`) appends a TCP
sink to the default logger when the `LOG_TARGET=host:port` convention is set, so
**every `SPDLOG_INFO/WARN/ERROR` is also streamed to that host** — no Apple-log or
`adb logcat` dependency in the path.

**Preferred: let spyder be the listener (v0.51.0+).** `log_collect_start` opens a
fresh kernel-assigned port and returns the LAN-reachable `hosts` of your machine,
so there's no port to pick and no `ipconfig getifaddr en0` guesswork. Hand one
`host:port` to `LOG_TARGET` in the launch env, then drain with `log_collect_get`:

```jsonc
// 1. Open a listener (spyder picks the port + reports your LAN IPs).
log_collect_start { "owner": "tiltbuggy" }
//   → { "session_id": "ab12…", "port": 54321, "hosts": ["192.168.1.42", …] }

// 2. Launch/deploy with LOG_TARGET pointing at one of those host:port pairs.
//    On a device use a LAN host; on the iOS Simulator 127.0.0.1 also works.
deploy_app { "device": "Jevons", "path": "…/TiltBuggy.app",
             "env": { "LOG_TARGET": "192.168.1.42:54321" } }

// 3. Read what's arrived (capture continues); tear down when done.
log_collect_get  { "session_id": "ab12…" }
log_collect_stop { "session_id": "ab12…" }
```

spyder owns the port-per-session bookkeeping, reconnect handling, and a bounded
buffer; ge just streams newline-delimited text to it. (Caveat: the env arrives on
*that* launch — a user-tap relaunch from SpringBoard / the launcher loses it;
re-run `launch_app` with the same env to resume.)

**Fallback: a bare `nc` listener** (no spyder — desktop dev, Xcode-scheme env, or
`adb` by hand):

```bash
nc -l 9999                                    # on your Mac
LOG_TARGET=127.0.0.1:9999 bin/tiltbuggy       # desktop

# iOS Simulator (shares host loopback):
SIMCTL_CHILD_LOG_TARGET=127.0.0.1:9999 \
    xcrun simctl launch --terminate-running-process booted com.squz.tiltbuggy
# iOS device / Xcode: add LOG_TARGET=<mac-LAN-ip>:9999 to the scheme env.

# Android apps aren't shell-launched, so env vars don't reach them. spyder's
# launch env-passthrough sets LOG_TARGET for you; by hand, use a system property
# (no Java/Intent plumbing — ge's resolveLogTarget reads it too):
adb reverse tcp:9999 tcp:9999                  # device localhost → your Mac
adb shell setprop debug.ge.log_target 127.0.0.1:9999
adb shell am start -n <pkg>/ge.GeActivity
```

**Compile-gated behind `#ifndef NDEBUG`** — the entire feature (the `getenv`, the
sink, the sender thread) is compiled out of release builds, so a misconfigured
TestFlight / Play Store binary can never phone home to a developer's LAN. The
runtime gate (target unset → no sink) means even a debug build only opens a socket
when explicitly told where to point.

Implementation note: the sink formats on the calling thread and hands lines to a
dedicated sender thread via a bounded queue, reconnecting with exponential backoff.
A downed listener costs nothing on the render hot path; a log flood drops oldest
lines past the queue cap rather than growing memory. See `NetworkLogSink` in
`src/log.cpp`.

### Agent-drivable app channel — `include/ge/appchannel.h` (🎯T92)

The structured sibling of the T83 text sink: a bidirectional MessagePack-RPC
channel to spyder's `app_*` MCP tools, so **every ge app is agent-drivable by
default** — pause it, single-step it, inject a tap, query its state, grab a
screenshot, quit it cleanly, drain its logs/perf — without per-app plumbing.
Pairs with spyder ≥ v0.53.0 (the channel host).

**Activation — same `LOG_TARGET`, a scheme discriminator.** A value of
`appchannel://host:port` dials the RPC channel; a bare `host:port` keeps the T83
text `NetworkLogSink` unchanged (fully backwards-compatible). One env var, the
URL scheme picks the protocol. `ge::run` dials it automatically.

```jsonc
// spyder is the listener:
app_channel_start { "owner": "tiltbuggy" }
//   → { "listener_id": "…", "port": 49546, "hosts": ["192.168.1.42", …] }

// Launch with LOG_TARGET=appchannel://<host>:<port>:
//   desktop : LOG_TARGET=appchannel://127.0.0.1:49546 bin/tiltbuggy
//   iOS sim : SIMCTL_CHILD_LOG_TARGET=appchannel://127.0.0.1:49546 xcrun simctl launch …
//   iOS dev : LOG_TARGET=appchannel://<mac-LAN-ip>:49546 in the Xcode scheme env
//   Android : adb reverse tcp:49546 tcp:49546
//             adb shell setprop debug.ge.log_target appchannel://127.0.0.1:49546

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

Time-control rides the run loop via `ge::appchannel::applyTimeControl(dt)` (the
real frame `dt` in, the effective `dt` for `onUpdate` out). `app_pause` keeps
render + input alive, so `app_input` / `app_state` / `app_screenshot` while paused
are **state-correlated** — the killer combo for visual debugging.

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

The push half supersedes the T83 text emission in `appchannel://` mode: a typed
`log` push (`{timestamp, level, subsystem, format}`) drains via `app_log_get`, and
a periodic `perf` push (`{timestamp, samples:{frame_ms, …counters}}`, ~1 Hz) drains
via `app_perf_get`. Bare `host:port` keeps the text sink.

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
4. Use pImpl for classes that pull in bgfx/SDL/asio headers (keeps consuming app compile times sane).
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

- **bgfx fork lives at `squz/bgfx`, branch `ge-fork-upgrade`.** Three commits ahead of upstream
  (mobile-crashes guard, drawable-as-truth patch, shaderc CMake build). Don't rebase onto upstream
  without verifying all three patches still apply.

- **Android dual-renderer: Vulkan on emulator, OpenGL ES 3.1 on real devices.** From v0.2.0,
  bgfx compiles both backends and `BgfxContext.mm` runtime-selects via `ro.kernel.qemu` /
  `ro.hardware`. Vulkan on the Apple-Silicon AVD (its EGL translator caps at GLES 3.0; a 3.1
  request trips `EGL_BAD_CONFIG`); OpenGL ES on real Adreno/Mali devices (the Vulkan path
  silently stalls after the first present — see `docs/papers/adreno-830-bgfx-vulkan-crash.md`).
  Single-threaded mode (`BGFX_CONFIG_MULTITHREADED=0`) is non-negotiable on Android so the
  swap-chain teardown on background can be gated by SDL3's `WINDOW_FOCUS_LOST` handler.

- **iOS / iPad orientation lock needs BOTH `Info.plist` and `SessionHostConfig.orientation`.**
  iPadOS 26+ ignores plist orientation alone (multitasking treats every iPad app as resizable).
  Narrow `UISupportedInterfaceOrientations` *and* set `SessionHostConfig.orientation` non-zero
  so the engine activates the `prefersInterfaceOrientationLocked` swizzle (Apple TN3192). The
  swizzle's library file is bundled into `libge` from v0.3.0; consumers don't need to add it
  to their build. The specific `wire::kOrientation*` value is currently a boolean
  ("lock yes/no") — *which* orientation gets locked is the plist's job. See `CLAUDE.md`'s
  "iOS orientation lock (iPadOS 26+)" section for the full background.

- **`ged_addr` is consumed-once on Android.** Pass it via `--es ged_addr "host:port"` to
  `am start`. Subsequent launches without the flag fall through to QR scan. The matrix-cell harness
  passes it automatically; manual `adb shell am start` invocations must include it explicitly.

- **iOS launch param is `-ged_addr "host:port"`.** Add to `simctl launch` or `devicectl` args:
  `xcrun simctl launch booted <bundle-id> -ged_addr localhost:42069`.

- **Physical-device rotation is not automatable.** `matrix-cell.sh` skips the rotation round-trip
  sub-check for `ios-device-*` and `android-device-*` cells; it runs only on simulators/emulators.

- **BSD `sed` on macOS.** All shell scripts must use POSIX character classes (`[[:space:]]` not `\s`,
  `[[:digit:]]` not `\d`). Never introduce GNU-only sed syntax.

- **`ios_sim_is_running` uses retry loops.** iOS drops the app from launchctl mid-orientation
  transition; a single-snapshot check yields false "dead" reports. The smoke-test and matrix-cell
  scripts already handle this — don't short-circuit them.

- **`make -j` is not set in parent Makefiles.** Projects that need parallel builds set `MAKEFLAGS`
  in their own Makefile. Never pass `-j` on the command line.

- **`bin/ged -no-open`** prevents the dashboard from opening in the browser on first server connect.
  Always use this flag in automated/CI contexts.

- **SQLite3 is compiled into `libge.a`.** It comes from `vendor/src/sqlite3.c`. Never add `-lsqlite3`
  to link lines — it's already included and double-linking produces duplicate-symbol errors.

## Smoke testing

After building and deploying, use `tools/smoke-test.sh` before asking a human for visual feedback:

```bash
ge/tools/smoke-test.sh --platform ios-sim --device tablet
ge/tools/smoke-test.sh --platform android-emu --package com.squz.player
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
