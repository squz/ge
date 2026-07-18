# Web platform (Emscripten / WebGL2) — 🎯T157

ge's fourth direct-mode platform. A consumer app's unmodified
`main.cpp` / `ge::run(Factory)` builds to `<app>.html/.js/.wasm` and runs
in current Chrome, Firefox, and Safari.

```bash
brew install emscripten          # or an activated emsdk (em++ on PATH)
cd sample/tiltbuggy
make web                         # → build/web/TiltBuggy.html
python3 -m http.server -d build/web 8080   # file:// cannot fetch wasm
```

`make web` (Module.mk) configures ge's `tools/web-template/` CMake project
with the app's existing `APP_NAME` / `APP_SRC` / `APP_DISPLAY_NAME` vars —
there is no per-consumer scaffolding. The template compiles
`shaders/*.glsl` via sokol-shdc (`glsl300es`), preloads `assets/`, `data/`,
and `fonts/` when present, and links against `prebuilt/web-wasm/` (cooked
by `tools/prebuild.sh web-wasm`) through `cmake/web-wasm.cmake`.

## Architecture

| Concern | Choice |
|---|---|
| Renderer | `SOKOL_GLES3` over WebGL2, `SOKOL_IMPL` inline in `src/SokolContext_web.cpp` (the Apple single-backend model — no T107 dispatch). WebGPU is 🎯T157.1, gated. |
| Run loop | The **native blocking loop runs verbatim**, suspending between frames via a RAF-aligned Asyncify await (`ge_web_await_frame`, `src/SessionHost.mm`). A hidden tab stops RAF → the game pauses. |
| Exceptions | JS-based (`-fexceptions`); Asyncify rejects `-fwasm-exceptions`. |
| Persistence | `SDL_GetPrefPath` → `/libsdl/<org>/<app>/game.db` on an **IDBFS** mount; `tools/web-template/pre.js` hydrates it before `main()` and syncs every 5 s + on tab hide. sqlite cooks with `SQLITE_NO_SYNC` on web (durability is the explicit `FS.syncfs`). |
| Networking | Direct-only: `GE_DIRECT_ONLY`; app-channel is `NDEBUG`-compiled-out in release. No asio/raw TCP reaches the wasm link. |
| Threads | None — release web builds are single-threaded; no COOP/COEP headers needed. |
| Sensors / chrome | Attitude, Immersive, CutoutInsets, orientation lock: stubs. `DeviceOrientationEvent` parallax/tilt is a follow-on. Tilt comes from the Shift-mouse AccelSynth (absolute motion; relative mouse / pointer lock is deliberately not used — a denied lock makes SDL discard motion). |
| IAP / logging / audio | StubStore; stderr → browser console; SDL3 → WebAudio. |

## The Asyncify contract (read before touching the run loop)

Why not `emscripten_set_main_loop(simulate_infinite_loop)`: its "never
returns" is a JS exception unwound through the wasm stack, and C++ cleanup
pads catch foreign exceptions — `host`, `rc`, and the consumer's
pre-`ge::run` state were destroyed with the RAF callback still registered.
Asyncify's unwind is a code transform, not an exception, so the documented
state-before-run capture pattern survives verbatim.

**Asyncify cannot suspend beneath a JS frame.** With JS-based exceptions,
any call inside a scope with active cleanups compiles to a JS `invoke_*`
trampoline, so the whole suspend chain must be plain wasm calls:

- `ge::run` / `runDirectHosted` are declared `noexcept` (main's call to
  them must not be invoke-wrapped; see the note in `SessionHost.h`).
- `SessionHost.mm` compiles `-fno-exceptions -DSPDLOG_NO_EXCEPTIONS` on
  web (`tools/prebuild.sh`); `guardCallback` / `renderBatch` degrade via
  `__cpp_exceptions`.
- Anything that *might* suspend must not be called from C++ try scopes:
  sqlite's fsync (`__wasi_fd_sync`) is compiled out (`SQLITE_NO_SYNC`),
  SDL's own swap sleep is disabled (`SDL_HINT_EMSCRIPTEN_ASYNCIFY=0`
  plus swap interval 0 in `SokolContext_web.cpp`).
- `-sASYNCIFY_IGNORE_INDIRECT=1` keeps instrumentation to the direct
  `main → run → loop` chain — game callbacks pay no Asyncify tax, but it
  also means **a suspend reached through an indirect call will abort**
  ("Aborted(invalid state)"). If that abort ever reappears, link once
  with `-sASSERTIONS=2 -sASYNCIFY_ADVISE=1` and read the report.

## Shutdown semantics

`shouldQuit()` never trips in a browser (no SIGINT; `SDL_EVENT_QUIT`
doesn't fire), so `onShutdown` effectively does not run — page teardown is
the exit. Persistence does not depend on it (periodic + hide-event
`FS.syncfs`).

## Verification

Desktop-browser smoke used for 🎯T157 acceptance (all pass as of the T157
branch): boot + tick + screenshot in Chrome, Firefox, WebKit; Shift-drag
moves gravity and the buggy rolls; `game.db` (12 KB sqlite) present in
IndexedDB after the 5 s sync and restored on reload with a clean boot.
Driver scripts live in the session scratchpad (playwright-core against
`python3 -m http.server`); promote them to `tools/` when web joins the
matrix.

## Limitations (v1)

No device sensors (accelerometer parallax/tilt), no WebGL context-loss
recovery (sokol has no restore path), no dev app-channel (needs a
WebSocket transport), `deviceClass()` reports Desktop, and the headless
`render` verb is desktop-only. WebGPU backend: 🎯T157.1.
