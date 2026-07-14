# Device API migration audit — decouple streaming from the app

**Date:** 2026-07-14  
**Target:** 🎯T154  
**Criterion of done:** A ge game binary’s *application* code never knows whether it is running as a direct app or as a stream server. Everything it learns about or does to “the device” goes through the same game-facing surface (`Context`, SDL events via `onEvent`, `ge::audio` registration, `ctx.db()`, lifecycle callbacks on `RunConfig`). Compile-time selects **physical** vs **server** packaging; there is no public runtime modality switch and no stream-only branches in samples.

AccelSynth / presentation-tilt synthesis (devices without a real accelerometer) is **out of scope** — orthogonal axis (sensor present vs synth).

---

## Architecture reality (today)

```
DIRECT APP (bin/<app>)              SERVER BUILD (bin/<app>-server)  ← different binary
──────────────────────              ────────────────────────────────
make / make game                    make server (-DGE_SERVER_BUILD)
ge::run → windowed DirectRenderHost ge::run → runServer (console identity)
  Context ← local OS                  still uses DirectRenderHost + HIDDEN window
  SDL events ← local                  as offscreen Metal drawable (host detail)
  db ← PrefPath on device             + ServerSession → spyder relay
                                      player attach ≥0; discovery closed-loop
```

**Product rule (user):** desktop game and server are totally different builds; server is a **console program**, not a Mac app. 🎯T154.1: `make server` + Accessory console identity (no Dock game). Hidden Metal window remains as encode drawable, not a product surface.

Streaming discovery/state coupling: the *viewing* device must be Context authority while a player is attached; durable state on the player.

The **player process** (`playerCore` / Android `com.squz.player`) is **not** a `ge::run` app. It is a display + sensor proxy. Completing the polymorphic API does **not** require the player to become a full game (T34 is related polish); it requires the **server-side game** to see a complete virtual device through the same types.

---

## Game-facing surface inventory

### A. `Context` discovery (read each frame / session)

| Surface | Direct source today | Stream source today | Migrate? |
|---------|---------------------|---------------------|----------|
| `fullRectInPts()` / surface px | Local window / swapchain | **Content** surface: initial host size, then **aspect matched to player** so stream fills glass (no letterbox gutters) | **Shipped (aspect match):** fullRect is content, not raw phone px; aspect follows DeviceInfo; safe insets map viewer chrome into content. |
| `drawSafeRectInPts()` / draw insets | Cutouts only (Android JNI / iOS ≈ ui) | **One** wire safe rect → **same** as ui | **Yes — dual wire fields** (draw vs ui); closed loop each |
| `uiSafeRectInPts()` / ui insets | Full OS safe area | **Same single rect as draw** | **Yes — distinct** from draw |
| `pixelsPerPt()` | `SDL_GetWindowDisplayScale` host | Largely **host** ppt | **Yes — viewer density** for any layout that must match physical mm on the glass; content surface conversion must be defined |
| `deviceClass()` | `SDL_IsTablet` / desktop | DeviceInfo class (applied when valid) | **Yes — keep closed-loop;** fix gaps (desktop player class, refresh on change) |
| `deviceUiScale()` | Host short-side formula | Viewer short-side when metrics valid | **Yes — verify formula inputs are viewer-only** under stream |
| `orientation` (session) | Config + OS | SessionConfig lock to player; live orientation only partially | **Yes — live viewer orientation** in Context or events if games need it |
| `parallax()` | Host attitude (iOS/Android) | **Host** (Mac → usually 0) | **Yes — viewer attitude** (or document always-0 under stream if parallax is direct-only feature) |
| `presentationTilt()` | AccelSynth on host | AccelSynth on **server** from remote mouse | **Out of scope** (AccelSynth axis) |
| `fps()` / `frameTime()` / `framesPresented()` | Host run loop | Host run loop (server tick) | **Partial:** OK as *simulation/present rate of game process*; not viewer display FPS. Optional: expose viewer FPS later — not required for decoupling if documented |
| `db()` | PrefPath `game.db` on device | **`:memory:`** working set; player holds per-game durable file | **shipped** — GE2T attach seed + ~0.5s stream push + detach; path `<ge-player pref>/games/<serverName>.db` |
| Tweaks DB | PrefPath `tweaks.db` on host | Server Mac | **Yes — with player state** or session-ephemeral policy |
| `swapchainPass()` | Local present | Present + capture/cmdstream side effect | **OK if opaque:** game must not care; verify no stream branch in apps |
| Render-on-demand APIs | Direct only (docs: brokered unaffected) | Continuous forced for stream | **Yes or document:** either ROD works under stream (viewer still, save encode) or API returns “always continuous” without game branching |

