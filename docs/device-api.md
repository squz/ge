# Device API model (physical vs stream)

## Goal

Games talk only to **`ge::Context`**, SDL events via `onEvent`, `ge::audio`,
`ctx.db()`, and `RunConfig` lifecycle hooks. They never branch on
“am I streamed?”. The **backend** that fills those fields is selected at
**compile time**:

| Build | Binary | Host | Discovery source |
|-------|--------|------|------------------|
| Desktop / mobile **game** | `bin/<app>` | Windowed `DirectRenderHost` | Local OS |
| **Server** (`make server`) | `bin/<app>-server` | **Console** stream host (`runServer`) | Player → wire → `Context` |

These are **totally different builds** (separate objects, separate entry). Not one app with a runtime mode. The server is operator-facing (stdio/logs + relay), not a Dock game. A hidden SDL/Metal window is only the offscreen drawable for encode/cmdstream.

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

| Half | Meaning | Wire (today) |
|------|---------|----------------|
| **Push** | Content + session policy → player | SP2V, SP2S, `SessionConfig` (incl. immersive / noScreenSaver flags) |
| **Discover** | Viewer facts → game `Context` | `DeviceInfo` + `SafeAreaUpdate` with **dual** draw/ui safe; lifecycle SP2L |
| **State** | Durable db | **SP2T** full-sqlite snapshot: player durable PrefPath file is authority (**one file per game**: `<ge-player pref>/games/<serverName>.db`); server uses **`:memory:`** working set only. Attach: player→server seed (if user tables exist). Stream: server→player push ~every 0.5s (fire-and-forget). Detach: final push. |

## Discovery (dual safe)

- **drawSafe** — cutouts only (full surface on devices with no punch-hole, e.g. Pixel).
- **uiSafe** — cutouts + system bars + gestures (when bars are part of the surface).
- Under stream, viewer DeviceInfo maps into content-space insets via
  `mapViewerDualSafeInsets` (`ViewerMetrics.h`). Content `fullRect` is the
  server swapchain / encode size (may retarget aspect to the viewer).

TiltBuggy frames the arena and title on **drawSafe** (content chrome, not
interactive ui-safe clearance).

## Virtual device session (🎯T156) — location transparency

Stream is a **transport**, not a modality. The game↔player wire serializes the
polymorphic device API; spyder streamrelay is an **opaque router** on that path
(catalogue, attach/detach, byte pipe, counters). Spyder’s broader control plane
(app-channel, logs, deploy, telemetry, inventory) is out of scope of this
section and remains first-class spyder surface.

### 1:1 message ↔ API mapping

| Wire (player protocol) | Device API / host role |
|------------------------|------------------------|
| `SessionConfig` (SP2C) | Host → glass policy: sensors wanted, orientation lock, transport, immersive, noScreenSaver |
| Present (SP2V / SP2S) | Content push (swapchain / cmdstream) — game does not open the wire |
| `DeviceInfo` (SP2D) | Glass → host discovery: size, class, dual safe, capabilities |
| `SafeAreaUpdate` | Glass → host discovery refresh (chrome/cutouts) |
| SP2I + `SDL_Event` payload | Device-local input (mouse/finger/key/wheel) and sensor samples when present — **no separate SENSOR_UPDATE magic**; samples travel as `SDL_EVENT_SENSOR_UPDATE` inside SP2I |
| SP2L lifecycle | Viewer foreground/background, back, memory, audio focus |
| SP2T | Durable db snapshot (player authority; server `:memory:` working set) |

Protocol layout oracles: `ge/include/ge/Protocol.h` and the spyder player mirror.

### Surface classification

