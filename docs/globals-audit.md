# Process-global state audit (🎯T174)

Audited 2026-07-27, on the back of scoping `ge::debug` per session. Context:
ge's design is one process hosting N independent sessions (🎯T163 — today's
stream server still runs the interim single-world fan-out, so most **B**
items below are *latent*, not live). `sg_setup` is process-global and
sokol_gfx is not thread-safe, so all sessions share one GPU device — that
axiom is what makes pipeline/shader/sampler statics legitimately global.

Classes: **A** — correctly process-global. **B** — accidentally
session-shared; misbehaves under multi-session. **C** — borderline.

| Location | Holds | Class | Notes |
|---|---|---|---|
| `src/debug.cpp` queues + HUD | draw queues, enable, HUD config | **B → fixed (🎯T174)** | Now per-session via `Context::debugStateSlot()`; GPU half stays global. |
| `src/button.cpp` registries | live `Button*`s | **A — fixed (🎯T175.5)** | Per-session registries + defaults; dead sessions pruned lazily. |
| `src/appchannel.cpp` slice/serializer stores | state-slice getters, save/restore closures | **A — fixed (🎯T175.2)** | Per-session stores overlaying the defaults store; RPCs take {"session": id}. |
| `src/appchannel.cpp` hit surface/extras | per-session geometry | **A — fixed (🎯T175.2)** | Surface read from the addressed session's `Context::fullRectInPts()`; extras overlay per session. |
| `src/appchannel.cpp` task queues | game-thread marshal queue | **A — fixed (🎯T175.3)** | One queue per session; runOnGameThread targets the addressed session; the loop pumps its own. |
| `src/appchannel.cpp` perf counters | name → value | **A — fixed (🎯T175.4)** | Nested per session; frame_ms window per session; session id on the push. |
| `src/appchannel.cpp` time control | pause/speed/step | **A — fixed (🎯T175.10)** | Per-session; pause/step/speed RPCs address one session. |
| `src/appchannel.cpp` `Channel::instance()` | app-channel socket | **A** | One dev channel per process by design. |
| `src/CmdStream.cpp` capture sinks | LiveCapture sinks | **A — fixed (🎯T175.6)** | Per-session sinks; the engine binds the active session's each frame; the draw hot path reads the cached pointer unchanged. |
| `src/CmdStream.cpp` `imageRegistry()` | sokol image id → recipe | **A** | Keyed by process-unique handle. |
| `src/sprite.cpp` stream-buffer pool cursors | per-frame append state | **A — decision (🎯T175.11)** | Safe by serialisation: all sg_* on the one game thread, one sg_commit per frame (structural; sokol validates). Documented at the commit listener; revisit only if parallel session rendering ever appears. |
| `src/solid.cpp`, `src/hint_hand.cpp` states | GPU pipelines only | **A** | Shared device. |
| `tweak::` registry / db / `generation()` | dev knobs | **A/C** | Process facts; note a tweak in session A redraws session B via `renderWhenStateChanges` (benign). |
| `src/metrics.cpp` scope registry | `metrics::Scope*` | **A — fixed (🎯T175.9)** | Scopes latch a session tag at construction; the no-instance RPC resolves within the addressed session first. |
| `src/render/SensorControl.cpp` accel state | synthetic tilt | **A — fixed (🎯T175.7)** | Per-session AccelState structs; reset clears all. |
| `src/render/DirectRenderHost.mm` viewer bits | wire-fed lifecycle bit | **A — fixed (🎯T175.8)** | Per-session bit (ServerSession binds its host session id) ORed with the process bit. |
| `src/render/DirectRenderHost.mm` pending OS events | back/memory/audio-focus | **A**/C | OS facts; the dev-injected writes are the leak. |
| screenshot slot (`g_ssReq`) | one in-flight capture | **A — addressed (🎯T175.10)** | RPC validates the addressed session; the slot stays single-occupancy (the addressed session is the rendering one under process-per-session). |
| `src/audio.cpp`, `audio_apple.mm` | device registry, observers | **A** | Process/OS facts. |
| `src/iap.cpp` store, `iap_android.cpp` JNI | one store, JVM refs | **A** | Per-process app identity. |
| `src/Signal.cpp`, `src/log.cpp`, `src/Resource.cpp`, `src/manifest.cpp`, `Immersive_apple.mm` | quit/crash/log/paths/build info/chrome | **A** | Process-scoped by definition. |
| `src/text.cpp` `ftLibrary()`, `cachedFontBytes` | FreeType lib + font cache | **A + bug** | 🎯 filed: unsynchronised on a non-thread-safe library; `FontLoader_*` caches lock, `text.cpp` doesn't. |
| `src/svg.cpp` font registration | family → face | **A** | Idempotent process facts. |
| `RefreshRateBoost`, `TileQueue`, `hint::Player`, `Context::M` | per-instance | **A** | Already correctly scoped; `Context::M` is the reference pattern. |

## Doctrine

- Session-varying content (queues, callbacks/closures over game state,
  per-surface geometry, input authority) lives in `Context::M` or a
  Context-carried slot — never in a file-static.
- GPU objects (pipelines, shaders, samplers, stream buffers) stay
  process-global on the shared sokol device; the per-*frame* cursors are
  safe by serialisation (🎯T175.11 — one game thread, one commit per
  frame), documented at sprite.cpp's commit listener.
- Dev-plane singletons (app-channel socket, tweak registry, screenshot
  slot) stay global; their RPCs are session-addressed via the shared
  resolver (🎯T175.1) — explicit {"session": id}, sole-session default.
