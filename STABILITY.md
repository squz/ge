# STABILITY

**Pre-1.0 stability tracking for the `ge` engine.**

Snapshot as of: **v0.1.0** (first release).

---


> **Plateau P (2026-07-11):** the `ged` daemon and its dashboard SPA (`web/`) were **removed**. Dev control plane is [spyder](https://github.com/marcelocantos/spyder); ge keeps app-side app-channel + optional server-mode H.264 encode. Entries below that catalogue `ged` HTTP/MCP routes are **historical** until rewritten.

## Stability commitment

ge is **pre-1.0**. Breaking changes to the public C++ API, CLI / launch
surface, Makefile / `Module.mk` exports, wire protocol, ged HTTP/MCP
surface, and file formats may land in any minor release while we remain
pre-1.0. The pre-1.0 period exists so that these surfaces can be refined
without ecosystem friction.

Once **1.0** ships, every item in the interaction surface catalogue
below becomes a binding backwards-compatibility contract. Post-1.0
breaking changes to any catalogued item are not permitted as a minor or
patch release — the project's policy is that a new product is forked
rather than a major bump taken (see the release skill's Phase 1.6
breaking-change audit for enforcement).

This document's two post-1.0 survivors are:

- The **interaction surface catalogue** — canonical diffable snapshot of
  every public item, used as the baseline for each future release's
  breaking-change audit.
- (Until 1.0) the **gaps and prerequisites** section, tracking the
  concrete items we want resolved before the backwards-compat contract
  locks in.

---

## Interaction surface catalogue

Every public surface listed below is annotated with one of:

- **Stable** — well-exercised, unlikely to change pre-1.0.
- **Needs review** — functional but naming / scope / signature may
  benefit from refinement before 1.0.
- **Fluid** — actively evolving, known to need rework, or experimental;
  freezing now would be costly.

---

## Public C++ API

All public API lives under `include/ge/*.h`.

### Namespaces

- `ge::` — primary engine namespace; session host, rendering, platform, I/O, assets, animation.
- `wire::` — C-style POD structs and constants for the H.264 streaming wire protocol.
- `tweak::` — runtime parameter tweak system (dashboard-driven live editing).
- `imgdiff::` — image comparison utilities (testing).
- Top-level (no namespace) — `DampedRotation`, `DampedValue`, `DeltaTimer`, `SdlContext`, `EventWatchHandle`, `FrameLog<Entry>`.

### Session host / entry point

- `ge::run(Factory factory, const SessionHostConfig& config = {}) → void`. Blocks until SIGINT or all sessions end; drives the bgfx loop, H.264 pipeline, and per-session state. **Stable**.
- `ge::Factory = std::function<RunConfig(Context)>`. **Stable**.
- `ge::Context` — cheaply copyable (shared_ptr internals) platform context passed to the factory.
  - `int width() const` / `int height() const` — **Stable**
  - `DeviceClass deviceClass() const` — **Stable**
  - `std::shared_ptr<sqlpipe::Database> db() const` — **Needs review**. The `db()` getter exposes a concrete `sqlpipe::Database`; interface may change when the persistence story stabilises.
- `ge::DeviceClass : uint8_t { Unknown=0, Phone=1, Tablet=2, Desktop=3 }`. **Stable**.
- `ge::RunConfig` — designated-initialiser struct of render-loop callbacks.
  - `std::function<void(float dt)> onUpdate` — **Stable**
  - `std::function<void(int w, int h)> onRender` — **Stable**
  - `std::function<void(const SDL_Event&)> onEvent` — **Stable**
  - `std::function<void()> onShutdown` — **Stable**
- `ge::SessionHostConfig` — configuration passed to `ge::run`.
  - `int width`, `int height` — **Stable**
  - `bool headless` — **Stable**
  - `const char* orgName`, `const char* appName` — **Stable**
  - `std::string schemaDdl` — **Needs review**. sqlpipe auto-migration API is still evolving.
  - `uint8_t sensors` — **Needs review**. Raw bitmask; a named enum would be cleaner before 1.0.
  - `uint8_t orientation` — **Needs review**. Raw byte; could be typed `wire::Orientation`.
  - `bool disableScreenSaver` — **Stable**.

### Rendering

- `ge::BgfxConfig` — POD struct (`width`, `height`, `headless`, `title`). **Needs review**.
- `ge::BgfxContext` — RAII bgfx init/shutdown, exposes `width()`, `height()`, `shouldQuit()`, `window()`. **Needs review**. Exposed to consumers wanting manual bgfx control, but `ge::run` wraps all typical usage.
- `ge::RenderHost` — abstract interface between engine and render subsystem. Pure-virtual methods: `width()`, `height()`, `deviceClass()`, `send(const wire::SessionConfig&)`, `setEventHandler(...)`, `pumpEvents()`, `beginFrame()`, `endFrame(uint32_t frameNumber)`, `shouldQuit()`. **Fluid**. Introduced in PR #11 (engine/render/bridge split); actively evolving.

### Protocol types and constants (`wire::`)

- `wire::kProtocolVersion = 6` (`uint16_t`). **Fluid**. Bumped on breaking change.
- `wire::kMaxMessageSize = 512 * 1024 * 1024`. **Stable**.
- All 11 message magic constants (`kDeviceInfoMagic` … `kSessionConfigMagic`) — listed under **Wire + ged API surface** below. **Fluid** collectively; new message types added with features.
- `wire::DeviceInfo`, `wire::SafeAreaUpdate`, `wire::AspectLock`, `wire::SessionConfig`, `wire::MessageHeader` — POD struct layouts documented under **Wire + ged API surface**. **Stable** for layout; **Fluid** for field set.
- `wire::kSensorAccelerometer = 1`. **Stable**.
- `wire::kOrientationLandscape / LandscapeFlipped / Portrait / PortraitFlipped`. **Stable** (aliases for SDL_DisplayOrientation).

### Assets

- `ge::MeshRef` — `{name, texture}`. **Stable**.
- `ge::ModelDef<Meta = nlohmann::json>` — `{meshes, meta}` with `to_json`/`from_json`. **Needs review** (template default leaks nlohmann::json into consumers).
- `ge::ManifestDoc<Meta = nlohmann::json>` — `{version, mesh_file, textures, models}`. **Needs review** (same).
- `ge::MeshVertex` — `{x, y, z, u, v}` 20-byte static-asserted binary layout. **Stable**. Data-breaking to change.
- `ge::Model` — `Mesh + const Texture*` pairing with `isValid()`, `mesh()`, `texture()`, `name()`. **Needs review**. Non-owning `Texture*` lifetime contract is fragile.

### Animation

- `ge::GlobeController` — drag/pinch/inertia for globe-like views. Methods: `event(SDL_Event)`, `update(dt)`, `rotation()`, `consumePinchDelta()`, `dragging()`, `pinching()`, `setSensitivity(...)`, `setDamping(...)`. **Stable**.
- `DampedRotation` (top-level, not `ge::`) — quaternion orientation + angular velocity with exponential decay. `matrix()`, `rotate()`, `applyDrag()`, `update(dt)`, `isMoving()`. **Needs review** (should be in `ge::` namespace).
- `DampedValue` (top-level) — scalar with inertial velocity + friction. **Needs review** (namespace).
- `DeltaTimer` (top-level) — `tick()` returns dt in seconds. **Stable**.
- `FrameLog<Entry>` (top-level) — double-buffered frame timing logger, 2 s analysis interval. **Needs review** (namespace + hardcoded interval).

### Tweak system (`tweak::`)

- `tweak::Scale { Linear, Log }`. **Stable**.
- `tweak::Color = linalg::vec<float,4>`. **Stable**.
- `tweak::float2 = linalg::vec<float,2>`. **Stable**.
- `tweak::TweakBase` — type-erased base; global registry (`all()`); virtual `loadJson/toJson/defaultJson/metadataJson/resetToDefault`. **Needs review** (global list).
- `tweak::Tweak<T>` — typed live parameter (`get`, `set`, `operator T`, `defaultVal`, `scale`, `speed`). **Stable**.
- `tweak::EnumTweak : Tweak<int>`. **Stable**.
- `tweak::AxisTweak : Tweak<float>` with drag axis + scale. **Stable**.
- `tweak::Vec2Tweak : Tweak<float2>` with per-axis Dir. **Stable**.
- `tweak::Dir { Right, Left, Up, Down }`. **Stable**.
- Free functions: `loadOverrides`, `save`, `resetOne`, `resetAll`, `allToJson`, `parseAndApply`, `parseAndReset`, `generation()`, `db()`. **Stable** for all except `db()` (returns raw `sqlite3*&`) — **Fluid**.

### Platform

- `ge::resource(const std::string&) → std::string`. Platform-specific asset path resolution. **Stable**.
- `SdlContext` (top-level) — RAII SDL lifecycle + window + `nativeSurface()` (CAMetalLayer on Apple, ANativeWindow on Android). `addEventWatch(EventFilter)` returns `EventWatchHandle`. **Needs review** (top-level; stale doc comment about WebGPU/Dawn in the header).
- `EventWatchHandle` (top-level) — move-only RAII SDL event-watch remover. **Stable**.
- `ge::installSignalHandlers() → void`. **Stable**.
- `ge::shouldQuit() → bool`. **Stable**.

### I/O

- `ge::openFile(const std::string&, bool binary = false) → std::unique_ptr<std::istream>`. Platform-transparent file open (APK assets, iOS bundle, normal FS). **Stable**.
- `ge::WsConnection` — abstract WebSocket: `sendBinary`, `sendText`, `recvBinary`, `close`, `isOpen`, `available`, `setSendTimeout`, `setRecvTimeout`. **Stable**.
- `ge::connectWebSocket(host, port, path, connectTimeoutMs = 0) → std::shared_ptr<WsConnection>`. **Stable**.

### Video (Apple VideoToolbox only; no platform guard in header)

- `ge::VideoEncoder` — H.264 BGRA → NAL frames. `Frame` struct, `FrameCallback`, constructor `(width, height, fps, onFrame)`, `encode(bgra, bytesPerRow)`, `encode(CVPixelBufferRef)`, `flush()`, `resize()`. **Needs review** (no platform guard; resize semantics undocumented).
- `ge::VideoDecoder` — H.264 NAL → BGRA frames. `FrameCallback`, `setParameterSets(sps, spsLen, pps, ppsLen)`, `decode(nalData, nalSize)`, `flush()`. **Needs review** (no platform guard; two-phase init).

### Player subsystem

Both introduced in PR #11 (engine/render/bridge split).

- `ge::PlayerRender` — SDL window + renderer for the brokered player. `Config { initialW, initialH, borderless, orientation }`, `enableAccelerometer()`, `getDeviceDimensions(...)`, `updateVideoTexture(...)`, `pumpEvents() → PumpResult`, `render() → RenderStats`, `window()`. **Fluid**.
- `ge::PlayerWireBridge` — wire half of the player. `Config { host, port = 42069, serverName, connectTimeoutMs = 2000 }`, `DecodedFrame`, `PumpStats`, `connect(out SessionConfig)`, `sendDeviceInfo(...)`, `sendEvent(...)`, `pump()`, `pollFrame(out DecodedFrame)`, `lastPumpStats()`, `isOpen`, `close()`. **Fluid**.

### Utilities

- `ge::FontRef`, `ge::resolveFont(uri)` — `system:<name>` / `file:<path>` / relative path resolution. **Stable**.
- `ge::GeTexEncoding : uint16_t { Astc4x4=0, Png=1, Etc2Rgba8=2 }`. **Stable**.
- `ge::GeTexHeader` — 16-byte static-asserted header for `.getex` files. **Stable**. Data-breaking to change.
- `ge::kGeTexMagic[4] = "GETX"`. **Stable**.
- `ge::textureToFile(path, pixels, w, h)` — write to `.astc.getex` / `.png.getex` / `.astc` / `.png`. **Needs review** (tools-flavoured; placement in public headers is debatable).

---

## Build-system contract

ge is consumed via `-include $(ge)/Module.mk`. The contract below is what apps rely on.

### Variables the consumer sets before the include

| Variable | Default | Purpose | Stability |
|---|---|---|---|
| `ge` | `ge` | Path to the ge submodule. | **Stable** |
| `APP_NAME` | (required) | Binary stem → `bin/$(APP_NAME)`. Lowercase by convention. | **Stable** |
| `APP_DISPLAY_NAME` | `$(APP_NAME)` | Display name; Android activity casing derives from this. | **Stable** |
| `APP_ID` | (required for mobile) | Reverse-DNS bundle id / Java package. | **Stable** |
| `APP_SRC` | (required) | App `.cpp` list. | **Stable** |
| `APP_SHADERS` | — | `.bin` shader targets under `$(BUILD_DIR)`. | **Stable** |
| `BUILD_DIR` | `build` | Output root. | **Stable** |
| `CXX` / `CC` | `clang++` / `clang` | Compilers. | **Stable** |
| `CXXFLAGS` | `$(ge/CXXFLAGS_BASE) -Isrc` | Overrides OK, must retain `$(ge/INCLUDES)` and `$(ge/BGFX_ALL_INCLUDES)`. | **Stable** |
| `SDL_CFLAGS` | `-I$(ge)/vendor/sdl3/include` | SDL3 headers. | **Stable** |
| `FRAMEWORKS` | `$(ge/FRAMEWORKS)` | macOS/iOS frameworks; extend with `+=`. | **Stable** |
| `APP_LIBS` | `$(ge/BOX2D_OBJ)` | Extra app static libs. | **Needs review**. |
| `APP_DEBUG`, `APP_DEBUG_OBJ` | derived | Debug-variant paths / objects. | **Fluid**. |
| `IOS_DEVELOPMENT_TEAM` | `""` | Apple dev team ID for `ios-init`. | **Stable** |
| `CHECK_EXCLUDE` | `""` | Space-separated glob patterns for cell exclusion in `make check`. | **Stable** |
| `GE_ANDROID_TABLET_AVD` | `Pixel_Tablet` | AVD name used for Android tablet emulator cells. | **Needs review**. |
| `GE_IOS_TABLET_DEVICE` | `Pippa` | Preferred iPad name/UDID substring. | **Needs review**. |
| `GE_IOS_PHONE_DEVICE` | `""` | Preferred iPhone name/UDID substring. | **Stable** |

### Variables Module.mk exports

All namespaced `ge/`; read-only from the consumer.

| Category | Variables | Stability |
|---|---|---|
| Includes / flags | `ge/INCLUDES`, `ge/BGFX_ALL_INCLUDES`, `ge/CXXFLAGS_BASE`, `ge/FRAMEWORKS` | **Stable** |
| Library targets | `ge/LIB` (`libge.a`), `ge/BGFX_LIBS` (`libbgfx.a libbimg.a libbx.a`) | **Stable** |
| | `ge/SDL_LIBS` (prebuilt SDL3 stack) | **Needs review** (hard-coded macos-arm64 paths) |
| Source / object lists | `ge/SRC`, `ge/OBJ`, `ge/SRC_DIRECT`, `ge/SRC_BROKERED`, `ge/TEST_SRC`, `ge/TEST_OBJ` | **Needs review** (direct/brokered split is internal) |
| | `ge/BOX2D_OBJ` | **Stable** |
| | `ge/TRIANGLE_OBJ` | **Fluid** |
| Shaders | `ge/RENDER_SHADERS`, `ge/APP_SHADERS_GLES`, `ge/RENDER_SHADERS_GLES`, `ge/SHADERC`, `ge/SHADER_DIR`, `ge/SHADERC_VARYINGDEF` | **Stable** |
| | `ge/SHADERC_PROFILE`, `ge/SHADERC_PLATFORM` | **Needs review** |
| Binaries | `ge/PLAYER` (`bin/player`) | **Stable** |
| | `ge/IMGDIFF` (`bin/imgdiff`) | **Fluid** |
| Test matrix | `ge/CELLS`, `ge/CHECK_CELLS` | **Stable** |
| | `ge/CHECK_EXCLUDE_PATTERNS` | **Fluid** (internal) |
| Shared extension | `CLEAN`, `COMPILE_DB_DEPS` | **Stable** |
| | `ge/DEPGRAPH_DEPS` | **Fluid** |
| Canned recipe | `ge/INIT_DONE` | **Stable** |

### Make targets

| Target | Purpose | Stability |
|---|---|---|
| `all`, `run`, `clean` | Standard lifecycle. | **Stable** |
| `ge/debug` | Debug-variant binary (`bin/$(APP_NAME)-debug`, `-O0 -g -DDEBUG -DBX_CONFIG_DEBUG=1`). | **Stable** |
| `ge/player`, `ge/imgdiff` | Engine tools. | **Stable** / **Fluid** |
| `ge/init`, `compile_commands.json` | Dev setup + clangd DB. | **Stable** |
| `depgraph`, `clean-depgraph` | Dependency-graph SVG. | **Fluid** |
| `ged`, `ged-test`, `web` | Daemon + dashboard. | **Stable** / **Stable** / **Fluid** |
| `ge/ios-init`, `ge/ios`, `ge/ios-release`, `ge/ios-device-release` | iOS app build targets. | **Stable** |
| `ge/android-init`, `ge/android`, `ge/android-release` | Android app build targets. | **Stable** |
| `ge/player-ios*`, `ge/player-android*` | Engine-side mobile player builds. | **Needs review** (marked broken in CLAUDE.md, being rebuilt) |
| `check`, `matrix-test`, `check-list` | Test-matrix entry points. | **Stable** |
| `cell.<name>` | One `.PHONY` rule per canonical cell (24 total). | **Stable** |

### Test-matrix contract

Canonical 24-cell list (all **Stable**):

```
desktop-{dist,player}
{ios-sim,ios-device,android-emu,android-device}-{phone,tablet}-{dist,player}
{desktop,ios,android}-debug-{dist,player}
```

- Cell name grammar: `<platform>-<runtime>-<form-factor>-<mode>` or `<platform>-debug-<mode>`. **Stable**.
- `CHECK_EXCLUDE='glob1 glob2'` — space-separated shell globs; `*` translates to Make's `%`. **Stable**.
- Sub-checks run per cell (applicable subset): cold-launch, startup-flash (mobile), soak (60 s / 10 s debug), rotation round-trip (sim/emu mobile), bg/fg (mobile), reconnect (player-mode), clean-exit. **Stable** collectively.
- `matrix-cell.sh` exit codes: 0 pass, 1 sub-check fail, 2 setup error. **Stable**.

### Scaffold generation

- `tools/init-ios.sh <bundle-id> <app-name> [dev-team]` and `tools/init-android.sh <package> <app-name>` — invoked via `make ge/ios-init` / `make ge/android-init`. Abort if target directory exists. **Stable**.
- Template tokens (substituted in all `.in` files): `__BUNDLE_ID__`, `__APP_NAME__`, `__CMAKE_PROJECT__`, `__DEVELOPMENT_TEAM__`, `__GE_REL__`, `__PACKAGE__`, `__ACTIVITY__`. **Stable** (load-bearing substitution points).

### ge root Makefile delegator

- `SAMPLE ?= sample/tiltbuggy` — only user-facing variable. **Stable**.
- Forwarded targets: `all`, `check`, `matrix-test`, `check-list`, `unit-test`, `clean`, `run`, `ged`, `init`, `ge/%`, `cell.%`. **Stable**.

---

## Wire + ged API surface

### Wire protocol version

- `wire::kProtocolVersion = 6` (mirrored by `protocolVersion = 6` in `ged/daemon.go`). **Stable** within 0.x; bumped on breaking structural change.
- Little-endian byte order (static-asserted).
- Max frame: `kMaxMessageSize = 512 MB` (matched by `maxFrameSize` in `ged/bridge.go`).

### Wire message magic constants

All share the ASCII prefix `GE2` (`0x474532xx`).

| Constant | Value | ASCII | Direction | Purpose | Stability |
|---|---|---|---|---|---|
| `kDeviceInfoMagic` | 0x47453244 | `GE2D` | player → server | Device dims, class, pixel ratio, safe area, orientation | **Stable** |
| `kSafeAreaMagic` | 0x47453245 | `GE2E` | player → server | Safe-area update on orientation change | **Stable** |
| `kSdlEventMagic` | 0x47453249 | `GE2I` | player → server | Raw SDL input event | **Stable** |
| `kSessionEndMagic` | 0x4745324D | `GE2M` | ged → player | Server disconnected | **Stable** |
| `kServerAssignedMagic` | 0x4745324E | `GE2N` | ged → player | Server name for this session | **Stable** |
| `kSqlpipeMsgMagic` | 0x47453254 | `GE2T` | bidirectional | sqlpipe channel messages | **Needs review** |
| `kVideoStreamMagic` | 0x47453256 | `GE2V` | server → ged / player → ged | H.264 NAL frames | **Stable** |
| `kStreamStartMagic` | 0x47453257 | `GE2W` | ged → player | Begin H.264 upload | **Stable** |
| `kStreamStopMagic` | 0x47453258 | `GE2X` | ged → player | Stop H.264 upload | **Stable** |
| `kAspectLockMagic` | 0x47453260 | `` GE2` `` | server → player | Lock aspect ratio | **Stable** |
| `kSessionConfigMagic` | 0x47453243 | `GE2C` | server → player | Session requirements (sensors, orientation) | **Stable** |

### Wire payload structs

- `wire::MessageHeader { uint32_t magic; uint32_t length }` (8 bytes). **Stable**.
- `wire::DeviceInfo { magic, version, width, height, pixelRatio, deviceClass, orientation, safeX, safeY, safeW, safeH }`. **Stable**. Own `magic + version`; not prefixed by `MessageHeader`.
- `wire::SafeAreaUpdate { magic, safeX, safeY, safeW, safeH }`. **Stable**.
- `wire::AspectLock { magic, float ratio }` (`ratio = 0.0` = unlock). **Stable**.
- `wire::SessionConfig { magic, sensors, orientation, _pad[2] }`. **Stable**. Must be consumed by player before `DeviceInfo` is sent.

### H.264 video frame wire format

Carried by `kVideoStreamMagic` frames; intercepted by ged, not forwarded verbatim. Payload: `[1-byte flags][optional SPS/PPS][NAL data]`. **Needs review** (internal ged↔player contract).

### Server sideband WebSocket protocol

- `/ws/server` text JSON and binary frames.
- Hello (first frame): `{ "type": "hello", "name": <server-name>, "pid": <n>, "version": 6 }`. ged rejects mismatched versions.
- Server → ged text `type`s: `log`, `preview`, `preview_bin`, `accel`, `tweaks`, plus opaque `<any> { type, data }`.
- ged → server text `type`s: `player_attached`, `player_detached`, `tweak_set`, `tweak_reset`.
- **Needs review** — set of sideband types is growing; may be renamed before 1.0.

### ged connection addresses

- Default listen `:42069` (all interfaces). **Stable**.
- `GE_DAEMON_ADDR` env var (iOS only) in `host:port` format. **Stable**.
- `--port <n>` CLI override. **Stable**.
- LAN IP discovery via UDP dial against `8.8.8.8:53`. **Needs review** (air-gapped networks).

### ged HTTP / WebSocket routes

| Route | Method | Purpose | Stability |
|---|---|---|---|
| `/api/info` | GET | Server state: `{connected, servers[], sessions}` | **Stable** |
| `/api/url` | GET | QR pairing URL `{url: "ge-remote://ip:port"}` | **Stable** |
| `/api/qr` | GET | QR code PNG 256×256 | **Stable** |
| `/api/state/{type}` | GET | Cached sideband state | **Needs review** |
| `/api/tweaks` | GET | Alias for `/api/state/tweaks` | **Needs review** |
| `/api/servers/{id}/select` | POST | Switch all players to server | **Needs review** |
| `/api/sessions/{sid}/server/{serverID}` | POST | Switch session to server by ID | **Needs review** |
| `/api/sessions/{sid}/select/{serverName}` | POST | Switch session by name (player-driven) | **Stable** |
| `/api/sideband` | POST | Forward arbitrary JSON to server(s) | **Needs review** |
| `/api/tweaks`, `/api/tweaks/reset` | POST | Back-compat wrappers for sideband | **Needs review** |
| `/api/stop` | POST | SIGINT game server(s) | **Needs review** |
| `/quitquitquit` | POST | Graceful self-shutdown (supersession) | **Fluid** (internal) |
| `/ws/wire` | WebSocket | Player binary wire bridge; `?name=...&preference=...` | **Stable** |
| `/ws/server` | WebSocket | Game server sideband | **Stable** |
| `/ws/server/wire/{sid}` | WebSocket | Per-session wire from server | **Stable** |
| `/ws/logs` | WebSocket | Dashboard log stream | **Needs review** |
| `/ws/preview` | WebSocket | Dashboard preview frames | **Needs review** |
| `/ws/stream/{sid}` | WebSocket | Browser-playable fMP4 H.264 stream | **Needs review** |

### ged `/mcp` — MCP server

Streamable HTTP at `/mcp`; server name `"ged"`, version `"1.0.0"`.

| Tool | Inputs | Output | Stability |
|---|---|---|---|
| `info` | — | Same as `GET /api/info` | **Stable** |
| `tweak_list` | — | Cached `tweaks` sideband state | **Stable** |
| `tweak_get` | `name: string` | Tweak object | **Stable** |
| `tweak_set` | `name`, `value: number` | Confirmation | **Needs review** (numeric only; arrays/bools not yet) |
| `tweak_reset` | `name?: string` | Confirmation | **Stable** |
| `logs` | `count?: number` (def 20, max 200) | Newline-separated JSON entries | **Stable** |

### Player launch-param protocol

#### Android

| Priority | Method | Consumed-once? | Stability |
|---|---|---|---|
| 1 | `--es ged_addr "host:port"` intent extra (`adb shell am start`) | Yes | **Stable** |
| 2 | `debug.ge.address` sysprop (`adb shell setprop`) | No | **Stable** |
| 3 | Emulator auto-connect `10.0.2.2:42069` | N/A | **Needs review** (hardcoded port) |
| 4 | QR code scan (fallback) | N/A | **Stable** |

#### iOS

| Priority | Method | Consumed-once? | Stability |
|---|---|---|---|
| 1 | `-ged_addr "host:port"` launch arg (→ NSUserDefaults) | Yes | **Stable** |
| 2 | `GE_DAEMON_ADDR` env var (via `devicectl … -e`) | No | **Stable** |
| 3 | Simulator auto-connect `localhost:42069` | N/A | **Needs review** (hardcoded port) |
| 4 | QR code scan (fallback) | N/A | **Stable** |

### QR-pairing protocol

- Content: `ge-remote://<LAN-IP>:<port>` (e.g. `ge-remote://192.168.1.42:42069`). **Stable**.
- Both Android and iOS QR scanners validate the `ge-remote://` prefix; non-matching codes silently ignored.

---

## Shader + asset + tweak surface

### Shader pipeline

#### App-level contracts

- `<app>/shaders/varying.def.sc` must declare `vec3 a_position : POSITION` — `vec2` triggers a glsl-optimizer NaN-clip defect in `-p 300_es` on Android GLES. **Stable** (workaround documented).
- App-supplied `.sc` files live under `$(ge/SHADER_DIR)` (default `shaders/`); `APP_SHADERS` lists their `.bin` outputs. **Stable**.

#### Shader compilation

| Target | `--platform` | `-p` | Output |
|---|---|---|---|
| macOS / iOS (Metal) | `osx` | `metal` | `$(BUILD_DIR)/shaders/*.bin` |
| Android (Vulkan / SPIRV) | `android` | `spirv` | `$(BUILD_DIR)/shaders-gles/*.bin` |

- `ge/SHADERC_PROFILE` default `metal`; `ge/SHADERC_PLATFORM` default `osx`. **Stable**.
- Android uses bgfx Vulkan backend via SPIRV shaders; GLES profiles (`300_es` / `310_es`) are intentionally not used. **Stable**.
- The Android Gradle `syncAssets` task flattens `build/shaders-gles/` and `build/ge/shaders-gles/` into `assets/build/shaders/` and `assets/build/ge/shaders/` respectively. **Stable**.

#### ge-provided shaders

- `src/render/shaders/ge_compose_vs.sc` / `ge_compose_fs.sc` — fullscreen-quad compose pass.
- Engine-internal `varying.def.sc` declares `v_texcoord0 : TEXCOORD0`, `a_position : POSITION`, `a_texcoord0 : TEXCOORD0`. **Stable**.
- Bound sampler: `s_tex` at slot 0. **Stable**.

#### Cross-platform caveats

- Android GLES glsl-optimizer `vec4(vec2, f, f)` bug → workaround via `vec3 a_position` + `vec4(a_position, 1.0)`. **Stable**.
- Android emulator EGL 3.1 `BAD_CONFIG` on Apple-Silicon host → switched to Vulkan / SPIRV. **Stable**.

### Asset-manifest format

Schema (from `ManifestSchema.h`):

```json
{
  "version": 1,
  "mesh_file": "build/model.bin",
  "textures": { "body": "build/textures/body.ktx" },
  "models": {
    "car": {
      "meshes": [ { "name": "body_mesh", "texture": "body" } ],
      "meta": { }
    }
  }
}
```

| Field | Type | Required | Stability |
|---|---|---|---|
| `version` | int (= 1 for v0.1.0) | yes | **Stable** |
| `mesh_file` | string (relative path; resolved via `ge::resource`) | yes | **Stable** |
| `textures` | `map<string,string>` | yes | **Stable** |
| `models` | `map<string,ModelDef>` | yes | **Stable** |
| `models[*].meshes[*].{name,texture}` | strings | yes | **Stable** |
| `models[*].meta` | opaque object (templated on app type) | optional | **Needs review** |

Deterministic output: `std::map`-based serialisation gives sorted-key JSON. **Stable**.

### Asset lookup (`ge::resource`)

| Platform | Base path | Notes |
|---|---|---|
| iOS | `SDL_GetBasePath()` (bundle Resources) | Returns `base + relativePath`. |
| Android | `""` | Returns `relativePath`; SDL AssetManager resolves APK. |
| Desktop | Parent dir of binary (via `SDL_GetBasePath`) | `bin/` layout convention. |

Absolute paths (starting with `/`) returned unchanged on all platforms. **Stable**.

### Tweak system

#### SQLite persistence schema

```sql
CREATE TABLE IF NOT EXISTS tweaks (
    name TEXT PRIMARY KEY,
    json TEXT NOT NULL
)
```

- `tweak::save(name, json)` upserts via `INSERT … ON CONFLICT(name) DO UPDATE`. **Stable**.
- `tweak::resetOne(name)` / `resetAll()` deletes rows + in-memory reset. **Stable**.

#### `allToJson()` element shape

```json
{
  "name": "<tweak-name>",
  "value": <T>,
  "default": <T>,
  "scale": "linear" | "log",
  "speed": <float>
}
```

Type-specific additions:

| Tweak type | Extra fields |
|---|---|
| `Vec2Tweak` | `type: "vec2"`, `xDir`, `yDir` (each in `{"right","left","up","down"}`) |
| `EnumTweak` | `type: "enum"`, `labels: [<string>]` (no `scale`/`speed`) |
| `AxisTweak` | `type: "axis"`, `axis: [<x>, <y>]` |
| `Color` / `Tweak<float4>` | (same as `Tweak<float4>`) |

Value representation:

| C++ | JSON |
|---|---|
| `float`, `int` | number |
| `std::string` | string |
| `float2` | `[x, y]` |
| `float4` / `Color` | `[r, g, b, a]` (3-element defaults `a = 1.0`) |

All **Stable**.

#### `parseAndApply()` / `parseAndReset()`

- `parseAndApply`: `{ "name": "...", "value": <T> }`. Required keys; false on missing. Calls `save()`. **Stable**.
- `parseAndReset`: `{ "name": "..." }` or `{ "all": true }`. The reset-all detection is a string search for `"all"` in the body — **Needs review** (fragile).

### Test-matrix artefact layout

- Reference screenshots: `<app>/test/refs/<cell-name>-untilted.png`. **Note**: current `matrix-cell.sh` captures/compares using `$CELL-cold-launch.png` but the checked-in refs on sample/tiltbuggy use `-untilted.png`. **Needs review** (naming mismatch).
- Per-run root: `${TMPDIR:-/tmp}/ge-matrix-$$/`. **Stable**.
- Per-cell subdir: `$ARTIFACTS_ROOT/<cell-name>/`. Holds `cold-launch.png`, `reconnect.png`, `bgfg.png`, `frame_NNNN.png` (extracted video frames at 10 fps), `launch.mov` / `launch.mp4`. **Stable**.

---

## Gaps and prerequisites (pre-1.0)

Items to resolve before v1.0 locks in the catalogue above as a binding
contract:

### Public C++ API

- [ ] **Move top-level types into `ge::`**: `DampedRotation`, `DampedValue`, `DeltaTimer`, `SdlContext`, `EventWatchHandle`, `FrameLog<Entry>`. Top-level pollution is inappropriate for a library.
- [ ] **Replace raw bytes with named types**: `SessionHostConfig::sensors` and `orientation` should be `wire::SensorFlags` (bitflag enum) and `wire::Orientation` (enum class) rather than `uint8_t`.
- [ ] **`Context::db()` return type**: decide whether to expose `sqlpipe::Database` directly or wrap in a ge-owned facade. Locking this in post-1.0 requires either a stable sqlpipe 1.0 or a non-trivial refactor.
- [ ] **`ModelDef<Meta>` default**: the default `nlohmann::json` meta type leaks a heavy dependency. Consider requiring explicit `Meta` or providing a ge-native opaque-bag alternative.
- [ ] **`Model` texture ownership**: the non-owning `const Texture*` lifetime contract is undocumented and fragile.
- [ ] **Platform guards on Video{Encoder,Decoder}**: headers declare them unconditionally even though implementations are Apple-only. Either guard the headers or supply cross-platform stubs.
- [ ] **Remove stale doc comments**: `SdlContext::nativeSurface()` still references WebGPU/Dawn.
- [ ] **Add C++-level version macros**: `GE_VERSION_MAJOR/MINOR/PATCH` + `GE_VERSION_STRING` in a public header. Not present today; important for the 1.0 contract.

### Wire + ged API

- [ ] **Sideband message set consolidation**: the open set of `/ws/server` text types should be narrowed to a known enum before 1.0.
- [ ] **`tweak_set` MCP input**: accept non-numeric values (arrays, booleans, strings).
- [ ] **ged HTTP response schemas**: several endpoints return ad-hoc JSON; a documented schema (or OpenAPI) would make post-1.0 audits easier.
- [ ] **Hardcoded port `42069` in player auto-connect paths**: respect ged's `--port` override instead.
- [ ] **Protocol version bump policy**: document when a bump is required (field added vs. structural change vs. renamed message).

### Build system

- [ ] **`ge/SDL_LIBS` arch pinning**: currently hardcoded to `macos-arm64`. Multi-arch / Linux support requires restructuring.
- [ ] **Android tablet AVD variable**: the current `GE_ANDROID_TABLET_AVD` default `Pixel_Tablet` is a naming convention, not an auto-detect. Consider detecting tablet-class AVDs by geometry.
- [ ] **`GE_IOS_TABLET_DEVICE` default `Pippa`**: project-specific leak into the public default. Should be empty.
- [ ] **`ge/player-*` mobile targets**: currently marked broken in CLAUDE.md; reach feature parity with the consuming-app build targets before 1.0.

### Shaders / assets

- [ ] **Matrix-test refs filename mismatch**: the script captures/compares under `$CELL-cold-launch.png` but the checked-in baseline files use `-untilted.png`. Align on one convention and migrate refs.
- [ ] **ManifestDoc `version: 1`**: only version 1 is defined. Document the upgrade path for version 2+.
- [ ] **Tweak reset-all heuristic**: `parseAndReset` detects `"all"` via string search; replace with proper JSON key check.

### Third-party licensing

- [ ] **Triangle library** (`vendor/src/triangle.c`): non-standard licence (free for research/private, commercial requires direct author arrangement). `earcut.hpp` is the permissive alternative; plan removal of Triangle for commercial distribution.
- [ ] **FFmpeg (Android) LGPL-2.1 compliance**: ge links FFmpeg statically into the Android player; per LGPL §6(d), consumers must be able to relink. Either (a) switch to dynamic linking, (b) document the object-file drop-in build recipe for downstream consumers, or (c) ship object files alongside the static archive.

### Testing / reliability

- [ ] **Rotation-stability test reference screenshots**: audit which sub-cells should have refs committed vs. captured on first run.
- [ ] **Soak duration**: 60 s per cell × 24 cells is ~24 minutes of idle wall-time. 🎯T27 proposes a dedicated long-soak cell + 10 s per-cell default.

### Documentation

- [ ] **Agent guide**: added in v0.1.0 (`agents-guide.md`). Keep the Gotchas list pruned against each release.
- [ ] **Wire protocol reference doc**: concise standalone `docs/wire-protocol.md` would help second implementers (e.g. web player).

---

## Out of scope for 1.0

Features deliberately deferred beyond the first stable release:

- **🎯T1 — WebAssembly in-process game servers.** Single-binary deployment via Wasm — substantial cross-cutting work; not required for 1.0.
- **🎯T3 — Mip-cache manifest preflight.** Optimisation for reconnect latency. Nice-to-have.
- **🎯T5 — In-player connection management UI.** Dashboard-driven switching (🎯T6) is the 1.0 path.
- **🎯T9 — Device tilt parallax as a first-class RunConfig option.** Designing the `Context::parallax()` contract is non-trivial; ship without it.
- **🎯T11.* — pigeon replacement for ged's WebSocket stack.** Transport swap for LAN / internet NAT traversal. Post-1.0.

These remain on the bullseye frontier and are scheduled independently.