| Surface | Classification | Notes |
|---------|----------------|-------|
| SessionConfig policy fields | **marshalled** | Applied on glass before DeviceInfo |
| DeviceInfo / dual safe / class | **marshalled** | Seat-bound (see below) |
| Pointer/touch/key/wheel | **marshalled** | Device-local units; no host-Mac content remap |
| Hardware accelerometer samples | **marshalled** | Inside SP2I; only when glass has a real sensor |
| AccelSynth / presentation tilt | **marshalled (host-side)** | Engine-only; constructed when no usable real sensor — same class as direct |
| Present H.264 / cmdstream | **marshalled** | Negotiated transport |
| SP2T durable state | **marshalled** | Snapshot rung |
| Parallax / host attitude under stream | **uniform-no-op** | `{0,0}` same call site (T154) |
| Viewer lifecycle → pause gate | **named-divergence** | T154 residual until fully closed |
| Audio focus under stream | **named-divergence** | T154 residual |
| Haptics / refresh-rate boost | **named-divergence** | bridge or uniform no-op |
| IAP | **named-divergence** | player-OS side |
| Tweaks DB full peer sync | **named-divergence** | SP2T snapshot is MVP |

### Boundary purity

The marshalling layer holds **no** device-semantic policy: no arbitration between
synth and remote sensors, no multi-seat merges, no second gravity/tilt model.
Semantics live **above** (game / AccelSynth / Context) or **below** (glass OS).
A stream-only code path that invents different meaning for the same API surface
is a **defect**.

### Seat model (one authority per virtual device)

- **Direct:** one physical device → one sensor authority (real accel **or** AccelSynth).
- **Stream:** N attached players are **N remote devices** (seats). Default for
  single-world games (TiltBuggy): **primary seat = first attached wire**;
  spectators receive content fan-out; **only the primary seat** may update
  DeviceInfo/content-surface authority and deliver sensor samples to the game.
- Seat handoff (if ever supported) is an **explicit** session event — not
  per-event “arming window” arbitration.
- **Rejected:** time-multiplexed merge of multiple seats’ sensors into one
  gravity/tilt story.

### Sensor authority (AccelSynth vs real)

When AccelSynth is constructed (no usable real host sensor — direct desktop /
sim / stream server):

- Gravity **and** presentation tilt derive from AccelSynth `tilt_` state.
- While AccelSynth owns the gesture (`armed` or non-zero tilt / ease-back),
  external `SDL_EVENT_SENSOR_UPDATE` (e.g. wire-forwarded mobile samples) must
  **not** reach the game sensor handler.
- Hold-still keeps gravity aligned with presentation (authority re-asserts each
  frame). Ease-back on disarm matches direct.

### Spyder on the player path

| Spyder may | Spyder must not |
|------------|-----------------|
| Route bytes, name catalogue, attach sessions, counters | Interpret tilt, gravity, DeviceInfo policy, seating |
| Control plane: deploy, logs, app-channel, health | Own a second AccelSynth or modality branch in samples |

### Player (glass)

Apply SessionConfig; report DeviceInfo; forward device-local input and real
sensors. Relative-mouse arming is **delivery quality** (usable `xrel`), not
tilt semantics. No AccelSynth synthesis, no gravity model.

### Parity oracle

Same scripted device-local gesture → same game-observed gravity and
presentation trajectories under direct vs stream-authority path. The in-process
loopback inject path is the shipped SP2I marshalling used by `ServerSession`
(`wire::packSdlEvent` → `wire::unpackSdlEvent` → `AccelSynth::handle`), not a
reimplementation and not a double-run of one entry point. Unit harness:
`AccelSynth_test.cpp` (`T156.6`, hold-left + second-seat competitor). Real wire
may differ only by transport latency or coalescing.

Full player-core library extraction into the game process is a non-goal of this
section; glass purity of the spyder player is structural (no AccelSynth
synthesis) plus the dual-path SP2I oracle above.

## Explicit non-goals

- Runtime `selectBackend(...)`
- Requiring the player process to be a full `ge::run` game (unless a later target says so)
- Full incremental sqlpipe Peer protocol over SP2T (snapshot is the shipped rung; 🎯T154 residual for richer sync)
- Re-linking ge into the spyder player binary for parity
