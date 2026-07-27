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
| `src/button.cpp` `hitTargetRegistry()` | live `Button*`s | **B** | 🎯 filed: per-Context registry (also closes a teardown dangling window). |
| `src/appchannel.cpp` `g_slices`/`g_stateSaver`… | state-slice getters, save/restore closures | **B** | 🎯 filed: key by session; RPC gains optional session param. |
| `src/appchannel.cpp` `g_hitSurfaceW/H`, `g_extraHitTargets` | per-session geometry | **B** | Read surface from `Context::fullRectInPts()` instead. |
| `src/appchannel.cpp` `g_tasks` queue | game-thread marshal queue | **B** | One queue per session (each session has its own game thread) — real race otherwise. |
| `src/appchannel.cpp` perf counters | name → value | **B** (mild) | Session-qualify names or nest by session. |
| `src/appchannel.cpp` time control | pause/speed/step | **C** | "Pause" should mean one session; dev-only today. |
| `src/appchannel.cpp` `Channel::instance()` | app-channel socket | **A** | One dev channel per process by design. |
| `src/CmdStream.cpp` `g_live` | LiveCapture sink | **B** | 🎯 filed: per-Context sink; hot path (`sprite.cpp` consults it per draw). |
| `src/CmdStream.cpp` `imageRegistry()` | sokol image id → recipe | **A** | Keyed by process-unique handle. |
| `src/sprite.cpp` stream-buffer pool cursors | per-frame append state | **C** | "Frame" is ambiguous with N Contexts; safe while GPU work serialises per commit. |
| `src/solid.cpp`, `src/hint_hand.cpp` states | GPU pipelines only | **A** | Shared device. |
| `tweak::` registry / db / `generation()` | dev knobs | **A/C** | Process facts; note a tweak in session A redraws session B via `renderWhenStateChanges` (benign). |
| `src/metrics.cpp` scope registry | `metrics::Scope*` | **C** | `find(id)` needs session-qualified ids for the no-instance RPC path. |
| `src/render/SensorControl.cpp` accel latch | synthetic tilt | **B** | 🎯 filed: per-session sensor authority. |
| `src/render/DirectRenderHost.mm` `g_viewerBackgrounded` | wire-fed lifecycle bit | **B** | Split from the OS-fed atomics; per-session bit ORed with process bit. |
| `src/render/DirectRenderHost.mm` pending OS events | back/memory/audio-focus | **A**/C | OS facts; the dev-injected writes are the leak. |
| screenshot slot (`g_ssReq`) | one in-flight capture | **C** | Needs "which session" addressing, stays single-slot. |
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
  process-global on the shared sokol device; per-*frame* cursors in them
  are a **C** to revisit when sessions render concurrently.
- Dev-plane singletons (app-channel socket, tweak registry, screenshot
  slot) stay global but need session *addressing* in their RPCs before
  🎯T163 completes.
