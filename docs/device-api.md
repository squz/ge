# Device API model (physical vs stream)

## Goal

Games talk only to **`ge::Context`**, SDL events via `onEvent`, `ge::audio`,
`ctx.db()`, and `RunConfig` lifecycle hooks. They never branch on
“am I streamed?”. The **backend** that fills those fields is selected at
**compile time**:

| Build | Binary | Host | Discovery source |
|-------|--------|------|------------------|
| Desktop / mobile **game** | `bin/<app>` | Windowed `DirectRenderHost` | Local OS |
| **Server** (`make GE_SERVER=1`) | `bin/<app>-server` | **Console** stream host (`runServer`) | Player → wire → `Context` |

These are **totally different builds** (separate objects, separate entry). Not one app with a runtime mode. The server is operator-facing (stdio/logs + relay), not a Dock game; residual offscreen Metal drawable is 🎯T154.1.

**Projects do not rediscover this.** `Module.mk` owns the products:

```text
make / make game     → windowed game
make server          → console stream host
make run-server      → run host (GE_SERVER=host:port runtime relay addr)
make player          → desktop glass
```

Game repos only declare `APP_NAME` / `APP_SRC` and call `ge::run`. There is **no public API to select a backend** in game code.

**Full migration inventory and status:** [device-api-migration.md](device-api-migration.md) (🎯T154).

## Push vs discover vs state

| Half | Meaning | Wire (today / planned) |
|------|---------|-------------------------|
| **Push** | Content + session policy → player | GE2V, GE2S, `SessionConfig`, … |
| **Discover** | Viewer facts → game `Context` | `DeviceInfo`, `SafeAreaUpdate` (expand: dual safe, lifecycle, …) |
| **State** | Durable db | GE2T sqlpipe (**unimplemented**; PrefPath still on server) |

## Current proof point (provisional)

TiltBuggy uses `drawSafeRectInPts()` for title, dirt clipping, and camera
aspect. Under stream that is driven by **one** player safe rect mapped into
content space and currently copied into **both** draw and ui insets — **not**
full dual-contract parity. See migration doc.

## Explicit non-goals

- Runtime `selectBackend(...)`
- AccelSynth / presentation-tilt synth (with vs without real accelerometer)
- Requiring the player process to be a full `ge::run` game (unless a later target says so)
