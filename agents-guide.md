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
