# ge/ Engine Module

This file and `AGENTS.md` carry the same repository instructions for different
agent runtimes. Keep them in sync when changing repo guidance.

**IMPORTANT: When creating any artefact — code, targets, documentation, plans, tests — always consider whether it belongs in `ge/` (general-purpose engine, usable by any app) or in the parent project (game-specific logic). If unsure, ask before creating it.** Anything concerning the player, ged, wire protocol, engine infrastructure, or engine design belongs in `ge/`, not the consuming project.

Reusable rendering and streaming engine built on sokol_gfx + SDL3 (migrated from bgfx — 🎯T38). Consumed as a git submodule; build integration via `Module.mk`.

homebrew_tap: disabled
<!-- ge is a library consumed via git submodule; no binary to ship through brew. -->
profile: game
<!-- interactive rendering + streaming; "tests pass" doesn't guarantee visual correctness. -->

Apps built on ge use a **server/player architecture**: the app (server) renders headless via sokol_gfx, encodes H.264 frames (VideoToolbox on Apple), and streams them to the player over a ged-brokered WebSocket. The player decodes the H.264 stream (VideoToolbox/MediaCodec) and displays it via SDL. Input events flow back over the same WebSocket channel. The app itself has zero platform-specific rendering code — ge handles encoding, framing, and the network link.

## ge Claude Code Plugin

ge ships a Claude Code plugin that exposes four skills for shipping ge-consumer apps.

**Installation (one-time per checkout):**

```bash
cd ~/work/github.com/squz/<game>   # the consuming app's repo, not ge itself
make ship-init
```

This symlinks `~/.claude/plugins/ge` → the ge submodule root. Restart Claude Code
after running it. The skills are then available as `/ge:*` commands.

**Skills:**

| Skill | Description |
|---|---|
| `/ge:ship` | Conversational orchestrator — ask alpha/beta/release intent, run preflight, dispatch to `make ship-alpha/ship-beta/ship-release`. Never bypasses the make substrate. |
| `/ge:release-notes` | Draft CHANGELOG bullets from `git log <last-tag>..HEAD`, grouped by 🎯T-prefix. Idempotent. |
| `/ge:ship-status` | Read `build/ship/*/manifest.json` across squz game repos and emit a portfolio table (repo, lane, version, timestamp, SHA). |
| `/ge:onboard` | Walk a new engineer from zero to a shippable machine state: clone, fastlane, ASC API key, match passphrase, plugin install, cert verify. |

**Plugin location:** `.claude-plugin/plugin.json` at the ge repo root; skills
live under `skills/{ship,release-notes,ship-status,onboard}/SKILL.md`.

## Integrating ge into a New App

### Minimal Example

A complete ge app needs three things: a `Makefile`, a `main.cpp`, and game logic.

**main.cpp** — the standard entry point pattern:

```cpp
#include <ge/SessionHost.h>

int main() {
    MyState state;  // Persistent game state (survives reconnects)

    ge::run([&](ge::Context ctx) -> ge::RunConfig {
        auto app = std::make_shared<MyApp>(ctx);

        return {
            .onUpdate   = [&, app](float dt) { app->update(dt, state); },
            .onRender   = [&, app](const ge::Context& c) { app->render(state, c); },
            .onEvent    = [&, app](const SDL_Event& e) { app->event(e, state); },
            .onShutdown = [&, app]() { app->shutdown(); },
        };
    });
}
```

