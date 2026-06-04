# Prebuilds

ge ships prebuilt static libs and lifted public headers for vendor
dependencies **plus libge itself** so consumer apps' CI
doesn't need to recursively initialize ge's submodules and doesn't
need to recompile ge from sources.

The artefacts:

- **`ge/prebuilt/{ios-arm64,ios-arm64-simulator,android-arm64}/lib<name>.a`**
  — static libs for box2d, lunasvg_ge, plutovg_ge, sqlite3_ge, lz4_ge,
  liteparser, and **ge**, tracked via Git LFS.
- **`ge/headers/<dep>/include/`** — lifted public-header subset of
  each header-providing submodule (bgfx, bx, bimg, box2d, lunasvg,
  plutovg, liteparser, sdl3 — SDL3 core + freetype, spdlog, asio),
  committed as regular git files. ~14 MB total.

The `_ge` suffix on lunasvg, plutovg, sqlite3, lz4 avoids name clashes
with SDL3's bundled plutosvg/plutovg and any system-installed sqlite3.

The Apple iOS xcodeproj generator
(`ge/tools/ios-build/build_project.rb`) wires both into the project it
emits — header search paths point at `ge/headers/<dep>/include/`, the
linker pulls `-l<lib>` out of `ge/prebuilt/ios-arm64/`. The consumer's
xcodeproj compiles only the game's own sources plus
`src/iap_apple.swift` (the Swift StoreKit-2 bridge — kept inline
because Swift integration with a static lib still needs the
consumer's bridging-header config).

## Why this exists

Before T71, multimaze2's GHA workflow checked out ge with
`submodules: recursive`, which pulled ~150 MB of nested vendor
repos (bgfx + dependencies are by far the biggest). The same vendor
sources were then recompiled from scratch on every CI run because GHA
caches don't carry .o files across runs cleanly. Per-run cost was
~$0.50 and ~4 minutes of wall time, mostly spent on work that produces
the same .a files every time.

T71 moves the vendor compilation to a one-shot local-laptop step keyed
off the vendor submodule SHAs. The .a files land in the ge repo via
LFS, the headers land as regular files. Consumer CI checks out ge with
`submodules: true` (NOT recursive) and never touches ge's submodules.

## Refresh workflow

When you want to bump a vendor submodule SHA (e.g. updating bgfx to a
new upstream commit), you re-cook the prebuilts:

```bash
# 1. Bump the submodule.
cd ge/vendor/github.com/bkaradzic/bgfx
git fetch && git checkout <new-sha>
cd ../../../../..

# 2. Re-prebuild + re-lift. Both targets are idempotent and overwrite
#    their output dirs.
make prebuild
make ge/lift-headers

# 3. Commit the bump + the refreshed artefacts in one go.
git add vendor/github.com/bkaradzic/bgfx prebuilt/ headers/
git commit -m "bgfx: bump to <new-sha>; refresh prebuilts + headers"
```

Both scripts assume submodules are initialized
(`git submodule update --init --recursive`). `make prebuild` fans out
all three platforms in parallel, and each platform script compiles
independent source files in parallel internally.

For ordinary ge source/header edits where vendor submodules and vendor
sources did not change, use the faster libge-only path:

```bash
make prebuild-libge
```

This rebuilds only `libge.a` for each platform and preserves existing
vendor archives plus their manifest input hashes. If a vendor input has
changed, `tools/verify-prebuilds.py` still catches the stale vendor
archive because the old vendor input hash is kept in the manifest.

## Android NDK ABI pin (🎯T100)

The `android-arm64` prebuilt is **pinned to NDK r27** and must stay no
newer than the oldest NDK any consumer links with.

`libge.a` is a static archive: it bakes in *references* to libc++
runtime symbols (`std::exception_ptr` machinery, `__cxa_*` exception
ABI) but not their definitions — those come from the consumer's NDK
libc++ at the final `libmain.so` link. libc++ evolves its exception
ABI across releases; NDK r27 (Clang 18) lacks symbols that NDK r28+
(Clang 19/21) emit, e.g.:

- `__cxa_init_primary_exception`
- `std::exception_ptr::__from_native_exception_pointer(void*)`

If the prebuilt is compiled with a *newer* NDK than the consumer ships,
the consumer's older libc++abi can't resolve those references and the
Android link dies:

```
ld.lld: error: undefined symbol: __cxa_init_primary_exception
>>> referenced by DirectRenderHost.mm
```

This bit multimaze2 (which pins NDK r27) when a v0.50.0 prebuilt was
re-cooked on a laptop that had also installed NDK r29 — the bug 🎯T100
fixed.

**The rule: build the prebuilt with an NDK ≤ every consumer's NDK.**
Building older is forward-compatible (a newer consumer NDK provides a
superset of symbols); building newer is not. The pin lives in two
places, cross-referenced:

- `tools/prebuild.sh` — `GE_ANDROID_NDK_MAJOR` (default `27`) selects the
  highest installed NDK whose major matches the pin, ignoring an
  incidentally-newer NDK on the build machine. It then exports
  `ANDROID_NDK_HOME` so `write-manifest.py` records the same toolchain.
- `tools/verify-prebuilds.py` — `ANDROID_NDK_MAJOR_PIN` (`27`) asserts the
  committed manifest's recorded NDK major equals the pin, so a prebuilt
  cooked with the wrong NDK is caught in CI / pre-commit, not at a
  consumer's link step.

**Bumping the pin** (once the whole consumer fleet has moved to a newer
NDK): change `GE_ANDROID_NDK_MAJOR` in `tools/prebuild.sh` *and*
`ANDROID_NDK_MAJOR_PIN` in `tools/verify-prebuilds.py` together, re-cook
`android-arm64`, and confirm a consumer still links.

## Why submodules are kept

The submodules under `ge/vendor/github.com/<org>/<repo>/` remain in
`.gitmodules` and are the source of truth for *what version* of each
vendor we ship. Bumping a submodule SHA is the canonical mechanism for
upgrades. The prebuilt .a files are *derived artefacts*, not the
source.

Developers who edit ge itself can usually choose one of two local
iteration paths:

- `make prebuild-libge` — refresh committed `libge.a` prebuilts without
  rebuilding vendor archives.
- iOS project generator `engine_mode: :source` — compile ge sources
  directly into a local app target while still linking vendor prebuilts.
  Keep `engine_mode: :prebuilt` for release/CI projects.

Consumer CI doesn't recursively initialize ge's submodules, and doesn't
need to.

## CI savings

Measured on `multimaze2` `ios-testflight.yml`, macos-15 runner.

| Step | Pre-T71 ([26387615058](https://github.com/squz/multimaze2/actions/runs/26387615058)) | Phase 1: vendor lift ([26401589693](https://github.com/squz/multimaze2/actions/runs/26401589693)) | Phase 2: +libge ([26419057104](https://github.com/squz/multimaze2/actions/runs/26419057104)) |
|---|---|---|---|
| Init submodules | 97 s (recursive) | 8 s (top-level only) | 8 s |
| ship-alpha `match` | 2 s | 3 s | 3 s |
| ship-alpha `gym` | 92 s | 127 s | **61 s** |
| ship-alpha `pilot` | 22 s | 0 s (dry-run) | 0 s (dry-run) |
| Other | ~20 s | ~28 s | ~17 s |
| **Total** | **233 s (3:53)** | 166 s (2:46) | **89 s (1:29)** |

Δ vs pre-T71: **−144 s, −62%**. Per-run cost ~$0.50 → ~$0.19.

Two structural wins:

1. **Submodule init: 12× faster** (97 s → 8 s). Consumer no longer
   pulls ~150 MB of nested vendor repos; just ge itself + ge's
   prebuilt .a files via LFS smudge.
2. **`gym` 33% faster than pre-T71** (92 s → 61 s) once libge is also
   prebuilt. Phase 1 alone (vendor prebuilts only) had `gym` going the
   wrong way (+35 s); the libge prebuild swamps that and then some.
   The original +35 s in Phase 1 was probably just runner variance
   (compile-time variation between runs on the macos-15 image), not
   structural — it disappeared cleanly in Phase 2.

The pre-T71 baseline included the TestFlight upload step (`pilot`);
Phase 1 and Phase 2 measurements set `dry_run=true` to skip it
without consuming TestFlight build slots
(`SHIP_DRY_RUN=1` env-var hook in `tools/ship/release.sh`).

## Things this does *not* do

- **No CI-side prebuild job**. The prebuild runs only on Marcelo's
  laptop. A future target may add a GHA workflow that produces and
  commits prebuilts when a vendor submodule SHA changes or ge sources
  are edited; for now, manual.
- **No prebuilt Swift bridge**. `src/iap_apple.swift` still compiles
  in the consumer's xcodeproj because the bridging-header config is a
  per-target Xcode setting. Trivial cost (one Swift file, ~2 s).

## Layout

```
ge/
├── prebuilt/{ios-arm64,ios-arm64-simulator,android-arm64}/
│   ├── libge.a             8.9 MB
│   ├── libsqlite3_ge.a     1.5 MB
│   ├── libbox2d.a          490 KB
│   ├── liblunasvg_ge.a     392 KB
│   ├── libplutovg_ge.a     302 KB
│   ├── libliteparser.a     297 KB
│   └── liblz4_ge.a         76 KB
├── headers/                                  (all plain files)
│   ├── bgfx/include/bgfx/…
│   ├── bx/include/{bx,compat,tinystl}/…
│   ├── bimg/include/bimg/…
│   ├── box2d/include/box2d/…
│   ├── lunasvg/include/lunasvg.h
│   ├── plutovg/include/plutovg.h
│   ├── liteparser/include/{arena,liteparser,parse}.h
│   ├── sdl3/include/{SDL3,freetype,ft2build.h,…}
│   ├── spdlog/include/spdlog/…
│   └── asio/include/asio/…
└── tools/
    ├── prebuild.sh
    └── lift-headers.sh
```

LFS routing for `*.a` is configured in `ge/.gitattributes`.