### B. Input (`RunConfig::onEvent` / SDL)

| Surface | Direct | Stream | Migrate? |
|---------|--------|--------|----------|
| Pointer / touch / mouse / wheel | Local SDL | GE2I memcpy SDL_Event; content map on player | **Harden:** full event set, multi-touch, focus, text input if used; tests |
| Keyboard | Local | Forwarded | **Harden** |
| `SDL_EVENT_SENSOR_UPDATE` | Local (+ AccelSynth path) | Player screen-rotated samples; server skips re-rotate when streaming | **Harden:** sensor on/off, availability, rate; **AccelSynth out of scope** |
| Other sensors (gyro raw, etc.) | If any | Not on wire | **Only if games use them** — audit samples; default out until needed |
| Coordinate space | Window / `sdl_input` + Context size | Player maps to content/server space | **Yes — single contract:** events always in content/surface space matching `fullRect` |

### C. Lifecycle & policy (`RunConfig` + host)

| Surface | Direct | Stream | Migrate? |
|---------|--------|--------|----------|
| `onBackPressed` | Android host | **Not** from player | **Yes** |
| `onMemoryWarning` | iOS/Android host | **Not** from player | **Yes** (player memory pressure) |
| `paused()` / background skip render | Android host | Server keeps rendering | **Yes — viewer foreground/background** should gate or signal game |
| Audio focus / background | `ge::audio` on host activity | Player glass has lifecycle; **server audio is Mac** | **Yes — policy:** either audio on player only, or server audio follows **viewer** focus |
| Screen saver / immersive | Host config | **Host only until T154.2** — see SessionConfig inventory | **BLOCKING:** must apply on **player** |
| `disableScreenSaver` | Host | Host only until T154.2 | **Player** while attached |
| App-channel pause/step/speed | Direct + `SPYDER_APP_CHANNEL` | Server process | Dev-tooling; **not** modality decoupling for ship — leave as process-local debug |

### D. Present / push (server → player)

| Surface | Status | Migrate? |
|---------|--------|----------|
| H.264 GE2V | Works | Keep as negotiated present rung |
| Cmdstream GE2S | Works (sprite/recipe subset) | Keep; expand under T128 family |
| SessionConfig (sensors, orientation, transport) | Partial | **immersive / disableScreenSaver missing on wire** (Pixel status bar repro 2026-07-14) → 🎯T154.2 |
| AspectLock | Protocol exists | Wire if games need forced letterbox policy |
| “Game doesn’t branch on stream” | Mostly true for draw API | **Guard:** no sample checks transport/env |

Present stays **push to viewer**; decoupling means the game never opens sockets or names “player,” not that the GPU runs on the phone.

### E. Persistence & sync

| Surface | Status | Migrate? |
|---------|--------|----------|
| `Context::db()` path | Server uses Mac PrefPath when org/app set — **contradicts** SessionHostConfig comment (“headless :memory:, player owns via sqlpipe”) | **Yes — fix:** stream session db is player-authoritative |
| `kSqlpipeMsgMagic` (GE2T) | Defined, **no server/player handlers** | **Yes — implement** bidirectional sqlpipe |
| Tweak overrides | Server-local file | **Yes — player or sqlpipe** |
| Save games / progress in samples | Via db or files | **Player store** under stream |