Key points:
- **`ge::run(Factory)`** connects to the ged daemon broker and spawns sessions for attaching players
- **State lives outside the factory** so it persists across player reconnects
- **App resources are created per session** (each reconnect gets a fresh `Context`)
- The factory callback receives a `ge::Context` (rects, device class, DB) and returns a `RunConfig`
- `RunConfig` uses designated initializers: `onUpdate`, `onRender`, `onEvent`, `onShutdown`
- `onRender(const Context&)` is called each frame. The Context exposes three rects (`drawSafeRectInPts`, `uiSafeRectInPts`, `fullRectInPts`) — all in point space (🎯T60). The game must consciously pick the right one for each piece of work; there is no shortcut `width/height` to dodge the question. The engine refreshes them all before each call (the host's per-frame refresh runs before `onRender`). Future per-frame state (parallax delta, tilt, …) joins `Context`, not the signature. **🎯T101:** open this frame's render pass at the top of `onRender` with `auto p = c.swapchainPass();` *before any draws* — the returned `ge::Pass` holds the swapchain pass open for its lifetime and, on destruction, ends the pass + commits + presents. Open any `ge::offscreenPass` blocks before it.
- `ge::run` blocks until SIGINT or all sessions end
- Ctrl+C terminates the process gracefully

**Makefile** — minimal integration:

```makefile
BUILD_DIR := build
CXX := clang++

-include ge/Module.mk
ge/Module.mk:
	git submodule update --init --recursive

CXXFLAGS := -std=c++20 -O2 $(ge/INCLUDES)
SDL_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl3 2>/dev/null)
FRAMEWORKS := -framework Metal -framework QuartzCore -framework Foundation \
              -framework VideoToolbox -framework CoreMedia -framework CoreVideo

SRC := src/main.cpp src/MyApp.cpp
OBJ := $(SRC:%.cpp=$(BUILD_DIR)/%.o)
APP := bin/myapp

COMPILE_DB_DEPS += $(SRC) Makefile

$(APP): $(OBJ) $(ge/LIB)
	@mkdir -p $(@D)
	$(CXX) $(OBJ) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@

player: $(ge/PLAYER)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c $< -o $@
```

### Running Your App

```bash
make ged && bin/ged &       # Start the daemon broker
make && bin/myapp           # Terminal 1: game server (connects to ged)
make player && bin/player   # Terminal 2: desktop player (connects via ged)
```

The ged daemon manages player connections, QR codes, and session routing. Game servers and players both connect to ged.

### What ge Gives You

| Concern | ge handles it | You write |
|---------|--------------|-----------|
| Rendering backend | sokol_gfx — Metal on Apple, Vulkan with GLES3 fallback on Android (🎯T107) | sokol_gfx (`sg_*`) draw calls in `onRender` |
| H.264 encoding | `VideoEncoder_apple.mm` (VideoToolbox) | Nothing |
| H.264 decoding | `VideoDecoder_apple.mm` (VideoToolbox) | Nothing |
| Frame loop | `ge::run` with delta timing + signal handling | `onUpdate(dt)` + `onRender(const Context&)` callbacks |
| Input | Player captures SDL events, sends over WebSocket | `onEvent(e)` callback |
| Reconnection | `ge::run` spawns new session per player | Separate State from App |
| Session routing | ged daemon manages connections + QR codes | Nothing |
| Asset loading | `ge::loadManifest<T>()` for meshes + metadata | manifest.json + data files |
| Mobile builds | iOS/Android player projects in `ge/tools/` (player port dormant — 🎯T34) | Nothing (shared player) |

## Module.mk Integration

There is no standalone build. The parent Makefile includes `ge/Module.mk`, which provides variables, pattern rules, and generic targets. The parent defines a few variables before the include, extends shared variables with `+=` after it, and writes its own link rules using the exported `ge/` variables.

### Prerequisites

The parent must define these **before** the include:

| Variable | Purpose | Example |
|----------|---------|---------|
| `BUILD_DIR` | Output directory for all build artifacts | `build` |
| `CXX` | C++ compiler | `/usr/bin/clang++` |

These are referenced **after** the include but must be defined before any rules run:

| Variable | Purpose | Example |
|----------|---------|---------|
| `CXXFLAGS` | C++ flags (must include `$(ge/INCLUDES)`) | `-std=c++20 -Wall $(ge/INCLUDES)` |
| `SDL_CFLAGS` | SDL3 header search path | `-I/opt/homebrew/include` |

### Including Module.mk

Use `-include` (with leading dash) so the first clone works before the submodule exists, paired with a rebuild rule that auto-clones:

```makefile
-include ge/Module.mk

ge/Module.mk:
	git submodule update --init --recursive
```

Make will see that `ge/Module.mk` is missing, run the rebuild rule to clone, then restart and re-read the now-present file.

### Exported Variables

Engine-internal variables use the `ge/` prefix. These are read-only — the parent references them but does not modify them.

| Variable | Contents |
|----------|----------|
| `ge/INCLUDES` | `-I` flags for engine + vendor headers (sokol, SDL3, spdlog, asio, etc.) |
| `ge/SRC`, `ge/OBJ` | Engine source files and derived objects |
| `ge/LIB` | Static library path (`$(BUILD_DIR)/libge.a`) |
| `ge/SDL_LIBS` | SDL3 static libraries (SDL3, SDL3_image, SDL3_ttf, freetype, harfbuzz, etc.) |
| `ge/TEST_SRC`, `ge/TEST_OBJ` | Unit test sources and objects |
| `ge/TRIANGLE_OBJ` | Triangle library object — **opt-in; not linked into `libge.a`**. Commercial builds should not reference this without first reading NOTICES.md's Triangle section (restrictive license; commercial distribution requires arrangement with the author). |
| `ge/PLAYER` | ge player binary path (`bin/player`) |

### Shared Variables

Module.mk provides sensible defaults for project-wide variables. The parent extends these with `+=`:

| Variable | Default | Parent extends with |
|----------|---------|---------------------|
| `CLEAN` | `bin build` | Additional directories for `make clean` |
| `COMPILE_DB_DEPS` | `$(ge/SRC) $(ge/TEST_SRC) ge/Module.mk` | App sources and Makefile |

Example:

```makefile
# After the -include ge/Module.mk line:
COMPILE_DB_DEPS += $(SRC) Makefile
```

### Generic Targets

Module.mk defines these targets so the parent doesn't need to:

| Target | Action |
|--------|--------|
| `clean` | `rm -rf $(CLEAN)` |
| `compile_commands.json` | Generate clangd compile database from `$(COMPILE_DB_DEPS)` |
| `ged` | Build the ged daemon (`bin/ged`), compiling the dashboard first |
| `ge/ios` | Generate the iOS Xcode project |
| `ge/android` | Build the Android debug APK |
| `ge/app-icons` | Expand `icons/icon.svg` into iOS + Android icon resources (🎯T50) |

### App icons (🎯T50)

ge ships a build-time tool, `bin/ge-icon-gen`, that takes one source SVG and writes both platforms' app-icon resource layouts. SVG → PNG rasterization runs through `ge::rasterizeSvgToPixels` (T42) so the output is sharp at every size — no resampling artefacts.

**Why SVG → PNG and not native vector?** iOS accepts vector via PDF (Xcode 11+) and Android via VectorDrawable XML (API 26+). Neither accepts plain SVG, both conversions are lossy for the SVG features designers actually use (gradients, masks, filters, text). Rasterizing once at each platform-required pixel grid is predictable, lossless from the SVG, and saves ~10–50 KB per app — trivial for a game binary.

**Consumer workflow:**

1. Author `icons/icon.svg` in your project root. Square aspect, full-bleed (no transparent margins — iOS expects opaque content; Android launcher applies its own mask).
2. Run `make ge/app-icons`. This does two things in one shot:
   - Runs `bin/ge-icon-gen` to write `ios/Assets.xcassets/AppIcon.appiconset/` and `android/app/src/main/res/{mipmap-*,drawable,mipmap-anydpi-v26}/`.
   - Runs `tools/wire-icons.py` to idempotently patch the consuming project's `ios/Info.plist` (adds `CFBundleIconName=AppIcon`), `ios/CMakeLists.txt` (appends the `Assets.xcassets` block, target auto-detected from `add_executable`), and `android/app/src/main/AndroidManifest.xml` (adds `android:icon` and `android:roundIcon` on `<application>`). All three patches are no-ops if the wiring is already present, so re-running the target is safe.

   That's it — no separate `ge/ios-init` re-run or manual manifest edit required. Targets built afterwards pick up the icon automatically.

**What gets generated:**

- iOS: `Assets.xcassets/AppIcon.appiconset/icon.png` (1024×1024) + `Contents.json`. Single-source mode (Xcode 15+) — Xcode generates the smaller per-size icons automatically at build time.
- Android legacy: `mipmap-{m,h,xh,xxh,xxxh}dpi/ic_launcher.png` and matching `_round.png` at 48 / 72 / 96 / 144 / 192 px.
- Android adaptive: `drawable/ic_launcher_foreground.png` (432×432, full-bleed master SVG) + `drawable/ic_launcher_background.xml` (solid color from `ge/APP_ICON_BG_COLOR`, default white) + `mipmap-anydpi-v26/ic_launcher.xml` and `_round.xml` adaptive manifests.

**Override knobs in `Module.mk`:**

| Variable | Default | Purpose |
|---|---|---|
| `ge/APP_ICON_SVG` | `icons/icon.svg` | Source SVG path |
| `ge/APP_ICON_IOS_OUT` | `ios/Assets.xcassets/AppIcon.appiconset` | iOS output dir |
| `ge/APP_ICON_ANDROID_RES_OUT` | `android/app/src/main/res` | Android `res/` dir |
| `ge/APP_ICON_BG_COLOR` | `FFFFFF` | Adaptive-icon background hex (no leading `#` — Make eats it as a comment; the tool prepends `#` itself) |
| `ge/APP_ICON_IOS_INFOPLIST` | `ios/Info.plist` | Info.plist patched by `wire-icons.py` |
| `ge/APP_ICON_IOS_CMAKELISTS` | `ios/CMakeLists.txt` | CMakeLists patched by `wire-icons.py` |
| `ge/APP_ICON_ANDROID_MANIFEST` | `android/app/src/main/AndroidManifest.xml` | AndroidManifest patched by `wire-icons.py` |

**Constraints:** SVG-only input. PNG / JPEG sources are rejected with a clear error pointing at SVG. Multi-size resampling of raster sources is not supported — author the icon as SVG.

**Limitations not covered yet:** explicit `icons/icon-foreground.svg` + `icons/icon-background.svg` for split adaptive-icon control on Android (today: full-bleed master goes into the foreground, background is a solid color). iOS 18 light/dark/tinted variants. iOS animated app icons. All deferable to future targets when a real consumer needs them.

### Android Activity (🎯T41)

ge ships a canonical Android Activity (`ge.GeActivity`) at `android-shared/src/main/java/ge/GeActivity.java`. Consumer apps reference it directly from their AndroidManifest.xml (`android:name="ge.GeActivity"`); no per-app Activity subclass is needed. Gradle source-includes the file from the engine submodule:

```gradle
sourceSets {
    main {
        java.srcDirs = [
            "${rootDir}/<ge-rel-path>/android-shared/src/main/java",
            "${rootDir}/<ge-rel-path>/vendor/github.com/libsdl-org/SDL/android-project/app/src/main/java"
        ]
    }
}
```

`tools/init-android.sh` (`make ge/android-init`) writes the manifest and gradle config above and **does not** scaffold an `app/src/main/java/.../Activity.java` — the per-app Activity tree is gone.

`GeActivity` contains all the SDLActivity boilerplate the engine relies on: `getLibraries()` (returns `{"SDL3", "main"}`), display-cutout listener + `getDisplayCutoutInsets()`, sensor-fused attitude listener + `getAttitude()`, and `applyImmersive()`. Adding new engine-side hooks (more JNI helpers, new lifecycle behavior) only requires bumping the engine submodule pointer in the consumer; consumer apps inherit the change without editing Java.

**Apps that need custom Activity behavior** can subclass `ge.GeActivity` in their own `app/src/main/java/<package>/` tree, add `'src/main/java'` back to `java.srcDirs`, and point the manifest at the subclass. Zero-customization is the supported default — reach for a subclass only when the engine surface genuinely doesn't suffice.

### 16 KB page alignment (🎯T75)

**Every ge Android build produces 16 KB page-aligned shared libraries.** This is a hard contract, not a per-app opt-in: Google Play rejects submissions whose `.so` files are not 16 KB-aligned (Android 15+ devices ship 16 KB memory pages; Android 16 hard-blocks installs of 4 KB-aligned apps with an "ELF alignment check failed" system dialog).

The contract is enforced in **one ge-owned place** — `cmake/android-arm64.cmake` appends `-Wl,-z,max-page-size=16384` to `CMAKE_SHARED_LINKER_FLAGS` before the consumer's `add_library(main SHARED …)` and before ge's SDL `add_subdirectory()` calls. Because `include()` does not open a new scope and the SDL subdirectories copy the flag at entry, **every** shared object in the build inherits it: `libmain.so`, `libSDL3.so`, `libSDL3_image.so`, `libSDL3_ttf.so`, and their transitive deps (freetype, harfbuzz, plutovg). Consumer apps need no Gradle or CMakeLists changes — bumping the ge submodule pointer is enough.

**Why an explicit flag rather than relying on the NDK default:** NDK r28+ links 16 KB-aligned by default, but r27 and earlier default to 4 KB, and it is the consumer's *Gradle* build — not ge's `prebuild.sh` — that chooses the NDK linking the final `.so`. The explicit flag makes the contract NDK-version-independent. (The prebuilt `libge.a` and vendor `.a` static archives have no PT_LOAD segments, so alignment only matters at this final `.so` link.)

**Verify** any built `.so`:

```bash
llvm-readelf -lW <lib>.so | grep LOAD
# every PT_LOAD segment must show Align 0x4000 (16384)
```

`llvm-readelf` ships with the NDK (`<ndk>/toolchains/llvm/prebuilt/*/bin/`) and Homebrew LLVM.

### Developer Setup

`ge/init` installs common prerequisites (Homebrew packages, Git LFS, VS Code settings, compiledb). The parent's `init` target should depend on it and expand the `ge/INIT_DONE` canned recipe at the end:

```makefile
.PHONY: init
init: ge/init
	@echo "── Project setup ──"
	# ... project-specific steps ...
	$(ge/INIT_DONE)
```

### Linking

Link the app against `$(ge/LIB)` and SDL:

```makefile
$(APP): $(APP_OBJ) $(ge/LIB)
	$(CXX) $(APP_OBJ) $(ge/LIB) $(ge/SDL_LIBS) $(FRAMEWORKS) -o $@
```

The `FRAMEWORKS` variable should include VideoToolbox, CoreMedia, CoreVideo, Metal, QuartzCore, and Foundation on macOS/iOS.

Link the player:

```makefile
player: $(ge/PLAYER)
```

## Module Structure

| Directory | Contents |
|-----------|----------|
| `include/` | Public headers (one per class) |
| `src/` | Implementation files + test files (`*_test.cpp`) |
| `tools/` | Player entry point (`player.cpp`), capture backend (`player_capture_apple.mm`) |
| `tools/ios/` | iOS player: Xcode project, build scripts (player port dormant — 🎯T34) |
| `tools/android/` | Android player: Gradle project (player port dormant — 🎯T34) |
| `vendor/github.com/bkaradzic/{bgfx,bx,bimg}/` | Former bgfx rendering libraries (vendored; retained as historical reference — migrated to sokol_gfx in 🎯T38) |
| `vendor/` | Other third-party dependencies: spdlog, linalg.h, earcut.hpp, doctest, Triangle, asio, SQLite3 |

**Note:** SQLite3 is compiled into `libge.a` (from the vendored amalgamation `vendor/src/sqlite3.c`). Do not add `-lsqlite3` to link lines — it's already included.

### ge/SRC (files compiled into libge.a)

| File | Purpose |
|------|---------|
| `ge/src/Resource.cpp` | Asset path resolution |
| `ge/src/FileIO.cpp` | Platform-agnostic file I/O |
| `ge/src/WebSocketClient.cpp` | WebSocket client (ged connection) |
| `ge/src/SokolContext.mm` | sokol_gfx device setup and frame management (Apple); `ge/src/SokolContext_android.cpp` on Android |
| `ge/src/Signal.cpp` | SIGINT / graceful shutdown |
| `ge/src/SessionHost.mm` | `ge::run()` — sideband connect, session lifecycle |
| `ge/src/sprite.cpp` | `ge::Sprite::draw` + `ge::SpriteBatch` (lazy textured-quad program) |
| `ge/src/svg.cpp` | `ge::rasterizeSvg*`, `ge::renderSvgDocument`, font registration |
| `ge/src/png.cpp` | `ge::loadImage` (PNG / JPEG / etc. → Sprite via SDL3_image) |
| `ge/src/text.cpp` | `ge::rasterizeText` (FreeType) |
| `ge/src/VideoEncoder_apple.mm` | H.264 encoding via VideoToolbox |
| `ge/src/VideoDecoder_apple.mm` | H.264 decoding via VideoToolbox |
| `ge/src/audio.cpp` | `ge::audio` — device registry + pause/resume state machine (🎯T7) |
| `ge/src/audio_apple.mm` | iOS AVAudioSession interruption + route-change observers (🎯T43) |
| `ge/tools/player_capture_apple.mm` | Player screen capture backend (Apple) |

## H.264 Streaming Protocol

The server and player communicate over WebSocket (brokered by ged) using binary-framed messages.

### Connection Flow

```
Player → ged:    DeviceInfo   (dimensions, pixel ratio, device class, safe area)
ged → player:    StreamStart  (ged signals player to begin receiving H.264 frames)
Server → ged:    VideoStream  (encoded H.264 NAL units, each frame as one message)
ged → player:    VideoStream  (ged forwards frames to the player)
Player → server: SdlEvent     (input events forwarded back to the server)
Player → server: SafeAreaUpdate (on orientation change)
ged → player:    StreamStop / SessionEnd (on server disconnect)
```

### Steady-State Messages

```
Server → ged → player:  MessageHeader{kVideoStreamMagic} + H.264 NAL data
Player → server:        MessageHeader{kSdlEventMagic} + SDL_Event structs (input)
Player → server:        MessageHeader{kSafeAreaMagic} + SafeAreaUpdate (on resize)
Server → player:        MessageHeader{kAspectLockMagic} + AspectLock (optional)
```

### Key Constants (`Protocol.h`)

| Constant | Value | Purpose |
|----------|-------|---------|
| `kProtocolVersion` | 6 | Protocol version for compatibility checking |
| `kMaxMessageSize` | 512 MB | Maximum single message size |
| `kDeviceInfoMagic` | `0x47453244` | "GE2D" — player → ged: player dimensions/class |
| `kSdlEventMagic` | `0x47453249` | "GE2I" — player → server: SDL input event |
| `kSessionEndMagic` | `0x4745324D` | "GE2M" — ged → player: server disconnected |
| `kServerAssignedMagic` | `0x4745324E` | "GE2N" — ged → player: assigned server name |
| `kSqlpipeMsgMagic` | `0x47453254` | "GE2T" — bidirectional sqlpipe messages |
| `kVideoStreamMagic` | `0x47453256` | "GE2V" — server → ged: H.264 NAL units |
| `kStreamStartMagic` | `0x47453257` | "GE2W" — ged → player: start streaming |
| `kStreamStopMagic` | `0x47453258` | "GE2X" — ged → player: stop streaming |
| `kSafeAreaMagic` | `0x47453245` | "GE2E" — player → server: safe area update |
| `kAspectLockMagic` | `0x47453260` | "GE2`" — server → player: lock aspect ratio |

### Address Resolution

Game servers connect to the ged daemon broker. `ge::run` resolves the daemon address in order:
1. `GE_DAEMON_ADDR` environment variable (format: `"host:port"`)
2. Default: `localhost:42069`

Players connect via ged, which handles QR codes, WebSocket routing, and session management.

## Player

The ge player is a standalone H.264 video player. It receives encoded frames from the server (via ged), decodes them via VideoToolbox (macOS/iOS) or MediaCodec (Android), and renders to an SDL window. Input is forwarded back to the server over the same WebSocket channel.

The player has no app-specific code — it works with any ge app.

### Reconnection

The player retries on disconnect with exponential backoff: 10ms initial, doubling to 2000ms cap, reset on success. This means you can restart the server and the player will reconnect automatically.

### Audio Lifecycle (iOS / Android) (🎯T7 / 🎯T43)

ge provides a centralized audio control API (`<ge/audio.h>`) that automatically pauses all registered SDL audio devices when the app is backgrounded or loses audio focus, and resumes them when it returns to the foreground or regains focus.

**`ge::audio` API (🎯T7 + 🎯T43):**

```cpp
#include <ge/audio.h>

// Register a device — engine pauses/resumes it automatically.
SDL_AudioDeviceID dev = SDL_OpenAudioDevice(...);
auto reg = ge::audio::registerDevice(dev);  // RAII; keep alive

// Query state:
ge::audio::FocusState s = ge::audio::state();  // Active or Paused
```

The `Registration` RAII handle unregisters the device when dropped. `AudioPlayer` (wire-mode player) calls `registerDevice` itself, so no action is needed for wire-mode audio. Direct-mode apps that open their own SDL audio device call `registerDevice` once in their audio init.

**What the engine handles automatically (no game code required):**

| Event | iOS | Android |
|---|---|---|
| App backgrounded | `SDL_EVENT_DID_ENTER_BACKGROUND` → `onBackground()` | `WINDOW_FOCUS_LOST` / `WINDOW_MINIMIZED` → `onBackground()` |
| App foregrounded | `SDL_EVENT_DID_ENTER_FOREGROUND` → `onForeground()` | `WINDOW_RESTORED` / `WINDOW_FOCUS_GAINED` → `onForeground()` |
| Phone call / alarm (T43) | `AVAudioSessionInterruptionNotification` Began → `onAudioFocusLost()` | `AudioManager.OnAudioFocusChangeListener` LOSS → `onAudioFocusLost()` |
| Call ends / resumable (T43) | Notification Ended + shouldResume → `onAudioFocusGained()` | `OnAudioFocusChangeListener` GAIN → `onAudioFocusGained()` |
| Headphones unplugged (T43) | `AVAudioSessionRouteChangeNotification` OldDeviceUnavailable → brief pause + resume | (handled by Android OS / AudioManager) |

The two "silence reasons" (background + focus lost) are independent: both must clear before audio resumes. iOS audio-focus integration installs `AVAudioSession` notification observers in `DirectRenderHost`'s constructor via `ge::audio::installAppleAudioObservers()`. Android routes audio focus through `GeActivity.java`'s `OnAudioFocusChangeListener` → `nativeOnAudioFocusChange()` JNI → atomic drained by `pumpEvents()`.

Desktop builds are unaffected: SDL does not fire background/foreground events on macOS/Linux/Windows, and there is no audio focus concept.

### Ged Quiet Mode

Use `-no-open` to prevent ged from opening the dashboard in the browser on first server connection:

```bash
bin/ged -no-open
```

### Dashboard Development

The ged dashboard is a React/Vite app in `ge/web/`. For hot-reload iteration:

```bash
cd ge/web && npm run dev   # Hot-reload dashboard on :5173, proxies API/WS/MCP to ged
```

The Vite dev server proxies `/api`, `/ws`, and `/mcp` to ged at `localhost:42069`.

### Mobile Builds

**iOS and Android player builds are currently dormant** — the player port to sokol_gfx + H.264 decode is pending 🎯T34. The CMakeLists files and build scripts have been scrubbed of the old Dawn/WebGPU references and marked TODO.

### iOS code signing — development vs ship (🎯T110)

Two signing paths, deliberately separate. `build_project.rb` owns the `CODE_SIGN_*` settings — **never hand-edit signing into the generated xcodeproj.**

- **Development — `make ge/ios-device`** (install on your own test devices). Debug build with `GE_IOS_SIGNING=development` → **Automatic** signing + **Apple Development** (no match, no shared secret), paired with `xcodebuild -allowProvisioningUpdates`. Each developer signs with **their own** cert; iOS devices accumulate multiple dev certs fine, so "different developers, different certs" is a non-problem at the dev layer (match would be *worse* here — shared secret, single point of failure). Sim builds (`make ge/ios`) don't sign.
- **Ship — `make ge/ios-device-release`** (TestFlight / App Store). Release build, default `GE_IOS_SIGNING=appstore` → **Manual** + **Apple Distribution (Squz)** + the **match**-installed `match AppStore <bundle>` profile. The Distribution cert is the one identity that genuinely must be shared, so it lives in match (`fastlane/Fastfile`, `MATCH_PASSWORD` + ASC API key). See [[project_squz_cert_policy]].

**The Apple model in one line:** a *certificate* says **who** signed; a *provisioning profile* says **what that signature may do** (which app, which devices — or any device for App Store — which entitlements). The cert's display name (`Apple Development: MARCELO CANTOS`) names the *member*; the `TeamIdentifier` (`SWA3H3N7TW`) is what attributes the build to **Squz**. They are not the same entity — a dev cert under Team Squz is still a Squz build.

**A new device needs one-time enrollment.** `-allowProvisioningUpdates` reuses *already-registered* devices but will **not** enroll a brand-new one headlessly (`error: Device … isn't registered`). Today: connect it and Run once from Xcode (which enrolls it), then every later CLI build/deploy just works. The headless equivalent — a `fastlane register_devices` step keyed on the ASC API key — is the open piece of 🎯T110; until it lands, a never-before-seen device needs that single Xcode Run. Enrollment is **once per device, ever** — not per build.

**Deployment floor is iOS 16.3** (`build_project.rb` default). Dropping to iOS 15 buys ~2–4% of old/weak/low-spend devices and costs `@available` guards around the iOS-16+ orientation-lock stack — not worth it.

**Installing to a device:** modern devices via `spyder deploy_app` (or Apple `devicectl`, iOS 17+ only); iOS ≤16 devices use go-ios's lockdown path (spyder's high-level deploy/launch/screenshot assume the iOS-17 RemoteXPC tunnel — gap tracked as spyder 🎯T78).

