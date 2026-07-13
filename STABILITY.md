# STABILITY

**Pre-1.0 stability tracking for the `ge` engine.**

Snapshot as of: **v0.74.0** (Plateau P closed — ged removed; spyder is sole control plane).

---

> **Plateau P (2026-07-11):** the `ged` daemon and dashboard SPA were **removed**.
> Dev control plane is [spyder](https://github.com/marcelocantos/spyder).
> ge keeps the app-side app-channel client + optional `GE_SERVER` H.264 encode.
> Release-surface oracle: `make release-surface-test` (🎯T145 acceptance #2).

## Stability commitment

ge is **pre-1.0**. Breaking changes to the public C++ API, CLI / launch
surface, Makefile / `Module.mk` exports, wire protocol, and file formats may
land in any minor release while we remain pre-1.0. The pre-1.0 period exists
so that these surfaces can be refined without ecosystem friction.

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
- `tweak::` — runtime parameter tweak system (spyder / app-channel live editing).
- `imgdiff::` — image comparison utilities (testing).
- Top-level (no namespace) — `DampedRotation`, `DampedValue`, `DeltaTimer`, `SdlContext`, `EventWatchHandle`, `FrameLog<Entry>`.

### Session host / entry point

- `ge::run(Factory factory, const SessionHostConfig& config = {}) → void`. Blocks until SIGINT / host exit; drives the sokol frame loop (direct) or server-mode stream path when built with `GE_SERVER_BUILD`. **Stable**.
- `ge::Factory = std::function<RunConfig(Context)>`. **Stable**.
- `ge::Context` — cheaply copyable (shared_ptr internals) platform context.
  - `drawSafeRectInPts()` / `uiSafeRectInPts()` / `fullRectInPts()` → `Rect` — **Stable** (🎯T60; point space)
  - `pixelsPerPt()` / `ptsPerPixel()` / `deviceUiScale()` — **Stable**
  - `DeviceClass deviceClass() const` — **Stable**
  - `Pass swapchainPass() const` — **Stable** (🎯T101; open at top of `onRender`)
  - `std::shared_ptr<sqlpipe::Database> db() const` — **Needs review** (concrete sqlpipe type)
  - `parallax()`, `fps()` / `frameTime()`, render-on-demand controls — **Needs review** / **Fluid**
- `ge::DeviceClass : uint8_t { Unknown=0, Phone=1, Tablet=2, Desktop=3 }`. **Stable**.
- `ge::RunConfig` — designated-initialiser struct of render-loop callbacks.
  - `std::function<void(float dt)> onUpdate` — **Stable**
  - `std::function<void(const Context&)> onRender` — **Stable**
  - `std::function<void(const SDL_Event&)> onEvent` — **Stable**
  - `std::function<void()> onShutdown` — **Stable**
  - Optional: `onBackPressed`, `onMemoryWarning`, `onMetrics` — **Needs review**
- `ge::SessionHostConfig` — configuration passed to `ge::run`.
  - `int width`, `int height` — **Stable**
  - `bool headless` / `hidden` — **Stable** / **Needs review**
  - `const char* orgName`, `const char* appName` — **Stable**
  - `std::string schemaDdl` — **Needs review**
  - `uint8_t sensors`, `uint8_t orientation` — **Needs review** (raw bytes)
  - `bool disableScreenSaver`, `parallaxFactor`, `metricsReportThreshold`, `crashDiagnostics` — **Needs review**
- `ge::renderToPng` / `ge::renderBatch` — headless one-shot PNG (🎯T124). **Stable**.

### Rendering

- sokol_gfx is the only live backend (bgfx removed — 🎯T38/T98). Consumers call `sg_*` after opening `Context::swapchainPass()`.
- `ge::Pass` — move-only RAII pass (swapchain / offscreen). **Stable**.
- `ge::Sprite` — move-only owning textured quad (🎯T135). **Stable**.
- `ge::drawSolid` — unlit solid-color indexed mesh fill (`<ge/solid.h>`). MVP × position + flat colour. **Stable** (new).
- `ge::sprite` / `svg` / `png` / `text` / `debug` factories — **Stable** / **Needs review** collectively.
- `ge::RenderHost` — abstract host interface used internally by direct / server paths. **Fluid**.

### Protocol types and constants (`wire::`)

- `wire::kProtocolVersion = 6` (`uint16_t`). **Fluid**. Bumped on breaking change.
- `wire::kMaxMessageSize = 512 * 1024 * 1024`. **Stable**.
- All 11 message magic constants (`kDeviceInfoMagic` … `kSessionConfigMagic`) — listed under **Wire + stream-relay API surface** below. **Fluid** collectively; new message types added with features.
- `wire::DeviceInfo`, `wire::SafeAreaUpdate`, `wire::AspectLock`, `wire::SessionConfig`, `wire::MessageHeader` — POD struct layouts documented under **Wire + stream-relay API surface**. **Stable** for layout; **Fluid** for field set.
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
- `ge::PlayerWireBridge` — wire half of the player. `Config { host, port = 3030, serverName, connectTimeoutMs = 2000 }`, `DecodedFrame`, `PumpStats`, `connect(out SessionConfig)`, `sendDeviceInfo(...)`, `sendEvent(...)`, `pump()`, `pollFrame(out DecodedFrame)`, `lastPumpStats()`, `isOpen`, `close()`. **Fluid**.

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
| `CXXFLAGS` | `$(ge/CXXFLAGS_BASE) -Isrc` | Overrides OK; must retain `$(ge/INCLUDES)`. | **Stable** |
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
| Includes / flags | `ge/INCLUDES`, `ge/CXXFLAGS_BASE`, `ge/FRAMEWORKS` | **Stable** |
| Library targets | `ge/LIB` (`libge.a`) | **Stable** |
| | `ge/SDL_LIBS` (prebuilt SDL3 stack) | **Needs review** (hard-coded macos-arm64 paths) |
| Source / object lists | `ge/SRC`, `ge/OBJ`, `ge/SRC_DIRECT`, `ge/SRC_BROKERED`, `ge/TEST_SRC`, `ge/TEST_OBJ` | **Needs review** (direct/brokered split is internal) |
| | `ge/BOX2D_OBJ` | **Stable** |
| | `ge/TRIANGLE_OBJ` | **Fluid** |
| Shaders | `ge/SHADER_DIR`, sokol-shdc outputs under `$(BUILD_DIR)/…/shaders` | **Stable** / **Needs review** |
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
| `ge/debug` | Debug-variant binary (`bin/$(APP_NAME)-debug`). | **Stable** |
| `ge/player`, `ge/imgdiff` | Engine tools. | **Stable** / **Fluid** |
| `ge/init`, `compile_commands.json` | Dev setup + clangd DB. | **Stable** |
| `depgraph`, `clean-depgraph` | Dependency-graph SVG. | **Fluid** |
| ~~`ged`, `ged-test`, `web`~~ | **Removed** (Plateau P / T145). Use spyder. | **Removed** |
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
- Forwarded targets: `all`, `check`, `matrix-test`, `check-list`, `unit-test`, `clean`, `run`, `init`, `ge/%`, `cell.%`. **Stable**.

---

## Wire + stream-relay API surface

### Wire protocol version

- `wire::kProtocolVersion = 6` (stream relay is spyder; protocol version is owned by ge `Protocol.h`). **Stable** within 0.x; bumped on breaking structural change.
- Little-endian byte order (static-asserted).
- Max frame: `kMaxMessageSize = 512 MB` (ge `kMaxMessageSize` is the wire limit).

### Wire message magic constants

All share the ASCII prefix `GE2` (`0x474532xx`).

| Constant | Value | ASCII | Direction | Purpose | Stability |
|---|---|---|---|---|---|
| `kDeviceInfoMagic` | 0x47453244 | `GE2D` | player → server | Device dims, class, pixel ratio, safe area, orientation | **Stable** |
| `kSafeAreaMagic` | 0x47453245 | `GE2E` | player → server | Safe-area update on orientation change | **Stable** |
| `kSdlEventMagic` | 0x47453249 | `GE2I` | player → server | Raw SDL input event | **Stable** |
| `kSessionEndMagic` | 0x4745324D | `GE2M` | relay → player | Server disconnected | **Stable** |
| `kServerAssignedMagic` | 0x4745324E | `GE2N` | relay → player | Server name for this session | **Stable** |
| `kSqlpipeMsgMagic` | 0x47453254 | `GE2T` | bidirectional | sqlpipe channel messages | **Needs review** |
| `kVideoStreamMagic` | 0x47453256 | `GE2V` | server → relay / player → relay | H.264 NAL frames | **Stable** |
| `kStreamStartMagic` | 0x47453257 | `GE2W` | relay → player | Begin H.264 upload | **Stable** |
| `kStreamStopMagic` | 0x47453258 | `GE2X` | relay → player | Stop H.264 upload | **Stable** |
| `kAspectLockMagic` | 0x47453260 | `` GE2` `` | server → player | Lock aspect ratio | **Stable** |
| `kSessionConfigMagic` | 0x47453243 | `GE2C` | server → player | Session requirements (sensors, orientation) | **Stable** |

### Wire payload structs

- `wire::MessageHeader { uint32_t magic; uint32_t length }` (8 bytes). **Stable**.
- `wire::DeviceInfo { magic, version, width, height, pixelRatio, deviceClass, orientation, safeX, safeY, safeW, safeH }`. **Stable**. Own `magic + version`; not prefixed by `MessageHeader`.
- `wire::SafeAreaUpdate { magic, safeX, safeY, safeW, safeH }`. **Stable**.
- `wire::AspectLock { magic, float ratio }` (`ratio = 0.0` = unlock). **Stable**.
- `wire::SessionConfig { magic, sensors, orientation, _pad[2] }`. **Stable**. Must be consumed by player before `DeviceInfo` is sent.

### H.264 video frame wire format

Carried by `kVideoStreamMagic` frames; intercepted by the stream relay, not forwarded verbatim. Payload: `[1-byte flags][optional SPS/PPS][NAL data]`. **Needs review** (internal relay↔player contract).

### Server sideband (stream path)

Server-mode builds dial spyder's stream relay (`GE_SERVER` / default `127.0.0.1:3030`).
Hello + wire frames use `wire::kProtocolVersion`. Control plane (tweaks, logs, state,
screenshots) is **not** the stream sideband — it is the **app-channel**
(`SPYDER_APP_CHANNEL`, compiled out under `NDEBUG`; see `appchannel.h`).

Historical ged text sideband types (`log`, `preview`, `tweak_*`, …) are **removed**
with the daemon. Do not document them as live API.

### Dev control plane (spyder — not ge)

| Surface | Where | Stability |
|---|---|---|
| Stream relay (H.264 + wire) | spyder `serve` default `:3030` | **Stable** (spyder owns routes) |
| App-channel msgpack RPC | `SPYDER_APP_CHANNEL=host:port` | **Stable** (ge client; spyder server) |
| MCP / dashboard / `app_exec` | spyder | **Fluid** (spyder versioned surface) |
| `make release-surface-test` | ge | **Stable** (T145 NDEBUG + non-server symbol oracle) |

The retired ged HTTP/WebSocket/MCP route catalogue is **gone** (Plateau P). Use
spyder's docs for current control-plane endpoints.

### Player launch-param protocol

#### Android

| Priority | Method | Consumed-once? | Stability |
|---|---|---|---|
| 1 | `--es stream_addr` (legacy `ged_addr`) "host:port"` intent extra (`adb shell am start`) | Yes | **Stable** |
| 2 | `debug.ge.address` sysprop (`adb shell setprop`) | No | **Stable** |
| 3 | Emulator auto-connect `10.0.2.2:3030` | N/A | **Needs review** (hardcoded port) |
| 4 | QR code scan (fallback) | N/A | **Stable** |

#### iOS

| Priority | Method | Consumed-once? | Stability |
|---|---|---|---|
| 1 | `-stream_addr` (legacy `-ged_addr`) "host:port"` launch arg (→ NSUserDefaults) | Yes | **Stable** |
| 2 | `GE_STREAM_ADDR` / legacy `GE_DAEMON_ADDR` env var | No | **Stable** |
| 3 | Simulator auto-connect `localhost:3030` | N/A | **Needs review** (hardcoded port) |
| 4 | QR code scan (fallback) | N/A | **Stable** |

### QR-pairing protocol

- Content: `ge-remote://<LAN-IP>:<port>` (e.g. `ge-remote://192.168.1.42:3030`). **Stable**.
- Both Android and iOS QR scanners validate the `ge-remote://` prefix; non-matching codes silently ignored.

---

## Shader + asset + tweak surface

### Shader pipeline

#### App-level contracts

- `<app>/shaders/varying.def.sc` must declare `vec3 a_position : POSITION` — `vec2` triggers a glsl-optimizer NaN-clip defect in `-p 300_es` on Android GLES. **Stable** (workaround documented).
- App-supplied `.sc` files live under `$(ge/SHADER_DIR)` (default `shaders/`); `APP_SHADERS` lists their `.bin` outputs. **Stable**.

#### Shader compilation (sokol-shdc)

- App shaders: `.glsl` → generated headers under `$(BUILD_DIR)/…/shaders` via sokol-shdc.
- `GE_SHDC_LANGS` must include the languages needed per platform (Metal + `spirv_vk` for Android Vulkan). **Stable**.
- Engine sprites/debug ship with internal sokol programs (no consumer bgfx `.sc` pipeline). **Stable**.


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

### Wire + stream-relay API

- [x] **Player default port is 3030** (spyder). Prefer `GE_STREAM_ADDR` / `-stream_addr` (legacy `ged_*` aliases remain).
- [x] **Release surface oracle** (`make release-surface-test`): NDEBUG strips app-channel; non-`GE_SERVER` link drops encode symbols.
- [ ] **Protocol version bump policy**: document when a bump is required (field added vs. structural change vs. renamed message).
- [ ] **Standalone wire protocol doc**: concise `docs/wire-protocol.md` for second implementers.

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

- [x] **README matches sokol + spyder** (post Plateau P). Keep `AGENTS.md` canonical for depth.
- [ ] **Agent guide**: keep `agents-guide.md` Gotchas pruned against each release.

---

## Out of scope for 1.0 (Plateau P parks)

Deferred / set-aside after the control-plane migration; not required to ship multimaze/IAP:

- **🎯T1 — WebAssembly in-process game servers.**
- **🎯T11.* — pigeon transport** (parked; WebSocket via spyder remains).
- **🎯T128.* — command-stream ladder** (H.264 via spyder is the closed stream story).
- **🎯T5 / T6 / T34 — player product UI / player-as-ge-app.**
- **🎯T33.* — matrix-on-spyder reliability theme.**

Active ship cluster: multimaze/IAP (T69, T65.*, T74, T118) and related ergonomics — see `bullseye_frontier`.