### F. Secondary engine services (modality-sensitive)

| Surface | Stream issue | Migrate? |
|---------|--------------|----------|
| Screenshot / app-channel capture | Captures **server** framebuffer (OK for dev) | Dev OK; not player camera |
| IAP | Platform on direct host | Stream: purchases are on **player OS** — major product decision; **in scope for decoupling** if games call `ge::iap` without modality branch → wire or player-side IAP bridge |
| FileIO / `resource()` | Server filesystem / bundled assets | Server may keep content assets; **user documents** belong on player |
| Refresh-rate boost | Host display | **Player** display while streaming |
| Clipboard / share sheets | Host | **Player** if games use them |
| Haptics | Host | **Player** if games use them |

### G. Structural / process coupling (not Context fields)

| Issue | Why it couples modality | Migrate? |
|-------|-------------------------|----------|
| Server = hidden `DirectRenderHost` on Mac | Context defaults to Mac until viewer metrics applied | **WireDevice (or equivalent)** owns discovery refresh from player only while attached; host window is encode target only |
| Player not `ge::run` | Fine for dumb glass; discovery must still be complete | Complete discover/push protocols |
| Multi-session (≥0 players) | Which viewer’s metrics apply per game instance? | **Per-session Context** from that session’s player |
| `GE_TRANSPORT`, relay URL | Process env for server packaging | OK if **outside** game code |
| Spyder streamrelay | Byte-agnostic pipe | Keep; new magics for lifecycle/sqlpipe/IAP as needed |

---

## What is already partially done (do not re-claim as complete)

- Single safe rect DeviceInfo/SafeAreaUpdate → mapped insets → **both** draw and ui (provisional; **not** dual-contract).
- Device class from DeviceInfo when valid.
- SDL events + accel samples over wire with content mapping / screen-frame accel.
- SessionConfig orientation/sensors/transport.
- Present: H.264 + cmdstream.
- Compile-time app vs `GE_SERVER_BUILD` (no public selector).

---

## Definition of decoupled (acceptance backbone)

1. **No game-code modality:** samples and apps do not read `GE_SERVER`, transport, or “am I streaming?”  
2. **Viewer-authoritative discovery** while a player is attached: distinct draw/ui safe, density, class, ui scale, orientation; parallax from viewer or explicit zero.  
3. **Content vs viewer contract documented:** fullRect/surface size policy fixed; safe insets expressed in content space.  
4. **Input events** in content space; sensor samples match direct semantics for hardware paths.  
5. **Lifecycle + audio policy** follow the **viewer** when streaming.  
6. **Durable `ctx.db()` (and tweaks) on the player** via sqlpipe (GE2T); server ephemeral OK for sim.  
7. **Present** remains negotiated push; opaque to game via `swapchainPass`.  
8. **IAP / haptics / refresh-rate / immersive** either work via player bridge or are documented unavailable with a single API that no-ops both sides consistently.  
9. **AccelSynth out of scope.**  
10. **Oracles:** unit tests for mapping/db ownership; TiltBuggy (or successor) uses only polymorphic surface; Android + iOS stream + desktop direct without stream branches.

---

## SessionHostConfig → wire SessionConfig inventory (do not drop)

Game-facing setup is `SessionHostConfig` on `ge::run`. Viewer-affecting
fields must appear on `wire::SessionConfig` (or a documented successor) and
be applied on the player. Host-only fields stay off the wire.