### iOS orientation lock (iPadOS 26+)

**Locking iPad orientation is a TWO-knob setup, and you need both:**

1. **`Info.plist` — `UISupportedInterfaceOrientations`** narrows the *set* iOS will rotate to at launch. Narrow this to the orientations you want allowed.
2. **`SessionHostConfig.orientation` — non-zero** tells the engine to call `playerForceOrientation()`, which activates the `prefersInterfaceOrientationLocked` swizzle (Apple TN3192) and freezes the UI in whatever orientation iOS picked at launch.

`Info.plist` *alone* is **not** enough on iPadOS 26+ — the OS treats every iPad app as resizable under multitasking, so the swivel gesture would re-rotate you mid-play. The swizzle *alone* without a narrowed plist locks "whatever orientation the user happened to be holding the device in at launch," which is also rarely what you want. Ship both.

This was the takeaway of a long debugging session in v0.1.0 (see commit `e0da016`, "Revert Info.plist portrait-only experiment", for the failed plist-only attempt; and `5c2f2a5` for the swizzle that completed the picture, tested on iPadOS 26.4). Things that *also* don't work alone and have all been tried: `UIRequiresFullScreen`, `SDL_HINT_ORIENTATIONS`, `requestGeometryUpdate`, `setNeedsUpdateOfSupportedInterfaceOrientations`. The full list is in the banner comment at the top of `tools/player_orientation_ios.mm`.

**How games request the lock:**

