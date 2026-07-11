# ge

Reusable C++20 rendering engine for Squz games. Built on **sokol_gfx** + **SDL3**
(Metal on Apple; Vulkan with GLES3 fallback on Android). Consumed as a git
submodule via `Module.mk`.

**Dev control plane is [spyder](https://github.com/marcelocantos/spyder)** —
device inventory, launch, tweaks, logs, screenshots, and the optional H.264
stream relay. The historical `ged` daemon has been removed (🎯T145 / Plateau P).
Do not start `ged`; there is no `make ged` / `bin/ged`.

Full agent/developer guide: [`AGENTS.md`](AGENTS.md). Agent-drivable app-channel
and state slices: [`agents-guide.md`](agents-guide.md).

## Architecture

| Layer | Owner |
|---|---|
| Simulation, render, encode, wire schema, app-channel **client** | **ge** |
| Inventory, launch, reserve, inspect, tweak, logs, dashboard, stream **relay** | **spyder** |

**Primary modality — direct:** `ge::run` opens a local window (or mobile
surface). Dev builds dial spyder's app-channel when `SPYDER_APP_CHANNEL` is set
(injected by `spyder launch`). No streaming required for inspect/tweak/logs.

**Optional — server stream (dev only):** a `GE_SERVER=1` build dials spyder's
H.264 relay; a native player attaches through spyder. Release (`NDEBUG`) builds
compile the app-channel out; streaming lives only in the server-mode variant.

## Quick start

```bash
# Control plane (once per machine)
brew services start spyder   # or: spyder serve

# Sample (from this repo)
make                         # builds sample/tiltbuggy
make -C sample/tiltbuggy run # or: bin/tiltbuggy from the sample tree
make unit-test               # doctest suite
```

## Integrating into an app

```makefile
BUILD_DIR := build
CXX := clang++

-include ge/Module.mk
ge/Module.mk:
	git submodule update --init --recursive

CXXFLAGS := -std=c++20 -O2 $(ge/INCLUDES)
# … link against $(ge/LIB) $(ge/SDL_LIBS) and platform frameworks
```

```cpp
#include <ge/SessionHost.h>

int main() {
    MyState state;
    ge::run([&](ge::Context ctx) -> ge::RunConfig {
        auto app = std::make_shared<MyApp>(ctx);
        return {
            .onUpdate   = [&, app](float dt) { app->update(dt, state); },
            .onRender   = [&, app](const ge::Context& c) {
                auto p = c.swapchainPass();  // open before any draws
                app->render(state, c);
            },
            .onEvent    = [&, app](const SDL_Event& e) { app->event(e, state); },
            .onShutdown = [&, app]() { app->shutdown(); },
        };
    });
}
```

See `sample/tiltbuggy/` for a complete reference app.

## Dependencies

Vendored under `vendor/` (submodules / amalgams):

- [sokol](https://github.com/floooh/sokol) — `sokol_gfx` (Metal / Vulkan / GLES3)
- [SDL3](https://libsdl.org/) + SDL3_image + SDL3_ttf
- [spdlog](https://github.com/gabime/spdlog), [linalg.h](https://github.com/sgorsten/linalg), [doctest](https://github.com/doctest/doctest)
- [lunasvg](https://github.com/sammycage/lunasvg) (SVG), box2d (optional consumer), sqlite/sqlpipe
- [Triangle](https://www.cs.cmu.edu/~quake/triangle.html) — **opt-in only**; not linked into `libge.a`. Non-commercial license — see [`NOTICES.md`](NOTICES.md)

## Structure

```
include/ge/     Public headers
src/            Implementation + *_test.cpp
tools/          Player, prebuild, matrix, ship, icon-gen, iOS/Android templates
sample/         In-tree apps (tiltbuggy is the reference)
prebuilt/       Cross-arch libge.a + manifests
Module.mk       Consumer build contract
AGENTS.md       Canonical project instructions
```

## Public surface (pointers)

| Area | Headers |
|------|---------|
| Session / entry | `SessionHost.h` — `ge::run`, `Context`, `RunConfig` |
| Render | `Pass.h`, `sprite.h`, `svg.h`, `png.h`, `text.h`, `debug.h` |
| Input / UI | `button.h`, `sdl_input.h`, `gesture.h`, `long_press.h` |
| Animation | `GlobeController.h`, `DampedRotation.h`, `DampedValue.h` |
| Platform | `audio.h`, `iap.h`, `log.h`, `Resource.h` |
| Dev channel | `appchannel.h` (compiled out under `NDEBUG`) |
| Wire protocol | `Protocol.h` (`wire::`), player/bridge headers |

Stability catalogue (pre-1.0): [`STABILITY.md`](STABILITY.md).

## Player stream address

Native player default port is **3030** (spyder `serve`). Prefer
`stream_addr` / `GE_STREAM_ADDR` (legacy aliases: `ged_addr` / `GE_DAEMON_ADDR`).

```bash
# Desktop
bin/player --host 127.0.0.1 --port 3030 --name <server-name>

# iOS Simulator
xcrun simctl launch <udid> com.squz.player -stream_addr localhost:3030

# iOS device (devicectl)
xcrun devicectl device process launch --console-pty --device <udid> com.squz.player -- -stream_addr 192.168.1.100:3030

# Android emulator / device
adb shell am start -n com.squz.player/.GeActivity --es stream_addr 10.0.2.2:3030
adb shell am start -n com.squz.player/.GeActivity --es stream_addr 192.168.1.100:3030
```

## iOS orientation lock

On iPadOS 26+ you need **both**:

1. Narrow `UISupportedInterfaceOrientations` in `Info.plist`.
2. Set `SessionHostConfig.orientation` to a non-zero `wire::kOrientation*`.

Details: `AGENTS.md` (iOS orientation lock).

## License

See individual files and `vendor/` for license terms. Engine code is Apache-2.0
unless noted. Triangle is not linked into `libge.a` by default — see `NOTICES.md`.