| SessionHostConfig | Affects viewer glass? | Wire today | Required action |
|-------------------|----------------------|------------|-----------------|
| `sensors` | Yes | **Yes** | Keep |
| `orientation` | Yes | **Yes** | Keep |
| `transport` (negotiate) | Yes (decode path) | **Yes** | Keep |
| **`immersive`** | **Yes — status/nav bars** | **Yes — `kSessionFlagImmersive`** | Applied on player before DeviceInfo (T154.2) |
| **`disableScreenSaver`** | **Yes — keep awake** | **Yes — `kSessionFlagNoScreenSaver`** | Applied on player before DeviceInfo (T154.2) |
| `width` / `height` | Content surface (initial) | Server-local; **aspect retargets to viewer** under stream | Content fills glass; fullRect is content space |
| `orgName` / `appName` / `schemaDdl` | Durable id/db | Server :memory:; player store via GE2T | T154 sqlpipe residual |
| `parallaxFactor` | Motion feel | Not on wire; stream zeros parallax | Document or viewer attitude later |
| `metricsReportThreshold` | Host process | Host only | OK host-only |
| `crashDiagnostics` | Host process | Host only | OK host-only |
| `hidden` / headless | Server packaging | N/A to player | OK host-only |

**Rule:** adding a new `SessionHostConfig` field that changes what the user
sees or feels on the device **must** extend this table and the wire in the
same change.

### Handshake order (invariant)

```
ge::run SessionHostConfig     fixed constants at the outset (game code)
        ↓
server → player: SessionConfig   first app payload on the wire
player:          apply that policy (immersive, orientation, sensors, …)
player → server: DeviceInfo      first measurement of the configured surface
                 … stream …
player → server: SafeAreaUpdate  mid-session only (rotate/resize)
```

**Wrong:** compute safe rects, then receive init, then “make sure rects update.”  
**Right:** SessionConfig is seed data known before any discovery. Safe rects
do not exist until after it is applied; DeviceInfo is the first read of the
resulting glass, not a pre-init snapshot that needs patching.

## Suggested decomposition (children of T154)

| ID | Focus |
|----|--------|
| **T154.1** | Server console host (not Dock game): `make server` + Accessory identity |
| **T154.2** | **SessionConfig policy on glass: immersive + disableScreenSaver** (status bar repro) |
| T154.discovery | Dual safe rects when cutout API exists; density/class; Context contract |
| T154.input | Event set completeness, content-space contract, tests |
| T154.lifecycle | GE2L back/memory/fg/bg (partially shipped) |
| T154.audio | Viewer focus/background drives pause policy |
| T154.sqlpipe | GE2T + player durable db; server :memory: |
| T154.services | IAP/haptics/refresh bridges or uniform no-ops |
| T154.host | Viewer metrics sole Context source while attached; multi-session |
| T154.oracles | Docs matrix + sample + device proof |

## Shipped status (2026-07-14 T154 push)

| Item | Status |
|------|--------|
| Dual draw/ui wire (v8) + mapViewerDualSafeInsets | **shipped**; draw=cutouts only, ui=SDL safe |
| SessionConfig immersive + noScreenSaver (T154.2) | **shipped**; Pixel: flags=0x3, bars hidden, draw full |
| Content aspect matches viewer under stream | **shipped** (no letterbox gutters) |
| Content-surface fullRect docs | **shipped** |
| Stream server :memory: db (not PrefPath) | **shipped** (`durableDbPathForHost`) |
| GE2T full player db sync | **snapshot rung shipped** (player durable PrefPath ↔ server :memory: attach seed + detach push); richer Peer protocol residual |
| Viewer lifecycle GE2L + audio/back/memory inject | **shipped** |
| Parallax under stream | **{0,0}** (documented) |
| make game / make server products | **shipped** in Module.mk |
| Console server identity | **shipped** (🎯T154.1: Accessory + `make server`) |
| iOS stream player rebuild | **shipped** (`tools/ios` CMake; Pixel/Android + iPad mini proof) |
| AccelSynth | OOS |
| TiltBuggy title | **drawSafe** (content chrome, not uiSafe) |
- 2026-07-14 Pixel proof: SessionConfig first; DeviceInfo draw=(0,0 full); arena/title on drawSafe; content 1.6 aspect.