- **Direct-render apps** (`DirectRenderHost`-mode, e.g. TiltBuggy): set `SessionHostConfig.orientation = wire::kOrientationPortrait` (or whichever `wire::kOrientation*` constant matches the game's intent). The engine calls `playerForceOrientation` from `DirectRenderHost::send` automatically. The orientation stub/swizzle is linked into `libge` (v0.3.0+), so apps don't need to add anything to their build.
- **Player apps** get this for free — `wire::SessionConfig.orientation` from the server triggers `playerForceOrientation()` over the wire.

**Authoritative orientation locks (🎯T36, v0.31.0+).** Each `wire::kOrientation*` constant now produces a specific runtime behaviour:

| Constant | iOS effect |
|---|---|
| `kOrientationPortrait` | UI locked to Portrait (home indicator at bottom) |
| `kOrientationPortraitFlipped` | UI locked to PortraitUpsideDown |
| `kOrientationLandscape` | UI locked to LandscapeRight (SDL convention; home indicator on left) |
| `kOrientationLandscapeFlipped` | UI locked to LandscapeLeft |
| `kOrientationAnyLandscape` | UI locked at launch to whichever landscape iOS picks; user can't rotate mid-play. Use for tilt games where the player flips the device freely. |

The engine narrows `UISupportedInterfaceOrientations` to the requested mask at runtime, so iOS rotates the UI at launch even if the device was held in a non-matching orientation. The `Info.plist`'s `UISupportedInterfaceOrientations` becomes the **fallback** (used when no runtime lock is set), not the gate. You can leave the plist permissive and let `SessionConfig.orientation` decide.

If `SessionConfig.orientation` is set but the plist's allowed set doesn't include the requested orientation, the engine logs a loud `SPDLOG_WARN` pointing at the mismatch — the override still works (swizzle overrides plist at runtime), but narrowing the plist matches engine intent and avoids brief launch flicker.

## ged Features

### MCP Server

ged exposes an MCP server at `/mcp` (streamable HTTP) with tools: `info`, `tweak_list`, `tweak_get`, `tweak_set`, `tweak_reset`, `logs`. Configure in `.mcp.json`:

```json
{"mcpServers":{"ged":{"type":"http","url":"http://localhost:42069/mcp"}}}
```

### Server Supersede

When a new game server connects with the same name as an existing one, ged sends SIGINT to the old server process. This enables seamless restarts — just `make && bin/myapp` without manually killing the old process.

### launchd

ged can run as a launchd agent for auto-start on login and restart-on-crash.

## Public API

### Session Host

- **`ge::run(Factory, SessionHostConfig)`** (`SessionHost.h`) — Blocks until SIGINT or all sessions end. Connects to ged via sideband WebSocket, sets up sokol_gfx rendering (headless H.264 encode by default, or native window when `headless=false`), and calls the factory for each attaching player. The factory receives a `ge::Context` and returns a `RunConfig`. `SessionHostConfig` controls default dimensions, headless mode, and app identity for the persistent database path.
- **`ge::renderToPng(Factory, SessionHostConfig, prepare, outPath)` / `ge::renderBatch(Factory, SessionHostConfig, items)`** (`SessionHost.h`, 🎯T124) — Headless one-shot render of a restored game State to a PNG, with no device, ged, spyder, or window in the loop. Builds a hidden `DirectRenderHost` (`SessionHostConfig.hidden` → off-screen drawable), runs the factory once, calls `prepare()` to set up the frame's State, renders one frame through the **unchanged** `onRender` / `swapchainPass()` path (capturing the swapchain to RGBA instead of presenting), and writes the PNG via `ge::writePng`. `renderBatch` amortises host + factory across many `RenderItem{prepare, outPath}` with per-item error capture (one bad fixture doesn't sink the run) and returns the count rendered; `renderToPng` is the single-item case. The reused host is leak-free — a batch frame is byte-identical to a fresh single render of the same State. **Determinism contract:** same State + same `SessionHostConfig` size ⇒ byte-stable PNG — gate any render-liveness animation behind a flag the render path disables (`sample/tiltbuggy`'s `Renderer::setDiagnosticSpin(false)`). Consumer apps parse their own `render` verb before `ge::run`; tiltbuggy is the reference (`render --state <file|-> --out <png> [--batch <manifest>] [--isolate] [--size WxH]`, plus a `make render-test` / `make update-render-goldens` golden loop over committed `fixtures/`). `ge::writePng(path, rgba, w, h)` (`png.h`) is the standalone RGBA8 → PNG writer.
- **`ge::Context`** — Platform context passed to the factory once at session start and to `onRender` each frame. Provides `drawSafeRectInPts()` / `uiSafeRectInPts()` / `fullRectInPts()` (rect accessors in point space, 🎯T60, see `ge::Rect`), `drawSafeInsetsInPts()` / `uiSafeInsetsInPts()` (per-edge `SafeAreaInsets` in pt — 🎯T37 — reach for these only when aligning against a specific chrome edge), `deviceClass()`, `pixelsPerPt()` / `ptsPerPixel()` / `deviceUiScale()` (sizing scalars, below), `parallax()` (device-tilt parallax, see `SessionHostConfig.parallaxFactor`), and `db()` (the engine-managed sqlpipe database). The safe rect is *advisory*, not a clip region — games are free to draw anywhere on the surface but should keep the gameplay grid inside the safe rect so chrome doesn't intrude on it. Cheaply copyable (shared_ptr internals); accessors return live values that the engine updates before each callback. **🎯T60 migration**: pre-T60 `drawSafeRect()` / `uiSafeRect()` / `fullRect()` were renamed to `*InPts()` — consumers must update at compile time; no silent fallback exists.
- **Sizing scalars on `Context`** — Two distinct axes for cross-device sizing:
  - `pixelsPerPt()` / `ptsPerPixel()` — physical-size axis. 1pt is OS-calibrated (1/163" on iPhones, 1/132" on iPads, etc.) so the same pt count yields a similar physical mm at the device's typical viewing distance. iPhone @3x: 3.0; iPad @2x: 2.0; desktop: 1.0. Use for **touch targets, body text, fixed-feel chrome** — anything where the constraint is "must be at least N physical mm". Reciprocal pair.
  - `deviceUiScale()` — form-factor axis. Sublinear scale: `sqrt(short_side_mm / 65mm)` where 65mm is the iPhone-Pro-Max-class reference. 1.0 on the reference phone, ~1.55 on an 11" iPad. Use for **chrome icons, headlines, presentation art** that should *grow* on tablets without blowing up linearly. Returns 1.0 on desktop and 1.0 in wire mode if the player's `pixelRatio` is unknown.
  - Typical use: `touch_px = 44 * ctx.pixelsPerPt()` for a fixed-physical button; `gear_px = 28 * ctx.pixelsPerPt() * ctx.deviceUiScale()` for chrome that scales sublinearly with form factor.
- **System back-press (`RunConfig::onBackPressed`, 🎯T44)** — Setting this consumes the Android Back button / predictive-back gesture (`OnBackInvokedDispatcher` on API 33+, legacy `onBackPressed` override otherwise) and runs the callback on the game thread. Use to surface a pause menu, confirm exit, or step back through an in-game stack. Leaving the field unset means the OS handles back (typically backgrounding the app). iOS is a no-op in practice — the immersive flag suppresses edge-swipe-back. The callback fires one frame late (the engine drains a pending atomic in `pumpEvents` to keep dispatch on the game thread), but the predictive-back animation is unaffected because Java's "consumed?" answer is synchronous.
- **OS memory-pressure warnings (`RunConfig::onMemoryWarning`, 🎯T45)** — Fires when iOS sends `UIApplicationDidReceiveMemoryWarningNotification` (always Critical) or Android sends `onTrimMemory(level)` (mapped to `Low` / `Moderate` / `Critical` via the engine's collapse of the five Android buckets — `RUNNING_MODERATE`→Low, `RUNNING_LOW`/`UI_HIDDEN`/`BACKGROUND`/`MODERATE`→Moderate, `RUNNING_CRITICAL`/`COMPLETE`→Critical). The engine drops its own caches first; the game's response is layered on top. Recommended action: drop high-cost caches (texture mips, audio decoders, font glyph atlases) in proportion to the level. Both events fire on the game thread (the engine drains a pending atomic in `pumpEvents`, same pattern as back-press).
- **Performance metrics (`RunConfig::onMetrics` + `ge::Metrics`, 🎯T111)** — Opt-in callback for adapting the running render path to measured performance. `ge::Metrics` is an extensible bundle — `float fps` (smoothed frames/sec) first, `float frameTime` (seconds, `== 1/fps`) second; future fields (dropped frames, GPU time, render backend, resident memory) join it with default initialisers, so existing `const Metrics&` handlers keep compiling. Fires on the game thread *between frames* (after the per-frame refresh, before `onRender`) so a decision taken in the handler applies to the next frame's draw. Cadence is one knob — `SessionHostConfig.metricsReportThreshold` (relative fraction, default `0.1`): fire when smoothed fps deviates from the last *reported* value by ≥ the threshold; `0` ⇒ every frame; a baseline report fires on the first valid reading (gating against the last *reported* value, not the last frame, means no chatter at a boundary). The metric is an EMA of the run-loop `dt` — the same value the debug overlay's FPS readout shows — so it reads validly on desktop / wire / iOS / Android alike, `0` only before the first timed frame. **ge reports, the app decides:** the engine never steps quality, shows UI, or disables a visual; the consumer owns the decision *and the hysteresis*. The correct sticky pattern is a two-threshold band — drop at a low fps, restore only above a *higher* fps, and remember you simplified — so a recovery caused by simplifying doesn't snap back to the expensive path. For passive display only, `Context::fps()` / `Context::frameTime()` poll the same EMA (e.g. a HUD counter); don't drive adaptation off a polled read inside `onRender` — that stateless `if (fps < x) simplify()` is exactly the snap-back antipattern the callback exists to avoid.
- **Render-on-demand (`Context::setContinuousRendering` / `requestRedraw`, 🎯T132)** — Opt-in; ge renders every frame by default. `ctx.setContinuousRendering(false)` tells the run loop to stop drawing a *static* screen: it renders only when input arrives or `requestRedraw()` is called, and idles (blocks on `SDL_WaitEventTimeout` at ~0% CPU, never busy-spins) in between — so a still menu / level-picker costs ~no GPU / present / battery. The first input event resumes rendering on the next frame (no dropped interaction); `requestRedraw()` is thread-safe — call it from a timer / async / IAP callback to wake the idle loop and draw one frame. Both are `const` (they mutate shared per-session state), so they work on the `const Context&` `onRender` / `onEvent` receive. The consumer owns the policy of *when* the screen is static (a dirty flag, box2d body-sleep, etc.). Direct-mode (desktop / iOS / Android) only — the brokered/streaming path is unaffected (🎯T34). Backward compatible: never call it and every frame renders exactly as before. A continuous sensor stream (accelerometer / gyro, 🎯T134) does **not** force a redraw on a static screen — it isn't discrete input, and on a physical device an always-on accelerometer would otherwise render every sample; if you want sensor-driven redraws in on-demand mode, call `requestRedraw()` from `onEvent`.
- **Device-tilt parallax (`parallax()` + `SessionHostConfig.parallaxFactor`, 🎯T9)** — Reproduces the Apple Spatial Scenes effect: subtle device tilt drives a parallax offset that the game applies to its scene. Set `SessionHostConfig.parallaxFactor` > 0 to opt in (the same float controls both opt-in and sensitivity, scaling the engine's screen-XY delta before exposure). The engine maintains a 1.0 s EMA baseline so sustained tilts settle to the new neutral; `Context::parallax()` returns the recent delta as `la::float2{rotX, rotY}` in radians, suitable for `cameraOffset += ctx.parallax() * depth;` or feeding a small rotation matrix. Sensor source: iOS `CMMotionManager.deviceMotion.attitude` (sensor-fused, captures vertical-axis twist that gravity alone misses); Android `Sensor.TYPE_GAME_ROTATION_VECTOR` via JNI to the activity's `getAttitude()` (gyro+accel, no magnetometer — the EMA absorbs whatever heading reference Android picks). Desktop is a no-op (returns `{0, 0}`). Wire-mode parallax (player→server attitude streaming) is deferred until the player port lands.
- **`ge::Rect`** — `{ x, y, w, h }` float rectangle. Returned by `Context::drawSafeRectInPts()`, `Context::uiSafeRectInPts()`, and `Context::fullRectInPts()` (all in point space per 🎯T60; +y points down per SDL screen-coord convention). The Rect type is **direction-agnostic** (caller decides what +y means) and **sign-honest** (methods compute their formulas as written; signed-area rects produce well-defined non-conventional results rather than asserting). Corner accessors are direction-agnostic: `x0y0()`, `x1y0()`, `x0y1()`, `x1y1()` — first index is position along x (0 = origin, 1 = far), second along y.
  - **Constructors:**
    - `Rect{x, y, w, h}` — 4 floats directly.
    - `Rect{{.origin = {1, 2}, .size = {3, 4}}}` — `OriginSize` tagged ctor.
    - `Rect{{.a = {1, 2}, .b = {5, 6}}}` — `Corners` tagged ctor (sign-preserving; `Rect{Corners{a, b}}.normalized()` is the order-independent bbox of two points).
    - Designated init disambiguates the two tagged forms — `Rect{{1, 2}, {3, 4}}` is a deliberate compile error.
  - **Math** (v0.8.0+): `size()`, `halfExtents()`, `center()`, `area()` (signed `w * h`), `aspect()`, `empty()` (true iff `w == 0 || h == 0`; signed-area rects are *not* empty), contextual-`bool` (`if (r) ...` = non-empty) and `operator!()`, `contains(point)` (half-open `[x,x+w) × [y,y+h)`), `contains(other)`, `intersects(other)`, `intersect(other)`, `bbox(other)`, `translated(la::float2)`, `withOrigin(la::float2)` / `withSize(la::float2)`, `adjusted(Rect)` (component-wise add of all four fields — composes with the `Corners` ctor to express inset/outset/per-edge mutate as `r.adjusted({{.a = {l, t}, .b = {-r, -b}}})`), `scaled(ScalingVec)` / `scaled(ScalingScalar)` (pivot scaling around a normalized rect-local center, default `{0.5, 0.5}`), `normalized()` (positive-w/h form of a possibly-signed rect; preserves the region), `operator*(float)` / `operator/(float)`, `operator==/!=`, `Rect::centered(c, sz)`, `fitInside(la::float2 content)` (CSS `object-fit: contain` — largest sub-rect of `*this` with content's aspect ratio, centered; letterboxes / pillarboxes), `fillInside(la::float2 content)` (CSS `object-fit: cover` — smallest rect with content's aspect that covers `*this`, centered; overflows on the non-binding axis). Half-open hit-test matches point coords. There is **no `inset` / `outset` family of methods** — `adjusted + Corners` is the unified primitive, and `Context::drawSafeRectInPts` / `uiSafeRectInPts` use it internally to apply `SafeAreaInsets`.
- **linalg aliases in `ge::la`** — `ge/Linalg.h` re-exports the full `linalg::aliases` set into `ge::la` and is included by every public ge header, so games can write `ge::la::float2` / `ge::la::float4x4` / `ge::la::int3` no matter which ge header they pulled in. The sub-namespace is deliberate: keeps linalg's 96 aliases out of `ge::`'s autocomplete and preserves a clean visual marker at use sites that the type came from linalg, not ge proper. `using namespace ge::la;` brings the short forms in unqualified for game code that wants them.
- **`ge::ortho`** (🎯T54) — 2D projection builders. `ge::ortho::letterbox(content, screen)` returns a `float4x4` that maps content-space onto clip space, preserving content's aspect ratio (the dominant axis fills the screen; the opposite axis is *extended* so content drawn outside the nominal `(0..content)` rect lands in the letterbox/pillarbox bars — useful for backdrop bleed). `pixelOrtho(w, h)` is the no-aspect variant: `(0..w, 0..h)` directly to clip space, top-left origin / +Y down. `screenToContent(screenPt, content, screen)` is the inverse of `letterbox` for hit-testing under a letterboxed projection.
- **`ge::gesture`** (🎯T55) — Pure-math swipe predicates. `isHorizontalSwipe(dx, dy, threshold, dominanceRatio = 1.5f)` returns true iff `|dx| > threshold` AND `|dx| > |dy| * dominanceRatio`. `isVerticalSwipe` is the axis-swapped sibling. No platform / SDL deps; consumer accumulates per-frame deltas and decides routing.
- **`ge::layout`** (🎯T58) — Grid-row and grid-column position helpers. `gridY(rowIdx, rowH, gap, topMargin)` and `gridX(colIdx, colW, gap, leftMargin)` return positions for fixed-row stacks; both honor negative indices.
- **`ge::Button` and `ge::ButtonGroup`** (`<ge/button.h>`) — Standard touch/click button interactor with iOS-style press semantics: tap down inside highlights, drag outside un-highlights, drag back in re-highlights, release inside fires, release outside doesn't. `Button` is rendering-agnostic — consumer supplies a `hitTest` predicate (`ge::rectHitTest(rect)` for the common case, or a lambda wrapping `lunasvg::Document::elementFromPoint`, or anything else) and queries `highlighted()` from the render loop (or wires `onHighlightChange` for one-shot side effects). `tracking()` reports whether a press is in flight. `ButtonGroup` borrows a `vector<Button*>` and routes `PointerEvent`s through them with single-button-lock semantics: once a button starts tracking, all subsequent events for any finger are claimed by the group until that button returns to idle. `ge::PointerEvent` is the engine's pre-converted pointer event (`{kind, pos, id}`); SDL → PointerEvent conversion is done by `ge::input::fromSdl` (see next).
- **`ge::input::sdlPointerEventConverter(const Context&)`** + **`ge::input::fromSdl(const SDL_Event&, la::float2)`** (🎯T59, 🎯T60, `<ge/sdl_input.h>`) — Convert SDL pointer events to `ge::PointerEvent` in **point space** (🎯T60). The primary surface is `sdlPointerEventConverter(ctx)` — returns a callable bound to `ctx` that reads `ctx.fullRectInPts().size()` per call so surface-size changes (resize, orientation) are picked up automatically. `fromSdl` is the underlying free function (used by tests and callers without a Context); its second arg is `surfaceSizePts`. Returns `std::optional` — `nullopt` for non-pointer events and for touch-synthetic mouse events (`SDL_TOUCH_MOUSEID`) that duplicate a finger event. Mouse coords come from SDL in window-point space and are passed through as-is; touch coords are denormalized against `surfaceSizePts`. Mouse synthesizes a single virtual finger ID (`ge::kMouseId`); touch propagates `SDL_TouchFingerEvent::fingerID` verbatim. Consumers wiring `ge::Button` / `ge::ButtonGroup` use this once at their dispatch site instead of rewriting the SDL boilerplate.
- **`ge::LongPressWatcher`** (🎯T65.6, `<ge/long_press.h>`) — Long-press gesture detector for "secret" / debug triggers (e.g. a long-press in the top-right corner reveals the IAP debug panel). Rect-region + threshold + onFire callback. `handleEvent(PointerEvent)` from the dispatch site, `update(dt)` each frame; fires exactly once per held press after `thresholdSec`. Single-touch by design (mirrors `ge::Button`). Re-press after fire fires again on the next threshold crossing. Drift-out cancels without firing — the discipline is "hold still", not "drift in". Wiring for the IAP debug panel is a few lines in the consumer's `#ifndef NDEBUG` block: instantiate watcher + `ge::iap::DebugPanel`, route pointer events through both, render the panel's `rows()` with the game's own UI primitives.
- **`ge::SafeAreaInsets`** — Direction-agnostic per-edge insets (`y0`, `y1`, `x0`, `x1`, in **point space** after 🎯T60) describing the device chrome (camera notch, Dynamic Island, system gestures, home indicator). In ge's SDL screen-coord (y-down), `y0` = top, `y1` = bottom, `x0` = left, `x1` = right. Most games consume `Context::drawSafeRectInPts()` / `uiSafeRectInPts()` instead — the rect API is the natural shape. Reach for `Context::drawSafeInsetsInPts()` / `uiSafeInsetsInPts()` (🎯T37 + 🎯T60) only when the task is "align flush with a specific chrome edge". All four edges are 0 on platforms with no safe-area concept (desktop) and on wire-mode sessions until the player→server safe-area plumbing lands (🎯T37 follow-up). On iOS / Android, populated from `SDL_GetWindowSafeArea` in `DirectRenderHost` (SDL returns pts; no conversion needed).
- **`ge::RunConfig`** — Render loop callbacks: `onUpdate(dt)`, `onRender(const Context&)`, `onEvent(SDL_Event)`, `onShutdown()`.
- **`ge::Factory`** — `std::function<RunConfig(Context)>`.

### High refresh rate during press (🎯T63)

`DirectRenderHost` automatically requests the display's maximum refresh rate whenever a touch or mouse-button press is in flight, and releases it when all presses end. This keeps ProMotion / VRR displays (iPad mini A17 Pro, etc.) in their high-refresh state so button-highlight feedback appears within ≤1 vsync of the Down event — not after a slow-to-recover 60 Hz cycle.

**Mechanism:** `DirectRenderHost` holds one `ge::RefreshRateBoost` (internal, `include/ge/RefreshRateBoost.h`). The counter increments on every `SDL_EVENT_FINGER_DOWN` / `SDL_EVENT_MOUSE_BUTTON_DOWN` and decrements on Up / Cancel. SDL_TOUCH_MOUSEID synthetic mouse events are excluded (they duplicate finger events). On background transition (focus lost / hidden / minimized) the counter is drained to zero.

**Platform details:**
- **iOS/iPadOS** — a `CADisplayLink` with `preferredFrameRateRange(min=80, max=device_max, preferred=device_max)` is added to the run loop on 0→1 and invalidated on 1→0. CADisplayLink's presence alone is enough to hold the display at high refresh; its callback is a no-op. API-guarded: `preferredFrameRateRange` requires iOS 15+; falls back to `preferredFramesPerSecond` on older OS.
- **Android** — calls `GeActivity.setFrameRateBoost(bool)` via JNI, which uses `Surface.setFrameRate(maxFps, FRAME_RATE_COMPATIBILITY_DEFAULT)` on API 30+; no-op on older devices.
- **macOS / desktop** — no-op. Desktop displays don't VRR-throttle on idle.

Consumer apps require no code changes — the boost is entirely engine-internal.

### Rendering

- **`ge::Pass`** (`Pass.h`, 🎯T101) — move-only RAII render pass. `Context::swapchainPass()` returns this frame's swapchain pass — its ctor acquires the drawable + opens the pass, its destructor ends the pass + commits + presents; call it once at the top of `onRender` before any draws and hold it for the frame. `ge::offscreenPass(const sg_pass&)` wraps an offscreen render-to-texture pass the same way (open it *before* the swapchain pass). Exactly one swapchain pass per frame. The old loop-driven `beginFrame`/`endFrame` bracket is gone: the host now exposes a per-frame `refreshFrame` (Context update, no pass) and the consumer owns the pass via `swapchainPass()`. **Migration:** every consumer adds `auto p = c.swapchainPass();` at the top of `onRender`. The wire/brokered host (`ServerWireBridge`) is pre-existing-dead bgfx (🎯T34) and will adopt this interface when its sokol port lands.

### Android renderer backend: Vulkan with GLES fallback (🎯T107)

On Android, ge selects **Vulkan or GLES3 at runtime**, per device. Full reference: [`docs/android-renderer-backend.md`](docs/android-renderer-backend.md).

- **Why two backends.** sokol_gfx picks its backend at *compile time* (`SOKOL_VULKAN` xor `SOKOL_GLES3`), so the two implementations can't share one library. Vulkan is preferred where the device clears sokol's capability bar; GLES3 is the universal fallback (every Android GPU runs it).
- **Plugin/dispatch architecture.** `libge.a` carries a generated **dispatch shim** (`tools/sokol-dispatch/`): each of the 150 `sg_*` is a forwarder through `g_ge_sg_api`. Each backend is its own `.so` — `libgesokol-gles.so` / `libgesokol-vk.so` — built from `src/render/SokolBackend{GLES,VK}_android.c` with `SOKOL_IMPL` + `-fvisibility=hidden`, exporting one symbol `ge_sokol_bind_<backend>()` that *returns* its `sg_*` table. Both `.so` are linked into `libmain.so`; `SokolContext_android` binds exactly one. **App code is unchanged and backend-agnostic** (call sites stay `sg_*`; the asymmetry is link-time symbol resolution). Apple keeps `SOKOL_IMPL` inline (single Metal backend, no dispatch).
- **Selection.** `SokolContext_android`'s ctor runs `ge_vk_probe` (`tools/vkprobe/ge_vkprobe.c` — instance ≥1.1, device ≥1.3, `VK_EXT_descriptor_buffer` + `descriptorBuffer`/`bufferDeviceAddress`/`dynamicRendering`/`synchronization2`, graphics+present queue) and binds Vulkan, or falls back to GLES3 (probe REJECT, or any VK bring-up failure). Decision + reason logged at startup.
- **VK glue lives in libge** (`SokolContext_android.cpp`, `VkM`): instance/device/swapchain/acquire/present + `vkCmdCopyImageToBuffer` screenshot readback — all Vulkan **1.0 core + KHR** (exported by the API-26 `libvulkan.so` stub, so libge links them directly). The `.so`'s sokol IMPL needs 1.1/1.2/1.3 core, resolved at runtime via a volk-style loader (`src/render/ge_vk_loader.h`, `dlopen(libvulkan.so)` + `vkGetInstanceProcAddr`), so the `.so` has no hard libvulkan link.
- **Shaders.** `GE_SHDC_LANGS` (in `Module.mk`, `tools/prebuild.sh`, and the consumer `CMakeLists.txt.in`) **must include `spirv_vk`** — without it `*_shader_desc(sg_query_backend())` returns NULL on a Vulkan device and `sg_make_shader` aborts.
- **Vendored sokol patch.** `sokol_gfx.h`'s `_sg_vk_init_caps` casts `maxPerStageDescriptor*` (often `0xFFFFFFFF`) to `int` → `-1`, breaking all shader validation. ge patches it to min-in-`uint32`-then-cast. **Re-apply on any sokol bump** (search `🎯T107 ge patch`); TODO upstream.
- **Validated** on Adreno 830 (the device a prior *bgfx* Vulkan experiment crashed — sokol's command pattern does **not** reproduce that driver bug; see `docs/papers/adreno-830-bgfx-vulkan-crash.md`, now historical), Xclipse 940, Tensor G2 (all Vulkan), and Mali fallbacks (GLES).

### Sprites, transforms, and SVG (🎯T42, 🎯T47, 🎯T48, 🎯T49, 🎯T50, 🎯T51)

ge has a small, unified surface for "rasterize/load → texture → draw". One `Sprite` struct, one transform primitive (`ge::frame(Rect)`), four rasterization sources.

#### `Sprite` and `ge::frame` — the universal pair

- **`ge::Sprite`** (`<ge/sprite.h>`) — `{ sg_image tex; sg_view view; int width, height; }`. The output of every "X to texture" factory in ge: SVG (one-shot or live document), PNG, text, anything else. Caller owns `tex` and must `sg_destroy_image` it. Sprite's model space is the unit square `(0..1, 0..1)` with the source image filling it (u=v=0 at top-left, u=v=1 at bottom-right).
- **`Sprite::draw(mvp)`** — submits a unit-square quad covering the sprite. `mvp` is the model-view-projection matrix (unit square → clip space, e.g. `la::mul(worldToClip, ge::frame(rect))`). Premultiplied-alpha blend is baked into the pipeline. The frame's `ge::Pass` (🎯T101) must already be open; the draw submits into the active sokol pass. Compose with linalg rotation / scaling matrices for non-axis-aligned placement.
- **`ge::frame(Rect)`** (`<ge/transform.h>`) — returns a `la::float4x4` that maps the unit-square local space to the rect in parent space. Origin in the translation column, `Rect.w` / `Rect.h` as the x / y basis. **Negative `h` flips the y basis** — that is how a y-up parent space tells `frame` to put unit y=0 at the top. No separate y-up / y-down API.
- **`ge::frameCentered(center, size)`** / **`ge::frameRotated(center, size, angle)`** (🎯T56, `<ge/transform.h>`) — sister builders parameterised by center + size. `frameCentered` is `constexpr` and matches `frame(Rect::centered(c, s))`. `frameRotated` adds a rotation by `angle` radians around `center` (positive = standard 2D-math CCW; CW on-screen with top-left origin); non-`constexpr` because `std::sin/cos` aren't `constexpr` until C++26 — same caveat as `DampedRotation::matrix()`.
- **Rect-to-rect mapping** is just composition — `la::mul(frame(b), la::inverse(frame(a)))`. Use `la::mul`, not `operator*`; linalg deprecates the latter for matrices.
- **Hit testing** falls out of inversion — apply `la::inverse(modelToWorld)` to a parent-space point to get unit-square coords, then scale by `sprite.width` / `sprite.height` to get source-image pixel coords (e.g. for `lunasvg::Document::elementFromPoint`).

#### Sources of a `Sprite`

- **`ge::rasterizeSvg(svg, w, h)`** (`<ge/svg.h>`) — rasterize an SVG string via lunasvg.
- **`ge::rasterizeSvgToPixels(svg, w, h)`** (`<ge/svg.h>`) — same but returns CPU-side `SvgPixels { rgba, width, height }` (premul RGBA8). Useful for unit tests and offline image processing.
- **`ge::renderSvgDocument(lunasvg::Document& doc, w, h)`** (`<ge/svg.h>`) — render an existing `lunasvg::Document` into a Sprite. For the **interactive flow**: hold the Document alive, mutate via lunasvg's API (`applyStyleSheet`, `getElementById`, `setAttribute`, `elementFromPoint`, `querySelectorAll`, …), call this to re-rasterize after state changes. `<ge/svg.h>` re-exports `<lunasvg.h>` so consumers get the full lunasvg surface via the same include.
- **`ge::loadImage(path)`** (`<ge/png.h>`) — load PNG / JPEG / BMP / etc. via SDL3_image. Path resolved via `ge::resource` (iOS bundle / Android APK / desktop fs). Premultiplies alpha before upload.
- **`ge::imageFromSurface(SDL_Surface*)`** (🎯T57, `<ge/png.h>`) — same post-load path as `loadImage` but starts from an in-memory `SDL_Surface*`. Useful for surfaces that don't come from a file (procedural bitmaps, in-process SVG rasterization output, asset data fetched over the wire). *Takes ownership* of the surface — destroys it before returning. `nullptr` input is safe (returns null `Sprite`).
- **`ge::rasterizeText(text, font, sizePt, color)`** (`<ge/text.h>`) — single-line text via FreeType. `font` is a `FontRef` from `ge::resolveFont`. Premul output. Single line / no wrapping today.

ge ships [lunasvg](https://github.com/sammycage/lunasvg) (with its bundled plutovg) as the canonical SVG rasterizer. SDL_image's built-in nanosvg path can't render text or `clipPath`; ge bypasses it. See [NOTICES.md](NOTICES.md) for the license chain (lunasvg + plutovg + FreeType-derived raster code + stb_*).

**Lunasvg features supported:** path, rect, circle, ellipse, polygon, polyline; fills (solid, linearGradient, radialGradient, pattern); strokes; clipPath; mask; gradients; opacity; nested groups; transforms; `<text>` (basic); CSS via `applyStyleSheet`; CSS selectors via `querySelectorAll`; hit testing via `elementFromPoint`. **Not** a SVG 2 / animation engine — no SMIL, no scripting.

#### `ge::SpriteBatch` — batched draws

`<ge/sprite.h>` also provides `ge::SpriteBatch` for high-volume sprite rendering. `addSprite(modelToWorld, sprite, color)` queues a quad with the matrix applied to the unit-square corners on the CPU; `submit(view)` flushes runs of same-texture sprites in one draw call per (texture, view) pair. Premultiplied-alpha blend by default; override via `setBlendState`. UV sub-rect overload supports atlasing.

#### Patterns

**Static SVG sprite (e.g. tiltbuggy's icy pond):**

```cpp
// Init:
auto pondSvg = R"SVG(<svg xmlns="..." width="384" height="256">…</svg>)SVG";
ge::Sprite pond = ge::rasterizeSvg(pondSvg, 384, 256);

// Per frame, in y-up world:
auto p = c.swapchainPass();  // open at top of onRender
const auto m = ge::frame(ge::Rect{
    -0.24f, +0.45f,   // x = left, y = top in y-up (larger y)
     0.48f, -0.20f,   // w positive, h NEGATIVE for y-up
});
pond.draw(la::mul(worldToClip, m));   // mvp = the game's world-to-clip ∘ model

// Shutdown:
if (pond.tex.id != SG_INVALID_ID) sg_destroy_image(pond.tex);
```

**Interactive SVG panel (CSS + hit testing):**

```cpp
auto doc = lunasvg::Document::loadFromData(svgBytes);
doc->applyStyleSheet("button.active { fill: #FFA000 }");
auto sprite = ge::renderSvgDocument(*doc, 1024, 256);

// Per frame: auto p = c.swapchainPass(); sprite.draw(la::mul(worldToClip, panelModelToWorld)); as above.

// On tap (parent-space coords → unit-square via inverse, then to image pixels):
const auto inv     = la::inverse(panelModelToWorld);
const auto unit    = la::mul(inv, la::float4{tap.x, tap.y, 0, 1});
auto el = doc->elementFromPoint(unit.x * sprite.width,
                                unit.y * sprite.height);
if (el && el.getAttribute("id") == "btn-play") { /* … */ }

// On state change:
doc->getElementById("btn-play").setAttribute("class", "active");
if (sprite.tex.id != SG_INVALID_ID) sg_destroy_image(sprite.tex);
sprite = ge::renderSvgDocument(*doc, 1024, 256);
```

**SVG `<text>` and fonts** — SVGs that use `<text>` need fonts registered with lunasvg's font cache. ge handles this in two layers:

- **Lazy default registration.** On the first `rasterizeSvg*` / `renderSvgDocument` call, ge calls `ge::resolveFont("system:sans-serif")` / `serif` / `monospace` (regular and bold each) and feeds the paths to lunasvg.
- **App overrides** — call `ge::registerSvgFontFace(family, bold, italic, FontRef)` before the first rasterize, or alongside it. The path most polished games take — ship your own face, point `<text font-family="...">` at it, render. ge does NOT bundle a default TTF (no asset bloat; no engine-imposed typography).

**Apple TTC limitation** — Apple's first-party fonts (SF Pro, Helvetica, HelveticaNeue) ship as `.ttc` collections; lunasvg's public C API drops the TTC face index, so requesting bold on an Apple system font yields synthetic **faux-bold** rather than the designed Bold cut. Custom fonts (separate `.ttf` per weight) and non-Apple platforms are unaffected. Dev-time only; ship custom fonts for production typography. Upgrade path if it bites: 5-line patch to lunasvg's wrapper to thread the ttcindex through to plutovg's existing `plutovg_font_face_load_from_file(path, ttcindex)`.

### Debug overlay (🎯T97)

`<ge/debug.h>` — an opt-in, flag-toggleable debug-render layer. Accumulate
diagnostic primitives anywhere in the frame, then `flush` once after the scene.
While the overlay is disabled, every accumulation call is a cheap no-op and
`flush` does nothing, so call sites stay unconditional — flipping `enabled()` at
runtime lights the whole layer up without touching the surrounding draw code.
The public surface names no rendering backend (coords are `ge::la` vectors,
colours are `ge::Color` — straight-alpha RGBA `float4`, same as `ge::rasterizeText`);
internally it's two sokol pipelines (line list + triangle list) sharing one
program + stream buffer, mirroring `ge::Sprite` (which packs to ABGR at upload).

- **`ge::debug::enabled()` / `setEnabled(bool)`** — runtime on/off. First query
  latches the `GE_DEBUG_OVERLAY` env var (`1/true/yes/on` → enabled), else off.
- **`ge::debug::line` / `tri`** — ad-hoc world-space primitives (`la::float2` or
  `la::float3`), transformed by the `worldToClip` passed to `flush`.
- **Submit-mesh convention — `ge::debug::mesh(verts, indices, wireColor=magenta, fillColor={})`**
  (`verts` / `indices` are `std::span` — pass a `vector`, `array`, or C-array) —
  hand the same indexed triangle mesh you draw with your own pipeline; while
  enabled, ge overlays a wireframe (every edge) in `wireColor` and/or a
  translucent fill ("sector tinting") in `fillColor`. A `ge::Color` with alpha 0
  skips that layer — so the default is a magenta wireframe with no fill; pass a
  `fillColor` for a tint, or both for filled-and-outlined (fill under wire) in one
  call. Out-of-range indices and a non-multiple-of-3 tail are skipped with a warning.
- **`ge::debug::box(rect, wireColor=magenta, fillColor={})`** — convenience
  shape built from `line`/`tri`, same two-colour convention (alpha-0 skips a
  layer); an axis-aligned `ge::Rect` in the z=0 plane.
- **`ge::debug::circle(center, radius, wireColor=magenta, fillColor={}, quality=0.5pt, minVerts=0)`**
  — you specify a **quality** (max on-screen polygon↔circle gap in pt), not a
  vertex count; the engine resolves the count at `flush` against `worldToClip`,
  so circles stay smooth zoomed in and cheap zoomed out. The polygon is
  *balanced* (vertices ~quality outside, edge midpoints ~quality inside),
  halving the count vs. an inscribed polygon. `minVerts` floors the count (hard
  floor 3). Like `point`, it adds a `circleItemCount` until flushed.
  `ge::debug::segmentsForQuality(radiusPt, qualityPt, minVerts)` exposes the
  policy for callers tessellating their own arcs.
- **`ge::debug::point(pos, color=magenta)`** (`la::float2` / `la::float3`) — a
  position marker held at a **fixed perceptual size** (`kPointSizePt`, 2 pt
  across) however far `worldToClip` zooms, so point clouds stay legible. Drawn
  as a tiny filled disc — internally a fixed-pt `circle` run through the shared
  `expandCircle` tessellator with a ≥8-vertex floor (so it reads round even
  tiny), not a bespoke quad. `pos` is projected through `worldToClip`; at
  `flush` the world radius that yields `kPointSizePt` pt is solved and the disc
  expanded (like `text`), so it adds a `pointItemCount`, not tri/line verts,
  until flushed. Single fill layer (alpha-0 = no-op). Reach for `circle` instead
  when you want a dot that scales with the world.
- **`ge::debug::text(posPx, str, color)`** — single-line monospace HUD text in
  framebuffer-pixel space (top-left origin), independent of `worldToClip`.
- **`ge::debug::flush(const Context&, worldToClip)`** — draw + clear everything
  queued, into the active render pass; call once per frame after the scene. When
  the overlay is enabled, `flush` also draws a smoothed FPS readout in the
  top-right corner, even if no other debug primitives were queued. GPU resources
  are created lazily on first enabled flush.

`sample/tiltbuggy` demonstrates all three surfaces end-to-end (sector-tinted
playfield, buggy-chassis wireframe, tilt HUD); run it with `GE_DEBUG_OVERLAY=1`.

### Protocol

- **`Protocol`** (`Protocol.h`) — Wire protocol structs (`DeviceInfo`, `SafeAreaUpdate`, `AspectLock`, `MessageHeader`) and magic number constants. Header-only.

### Assets

- **`ManifestSchema`** (`ManifestSchema.h`) — JSON-serializable types: `MeshRef`, `ModelDef<Meta>`, `ManifestDoc<Meta>`. Templated on application-specific metadata.
- **`ModelFormat`** (`ModelFormat.h`) — `ge::MeshVertex` struct (x, y, z, u, v).
- **`Model`** (`Model.h`) — Associates mesh data with metadata.

### Animation

- **`GlobeController`** (`GlobeController.h`) — Encapsulates `DampedRotation` + drag state + input source arbitration (mouse vs finger). `event()` handles SDL touch/mouse events, `update(dt)` flushes drag accumulation and applies inertia. Supports two-finger pinch-rotate (log-scale zoom delta via `consumePinchDelta()`, rotation around camera view axis). `setSensitivity()` and `setDamping()` for runtime tuning. `pinching()` getter for pinch state. Velocity decays to zero during stationary drag — no residual momentum on release. **Camera-agnostic (🎯T122/T123):** `setCameraBasis(right, up, view)` declares the consumer's world-space camera axes — horizontal drag spins about `up`, vertical about `right`, two-finger twist about `view`. Defaults to the Z-up convention (tiltbuggy: `{1,0,0}, {0,0,1}, {0,1,0}`); a Y-up consumer (esfera) sets `{1,0,0}, {0,1,0}, {0,0,-1}`, with no per-app axis literals. Header-only.
- **`DampedRotation`** (`DampedRotation.h`) — Quaternion orientation + angular velocity with exponential decay. Supports screen-space drag, inertia, framerate-independent damping (`damping^(60*dt)`). `applyDrag(dx, dy, sensitivity, screenUp, screenRight)` / `updateVelocityFromDrag(...)` take the consumer's camera basis (🎯T122/T123) — horizontal drag rotates about `screenUp`, vertical about `screenRight`; both default to the Z-up convention so existing callers are unchanged. `setDamping()` for runtime tuning. `isMoving()` checks if velocity is above threshold. `matrix()` returns the 4x4 rotation matrix for rendering.
- **`DampedValue`** (`DampedValue.h`) — 1D value + velocity with exponential decay.
- **`DeltaTimer`** (`DeltaTimer.h`) — Frame delta-time helper.

### Tweak System

- **`Tweak<T>`** (`Tweak.h`) — Generic runtime-tunable parameter with atomic `shared_ptr` for lock-free reads. Specialized types: `EnumTweak` (int with named labels for dropdown UI), `Vec2Tweak` (float2 with per-axis screen direction via `Dir` enum), `AxisTweak` (float with drag axis vector encoding direction+sensitivity), `Color` (float4 alias). Database: `loadOverrides()` opens SQLite DB and applies saved values, `save()` persists a tweak, `resetOne()`/`resetAll()` restore defaults. JSON API: `allToJson()` emits name, value, default, and type-specific metadata; `parseAndApply()` sets a value from JSON; `parseAndReset()` resets by name or all. Global generation counter (`generation()`) increments on every `set()` for change tracking. Header-only, in `tweak::` namespace.

### Audio (🎯T7 / 🎯T43)

- **`ge::audio`** (`<ge/audio.h>`) — Engine-driven pause/resume for SDL audio devices. See the "Audio Lifecycle" section above for the full description. Key surface:
  - `registerDevice(SDL_AudioDeviceID)` → RAII `Registration`; unregisters on drop.
  - `pauseAll()` / `resumeAll()` — manual control (engine uses automatically).
  - `state()` → `FocusState::Active` or `FocusState::Paused`.
  - `onBackground()` / `onForeground()` / `onAudioFocusLost()` / `onAudioFocusGained()` — engine-internal lifecycle hooks called by DirectRenderHost and AVAudioSession observers.

### Platform

- **`Resource`** (`Resource.h`) — `ge::resource(path)` resolves asset paths. Returns the path unchanged on desktop; prepends the iOS app bundle `Resources/` directory on iOS. Header-only.
- **`SdlContext`** (`SdlContext.h`) — RAII SDL3 window creation. Used by the player; not typically used by the server. pImpl.
- **`Signal`** (`Signal.h`) — SIGINT handler registration for graceful shutdown.

### Logging (🎯T66)

`ge::run` installs a platform-native spdlog sink at startup so consumer
apps' `SPDLOG_INFO` / `WARN` / `ERROR` calls surface uniformly without
per-app sink wiring. Consumers write normal spdlog macros and capture
from the host with platform tooling:

| Platform | Sink                | Capture |
|---|---|---|
| iOS / iPadOS / tvOS / watchOS | `os_log_with_type` via `os_log_create(<bundle-id>, "ge")` | `spyder log <udid> --process <CFBundleExecutable>` (live, `--follow`); Xcode Console over USB. |
| Android  | `__android_log_print` with tag = `<package-name>` (truncated to 23 chars) | `adb -s <serial> logcat -s <package-name>` or `spyder log <serial>`. |
| Desktop  | spdlog default colour-stderr (unchanged) | stderr. |

spdlog → native level mapping:

| spdlog       | Apple `os_log_type_t`     | Android `__android_log_print` priority |
|---|---|---|
| trace, debug | `OS_LOG_TYPE_DEBUG`       | `ANDROID_LOG_VERBOSE` / `ANDROID_LOG_DEBUG` |
| info         | `OS_LOG_TYPE_INFO`        | `ANDROID_LOG_INFO`    |
| warn         | `OS_LOG_TYPE_DEFAULT`     | `ANDROID_LOG_WARN`    |
| err          | `OS_LOG_TYPE_ERROR`       | `ANDROID_LOG_ERROR`   |
| critical     | `OS_LOG_TYPE_FAULT`       | `ANDROID_LOG_FATAL`   |

**Why this lives in ge.** Apple's privacy-by-default unified-log behaviour
hides `os_log` arguments as `<private>` unless every format specifier
carries `%{public}`, and entries logged via `OS_LOG_DEFAULT` (no named
subsystem) are filtered out of remote capture entirely. Apps that rolled
their own `NSLog`-based sink ended up emitting nothing visible. ge's
sink works around both: it creates a named subsystem with the consumer
app's bundle ID and passes the spdlog-rendered payload as one
`%{public}s` argument, so every value is visible without per-call
`%{public}` boilerplate at the call site.

- **`ge::log::install(subsystem = "")`** (`log.h`) — idempotent. Called
  automatically from `ge::run`; consumers don't invoke it directly.
  Empty `subsystem` auto-detects: `[[NSBundle mainBundle] bundleIdentifier]`
  on iOS, `Activity.getPackageName()` via SDL's JNI bridge on Android,
  falls back to `"ge"` otherwise.

### App Channel (🎯T92)

Dev-only bidirectional MessagePack-RPC channel to spyder's `app_*` MCP tools
(≥ v0.53.0), making every ge app agent-drivable: pause/step/speed, input
injection, state query + save/restore, screenshot, clean quit, structured
log + perf push. Activated when `SPYDER_APP_CHANNEL` is `host:port` (🎯T119; the
old `LOG_TARGET` plain-text sink is gone — spyder v0.58.0 dropped its listener,
and the app-channel is the only log path). `ge::run` dials it automatically.
Entire feature compiled out under `NDEBUG` (same gate as T83) — no socket, no
msgpack, no handlers in release. **Full reference: `agents-guide.md` → "Agent-drivable app channel".**

**State slices are ge's blessed telemetry/metadata pipe (🎯T115).** Apps
`registerStateSlice(name, getter)` for any number of app-defined slices (ge
hard-codes none); spyder ≥ v0.56.0 discovers them (`app_state_slices`), pulls one
(`app_state{slice}`), or watches a slice evolve over an `app_input` sequence
(`app_state_capture_*` — a spyder-side poller of the existing `state_query`, so no
extra app-side method is needed). A recommended geometry/physics slice shape
(`bodies` with pos/vel, `constraints` with rest/current length, `sensors` with
distance-to-nearest) lets spyder render/compare physics uniformly across games —
a convention, not a requirement. `sample/tiltbuggy` ships a `geometry` slice as
the in-repo proving ground. Consumers may also volunteer a representative
`example` payload per slice (optional third arg to `registerStateSlice`, 🎯T116),
emitted in the hello as a `{name, example}` descriptor (spyder ≥ v0.57.0) so an
agent gets a filter-writing template at connect time; slices without one keep the
compact bare-string form. **box2d consumers** populate a physics slice in one
line with `ge::box2d::worldGeometry(worldId)` (header-only `<ge/box2d_slice.h>`,
🎯T117) — it walks the whole `b2World` via `b2World_OverlapAABB` and emits the
geometry schema (body names → `id`s, shape outlines under `shapes`);
`ge::box2d::body(label, id)` is the curated single-body form. `libge` stays
box2d-free (the header is consumed only by apps that already link box2d). See
`agents-guide.md` → "State slices" for the schema and the iOS
local-network-permission gotcha.

- **`<ge/appchannel.h>`** — `registerMethod`, `installFromEnv`, `push`,
  `active`, `applyTimeControl` (run-loop pacing), `perfEmit` (custom perf
  counter), `registerStateSlice` / `registerStateSerializer` (consumer state
  registry — register *before* `ge::run`; getters run on the game thread),
  `pumpMainThreadTasks` (run-loop drains marshalled state tasks). Transport in
  `src/appchannel.cpp`; per-platform screenshot readback in
  `SokolContext::captureNextFrame` (Metal blit on Apple, `glReadPixels` on
  Android) bridged via `src/render/ScreenshotBridge.h`.

### I/O

- **`FileIO`** (`FileIO.h`) — `ge::openFile(path)` returns a `std::unique_ptr<std::istream>`. Uses `SDL_IOFromFile` internally for platform-agnostic file access (Android APK assets, iOS bundles, normal filesystem).
- **`WebSocketClient`** (`WebSocketClient.h`) — Async WebSocket client used by `SessionHost` to connect to ged. pImpl.

### Video

- **`VideoEncoder`** (`VideoEncoder.h`) — H.264 encoder interface. Platform implementation: `VideoEncoder_apple.mm` (VideoToolbox). Used internally by `SessionHost` when running headless. pImpl.
- **`VideoDecoder`** (`VideoDecoder.h`) — H.264 decoder interface. Platform implementation: `VideoDecoder_apple.mm` (VideoToolbox). Used by the player. pImpl.

### IAP (🎯T65)

In-app purchases with one cross-platform surface: games register a catalogue, query `owned()` in O(1), call `buy()` with a callback. Same code compiles on macOS (StubStore), iOS (StoreKit 2, T65.2), and Android (Play Billing, T65.3) without `#ifdef`.

```cpp
#include <ge/iap.h>

ge::iap::setCatalogue({
    {.id = "pro",      .type = ge::iap::Type::NonConsumable},
    {.id = "hints_10", .type = ge::iap::Type::Consumable},
});

if (ge::iap::owned("pro")) { /* gate */ }

ge::iap::buy("pro", [](ge::iap::Result r) {
    if (r.ok) celebrate();
});

ge::iap::restore([](auto){ });  // Apple App Review requires a "Restore Purchases" button — route here.
```

**Backend selection** at process startup from `GE_IAP_MODE`:

| Mode | Backend | When |
|---|---|---|
| `stub` (default on desktop) | `StubStore` — in-memory entitlement set, no platform calls | CI, unit tests, headless server runs |
| `local` | `.storekit` (iOS) / `android.test.*` (Android) — real framework, fake products | Fast dev iteration (T65.4) |
| `platform` (default on mobile) | Real StoreKit / Play Billing | Release. Sandbox vs production decided by binary signing, not by code |

**Product IDs are local.** Game registers `"pro"`, ge prepends the bundle ID to form the platform SKU `com.squz.tiltbuggy.pro`. The same string is what gets registered in App Store Connect and Play Console — no separate mapping table.

**Explicit per-platform SKU override (🎯T67).** Legacy / migration apps that can't fit the auto-prefix convention pass an optional `sku` field on `Product`:

```cpp
ge::iap::setCatalogue({
    // Greenfield product — auto-prefix:
    //   iOS     → com.squz.tiltbuggy.pro
    //   Android → com.squz.tiltbuggy.pro
    {.id = "pro", .type = ge::iap::Type::NonConsumable},

    // Legacy product — explicit verbatim SKUs on each platform:
    {
        .id   = "allpacks",
        .type = ge::iap::Type::NonConsumable,
        .sku  = ge::iap::SkuMapping{
            .apple   = "com.squz.multimaze.allpacks",
            .android = "com_squz_multimaze_allpacks",
        },
    },

    // Mixed mode — iOS inherits legacy, Android is greenfield (auto-prefix):
    {
        .id   = "grandmaster_pack",
        .type = ge::iap::Type::NonConsumable,
        .sku  = ge::iap::SkuMapping{.apple = "com.squz.multimaze.grandmaster.001"},
    },
});
```

Game-side code keeps using the local id everywhere — `owned("allpacks")`, `buy("grandmaster_pack", ...)`, `testing::setOwned("allpacks", true)`. The mapping mechanic is hidden from the testing API and from `StubStore`'s entitlement index; only `AppleStore` and `AndroidStore` consult it when forming the platform SKU sent to the store and when reversing a transaction-update back into a local id.

**Testing surface** (`ge::iap::testing::setOwned`, `clearAll`) is authoritative on StubStore, no-op on platform stores. Used by unit tests and the in-engine debug menu (T65.6) for inner-loop iteration without touching a real store.

**Server-side receipt validation is intentionally not provided.** Modern frameworks return signature-verified transactions (StoreKit 2 JWS, Play Billing signed receipts); ge writes only verified entitlements to the cache. The JWS floor covers replay, bundle-ID-binding, and account-binding for the threat model that paid non-consumable unlocks against casual game audiences actually face. Add server-side validation when shipping subscriptions or high-value-currency consumables.

**Revoked entitlements are pruned (🎯T126).** The entitlement cache reconciles *clear-then-populate*: the post-launch StoreKit `currentEntitlements` walk — and Restore Purchases, which re-walks after `AppStore.sync()` — rebuilds the cached set from only what the store currently credits. So a non-consumable that was refunded, revoked, or Family-Sharing-removed makes `owned()` return false once the walk completes, and the reconciled set is what gets persisted, so the revocation doesn't survive into the next launch (the Apple Keychain item outlives app reinstall, which is exactly why a stale insert-only cache was a bug). The platform-agnostic reconciliation lives in `src/iap_entitlement_cache.h` (`EntitlementCache`, unit-tested on desktop since the platform stores don't compile off-device); the Android in-memory cache mirrors the same reconcile on Restore.

**Apple backend uses StoreKit 2 (🎯T68).** `iap_apple.mm` (C++) drives a Swift worker (`iap_apple.swift`, class `GEStoreKit2BridgeImpl`) via the Obj-C protocols declared in `iap_apple_bridge.h`. The Swift side handles:

| StoreKit 2 surface | What ge gets |
|---|---|
| `Transaction.updates` listener | Silent auto-restore on launch — no user gesture needed |
| `Transaction.currentEntitlements` | Entitlement cache populated within ~1 s of process start |
| `Product.purchase()` + `VerificationResult` | On-device JWS signature verification; `.unverified` transactions are logged and dropped |
| `AppStore.sync()` | The Restore Purchases path (App Store Review §3.1.1 still requires the button) |

The bridge is wired into the iOS Xcode project via CMake (`enable_language(Swift)` + `XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER` pointed at `src/iap_apple_bridge.h`). `iap_apple.mm` instantiates the Swift class via `NSClassFromString(@"GEStoreKit2BridgeImpl")` so no autogenerated `<Target>-Swift.h` import is needed in C++. Macros and Module.mk are unaffected — macOS desktop builds compile `iap_apple.mm` to a `makePlatformStore() returns nullptr` stub and skip the Swift file entirely.

**Adding new SK2 surface:** extend the `GEStoreKit2Bridge` / `GEStoreKit2Listener` protocols in `iap_apple_bridge.h`, add the Swift implementation in `iap_apple.swift`, route from C++ in `iap_apple.mm`. The protocol-typed `id<GEStoreKit2Bridge>` keeps C++ type-checked.

**LocalStore mode on iOS (🎯T65.4).** When `GE_IAP_MODE=local`, `iap_apple.mm` instantiates `GEStoreKit2LocalBridgeImpl` (from `iap_apple_local.swift`) instead of `GEStoreKit2BridgeImpl`. The local bridge starts an `SKTestSession` pointed at `StoreKit.storekit` in the app bundle, then delegates all product/purchase calls to the production bridge — the session intercepts StoreKit calls globally. Consumer setup:

1. Run `make ge/storekit-init` to copy `ge/ios/StoreKit.storekit` into `ios/StoreKit.storekit`.
2. Edit the file to match your `setCatalogue()` registration (remember: ge auto-prefixes local IDs, so `"pro"` → `"com.squz.mygame.pro"`).
3. In Xcode: **Build Phases → Copy Bundle Resources** → add `StoreKit.storekit`.
4. In Xcode: **Build Phases → Link Binary With Libraries** → add `StoreKitTest.framework`, set to **Optional**.
5. Set `GE_IAP_MODE=local` in the Xcode scheme environment variables.

See `docs/iap-testing.md` for the full LocalStore walkthrough and Android `android.test.*` SKU mapping.

- **`<ge/iap.h>`** — Public surface. `Type`, `Product`, `SkuMapping`, `LocalisedProduct`, `Result`, `setCatalogue`, `owned`, `products`, `buy`, `restore`, `testing::setOwned`, `testing::clearAll`.

### Testing

- **`ImageDiff`** (`ImageDiff.h`) — `imgdiff::compareCPU()` for pixel-level RMS comparison.

## Tests

Unit tests use doctest. `ge/src/main_test.cpp` provides the test runner; other `*_test.cpp` files register test cases.

```bash
make unit-test    # Build and run ge unit tests
```

## Namespaces

- `ge::` — Engine types and functions (`run`, `Context`, `RunConfig`, `resource`, `MeshVertex`)
- `wire::` — Protocol constants and structs (`DeviceInfo`, `MessageHeader`, `kVideoStreamMagic`, etc.)
- `tweak::` — Tweak system (`Tweak<T>`, `EnumTweak`, `Vec2Tweak`, `Color`)
- `imgdiff::` — Image comparison utilities
- Top-level — `DampedRotation`, `DampedValue`, `DeltaTimer`, `SdlContext`

## Working with Claude Code

### Modifying the engine

Changes to `ge/` affect all apps that consume it. After modifying engine code, rebuild both the app and player to ensure compatibility:

```bash
make && make player
```

### Modifying the player

The player entry point is `ge/tools/player.cpp`. The Apple capture backend is `ge/tools/player_capture_apple.mm`. Mobile player ports (`ge/tools/ios/`, `ge/tools/android/`) are dormant pending 🎯T34 (sokol_gfx rewrite).

The player is app-agnostic — it decodes and displays whatever H.264 stream it receives. Avoid adding app-specific logic to the player.

### Modifying the streaming protocol

Protocol changes require updating both `SessionHost.mm` (server side) and `player.cpp` (player side) in lockstep. Bump `kProtocolVersion` in `Protocol.h` when making breaking changes.

### Adding a new public API class

1. Header in `include/ge/ClassName.h`
2. Implementation in `src/ClassName.cpp` (or `.mm` for ObjC++)
3. Use pImpl for classes that pull in sokol/SDL/asio headers (see parent project's CLAUDE.md for pImpl guidelines)
4. Add to `ge/SRC` in `Module.mk` if it's a new source file
5. Update this CLAUDE.md's Public API section

### Mobile smoke testing

**Before asking the user whether something is rendering on a mobile device, exhaust all programmatic checks first.** The user cannot easily tell you what's on screen during an automated workflow — treat "ask user to look at device" as a last resort, not a first step.

The smoke test script uses the **spyder CLI** for all device-side operations. Spyder must be installed and `spyder serve` must be running (or auto-started) before running the script. After building and deploying to a device/simulator, run:

```bash
# Named device (inventory alias)
ge/tools/smoke-test.sh --device Pippa

# Selector predicate — any booted iPad sim running iOS 18+
ge/tools/smoke-test.sh --device platform=ios-sim,model=ipad,os>=18

# Selector predicate — Android emulator
ge/tools/smoke-test.sh --device platform=android-emu

# Desktop player (no device checks)
ge/tools/smoke-test.sh --platform desktop

# Sole connected device (errors if multiple)
ge/tools/smoke-test.sh
```

The `--device` flag accepts any spyder selector:
- **Inventory alias**: a name registered in `~/.spyder/inventory.json` (e.g. `Pippa`).
- **Selector predicate**: comma-separated `key=value` pairs understood by `spyder reserve --on`
  (e.g. `platform=ios-sim,model=ipad`, `platform=android,os>=12`).
- **Raw UUID / serial**: passed directly to spyder subcommands.

Use `--install <path>` to ensure the device runs the latest build. This performs an atomic
terminate → install → launch → verify-pid via `spyder deploy`:

```bash
# Deploy latest iOS build before testing
ge/tools/smoke-test.sh --device Pippa \
    --install ios/build/xcode/Debug-iphoneos/YourWorld.app

# Deploy iOS Simulator build
ge/tools/smoke-test.sh --device platform=ios-sim,model=ipad \
    --install ios/build/xcode/Debug-iphonesimulator/YourWorld.app

# Deploy Android APK
ge/tools/smoke-test.sh --device platform=android-emu \
    --install ge/tools/android/app/build/outputs/apk/debug/app-debug.apk
```

Without `--install`, the script checks passively (is the app installed? is it in the foreground?). **Always prefer `--install`** after a rebuild to avoid debugging stale binaries.

The script checks, in order:

1. **ged reachable** — port listening, `/api/info` responds, game server connected, active session count
2. **Game server running** — process alive (if `--server-pid` given)
3. **Device reachable** — `spyder devices` / `spyder resolve` confirms device found and connected; branches on spyder exit codes (11 = not found, 12 = not connected, 40 = trust not granted, 41 = developer mode off, 42 = locked)
4. **App deployed and running** — with `--install`: `spyder deploy` (atomic terminate→install→launch→PID-verify); without: `spyder list-apps` + `spyder device-state` for foreground app
5. **Player connected** — polls ged `/api/info` for active sessions (up to `--timeout` seconds)
6. **Player logs** — `spyder log <device>` filtered to error/fatal/crash; falls back to `~/Library/Logs/DiagnosticReports` crash scan

Each check prints PASS/FAIL/WARN. The script exits non-zero if any check fails. **Do not ask the user about visual output until this script passes.** If it fails, read the spyder exit code in the FAIL message and branch accordingly:

| Exit code | Meaning | Action |
|-----------|---------|--------|
| 10 | spyder daemon unreachable | Run `spyder serve` |
| 11 | device not in inventory | Check alias spelling; run `spyder devices` |
| 12 | device not connected | Plug in / boot the device |
| 20 | app not installed | Re-run with `--install` |
| 21 | install failed | Check signing / provisioning profile |
| 22 | launch failed | Check bundle ID; look at crash logs |
| 24 | PID verify failed | App crashed at startup — check logs |
| 30 | timeout | Increase `--timeout` or check device health |
| 40 | trust not granted | Accept the "Trust this Computer" dialog |
| 41 | Developer Mode off | Enable in iOS Settings → Privacy & Security |
| 42 | device locked | Unlock the device |

Only after the smoke test passes and the problem is still unclear, ask the user what they see — but state what you already verified: "Smoke test passed (ged connected, player session active, no crash reports) — can you confirm whether the globe is rendering?"

**Device preference**: When the user tells you which device to test on (e.g. "use Pippa"), save it to auto-memory so you remember across sessions. Always pass the preferred device via `--device`. If the smoke test fails because that device isn't found, tell the user which device was expected and list the devices that *are* available (the script prints them via `spyder devices`), then ask how they'd like to proceed.

### Post-run device power state check (matrix cells)

Soak-style matrix cells call `spyder device_power_state <device>` after all sub-checks complete (🎯T33.4). This guards against a class of false-pass where the device auto-locked during the soak — rendering the soak result meaningless.

Qualifying cells: all `*-device-*` and `*-emu-*` player cells (🎯T25 reconnect), `android-emu-*-dist` (🎯T28.4 AccelSynth), and any future cell with a soak phase longer than 60 s.

**Exit-code semantics**:
- Exit `0` — all sub-checks passed and device was `awake` post-soak (or state was `unknown`, which is no-signal).
- Exit `1` — one or more sub-checks failed (normal cell failure).
- Exit `50` — all sub-checks passed, but `device_power_state` returned `display_off` or `asleep`. The soak ran against a sleeping device; its result is unreliable. CI should report this as "FAIL (device fell asleep)" rather than a plain pass or fail.

**State handling**:
- `awake` → pass, no action.
- `unknown` → warn but do not fail; `unknown` means tunneld was unavailable or developer mode was off — absence of evidence, not evidence of absence.
- `display_off` / `asleep` → exit 50.

The probe uses spyder's DVT Screenshot instrument, which does not itself reset the idle timer, so calling it at the end of a soak does not make the test self-defeating.

### Visual regression

ge uses spyder's `screenshot` + `diff` commands to catch rendering regressions in matrix cells. Two cells wire in visual regression checks today:

- `ios-sim-tablet-dist` — after the soak + bg/fg pass, before `check_clean_exit`
- `android-emu-tablet-dist` — same position

The check is implemented in `ge/tools/visual-regression.sh` (sourced by `matrix-cell.sh`). It calls `spyder screenshot <device>` to capture the current screen, then `spyder diff` to compare against the stored baseline.

**Where baselines live**: spyder stores baselines in `~/.spyder/visualdiff/<suite>/<case>/` on the developer's machine. They are **not** committed to ge's source tree — binary PNG baselines require their own large-binary git practice, and spyder's own data directory is the canonical location. The trade-off is that baselines are not visible in PR diffs; a note in the PR body explains the baseline-storage choice.

- Suite: `ge-tiltbuggy`
- Case: the matrix cell name (e.g. `ios-sim-tablet-dist`, `android-emu-tablet-dist`)

**Updating baselines**: after a deliberate rendering change, run:

```bash
cd sample/tiltbuggy
make update-baselines
```

This re-runs the two cells with `VR_UPDATE_MODE=1`, which calls `spyder baseline_update` instead of `spyder diff` at the capture point, writing a new PNG to `~/.spyder/visualdiff/ge-tiltbuggy/<cell-id>/...`.

**Pixel tolerance**: the default is `VR_PIXEL_TOLERANCE=8` (on a 0–255 scale). To override for a specific run:

```bash
VR_PIXEL_TOLERANCE=12 make cell.ios-sim-tablet-dist
```

Or when updating baselines:

```bash
VR_PIXEL_TOLERANCE=12 make update-baselines
```

**Exit code 51**: a visual regression mismatch causes the cell to exit with code 51 (deliberately outside spyder's reserved 10–42 range). The artifact directory for the cell contains `vr-<cell>-report.json` (spyder's diff report) and `vr-<cell>-<ts>.png` (the captured screenshot) for inspection.

**Coexistence with imgdiff**: ge's `imgdiff` utility (built by `make ge/imgdiff`) performs byte-exact RMS comparison against committed reference images in `test/refs/`. The two systems complement each other: spyder diff for high-level visual regression (full-frame, device-level screenshots), imgdiff for pixel-exact comparison of specific rendered outputs. Neither replaces the other.

**spyder not installed**: if `spyder` is not in `PATH`, the visual regression check emits a WARN (not a FAIL) and the cell continues. This keeps the cell runnable in environments without spyder. Install spyder and run `spyder serve` before relying on visual regression in CI.

### Spyder pool (one-time setup)

The matrix's iOS sim and Android emu cells pay a ~10–30 s boot cost when no pre-warmed instance exists. `tools/spyder-pool.yaml` commits pool templates for the three matrix platforms so spyder keeps ready instances around.

**One-time setup** (per developer machine, requires spyder v0.17.0+):

```bash
make pool-init
```

This symlinks `tools/spyder-pool.yaml` to `~/.spyder/pool.yaml`, restarts the spyder daemon, and warms one instance per template (~30–60 s total). The symlink means any future changes to `tools/spyder-pool.yaml` take effect on the next daemon restart without re-running `pool-init`.

**Drain** (optional — frees emulator resources when not in use):

```bash
make pool-drain
```

This shuts down all pool instances and removes the symlink.

**Pool templates** (defined in `tools/spyder-pool.yaml`):

| Template | Platform | Device | Runtime |
|----------|----------|--------|---------|
| `ios-phone` | iOS sim | iPhone 16 Pro | iOS 18.4 |
| `ios-tablet` | iOS sim | iPad Air 11-inch (M4) | iOS 18.4 |
| `android-phone` | Android emu | Pixel 9 Pro XL | android-36, google_apis_playstore |

The pool is transparent to the matrix — cells acquire a device normally, and spyder's resolver prefers a warm pool instance over creating a fresh one.
