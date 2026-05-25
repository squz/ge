# Vendor prebuilds (iOS arm64)

ge ships prebuilt static libs and lifted public headers for nine vendor
dependencies so consumer apps' iOS CI doesn't need to recursively
initialize ge's submodules. This cuts a typical multimaze2-class CI run
from ~4 min to ~90 s.

The artefacts:

- **`ge/prebuilt/ios-arm64/lib<vendor>.a`** — nine static libs (bgfx,
  bx, bimg, box2d, lunasvg_ge, plutovg_ge, sqlite3_ge, lz4_ge,
  liteparser), tracked via Git LFS. ~4.4 MB total.
- **`ge/headers/<vendor>/include/`** — lifted public-header subset of
  each vendor submodule, committed as regular git files. ~1.7 MB total.

The `_ge` suffix on lunasvg, plutovg, sqlite3, lz4 avoids name clashes
with SDL3's bundled plutosvg/plutovg and any system-installed sqlite3.

The Apple iOS xcodeproj generator
(`ge/tools/ios-build/build_project.rb`) wires both into the project it
emits — header search paths point at `ge/headers/<vendor>/include/`,
the linker pulls `-l<vendor>` out of `ge/prebuilt/ios-arm64/`. Vendor
source files are no longer added to the xcodeproj's compile phase.

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
make ge/prebuild-vendor-ios-arm64
make ge/lift-headers

# 3. Commit the bump + the refreshed artefacts in one go.
git add vendor/github.com/bkaradzic/bgfx prebuilt/ headers/
git commit -m "bgfx: bump to <new-sha>; refresh prebuilts + headers"
```

Both scripts assume submodules are initialized
(`git submodule update --init --recursive`). They run in ~30 s for a
single-vendor bump on an M-series MacBook.

## Why submodules are kept

The submodules under `ge/vendor/github.com/<org>/<repo>/` remain in
`.gitmodules` and are the source of truth for *what version* of each
vendor we ship. Bumping a submodule SHA is the canonical mechanism for
upgrades. The prebuilt .a files are *derived artefacts*, not the
source.

Developers who edit ge itself, or who want to verify the prebuilds
match the committed submodule SHAs, run
`git submodule update --init --recursive` and re-run the prebuild
script. Consumer CI doesn't, and doesn't need to.

## Why iOS arm64 only (today)

T71 deliberately scoped to iOS arm64 first:

- The iOS build (via `build_project.rb`) is the cost-driver in
  multimaze2-class CI workflows — most other paths build under `make`
  on a developer's laptop, where caching works.
- iOS arm64 is one slice; Android, macOS, iOS simulator, and Android
  emulator add roughly 5× the build matrix. Solving one cleanly is
  better than solving five badly.
- App Store IPAs are device-only — no simulator slice required.

When Android CI lands (target T46-class follow-up), the same pattern
extends: add `prebuilt/android-arm64/`, a sibling prebuild script, and
teach Module.mk's Android branch to link the prebuilts.

## CI savings

Measured on `multimaze2` `ios-testflight.yml`, macos-15 runner:

| Step | Pre-T71 ([26387615058](https://github.com/squz/multimaze2/actions/runs/26387615058)) | Post-T71 ([26401589693](https://github.com/squz/multimaze2/actions/runs/26401589693), `dry_run=true`) | Δ |
|---|---|---|---|
| Init submodules | 97 s (recursive) | 8 s (top-level only) | **−89 s** |
| Run ship-alpha | 122 s (incl. pilot upload) | 136 s (no pilot — dry-run) | +14 s |
| Other steps | ~14 s | ~18 s | +4 s |
| **Total wall time** | **233 s (3:53)** | **166 s (2:46)** | **−67 s (−29%)** |
| **Estimated cost** | ~$0.50 | ~$0.36 | |

Where the savings came from (and didn't):

- **Submodule init: 12× faster** (97 s → 8 s). This is the structural
  win. Consumer no longer pulls ~150 MB of nested vendor repos; just ge
  itself + the prebuilt .a files via LFS smudge inside ge's clone.
- **`ship-alpha` got slightly slower**, not faster. Expected
  vendor-compile savings (~60 s) didn't show up in the measurement.
  Plausible reasons: LFS smudge during the submodule clone eats some
  of it; runner-to-runner variance; the `gym` archive pass spends most
  of its time on linker + signing + IPA packaging rather than compile.
  Worth a second measurement run to confirm — but the overall −29%
  total is real.

The pre-T71 run included the TestFlight upload step (`pilot`); the
post-T71 run set `dry_run=true` to skip it without consuming a
TestFlight build slot (see `SHIP_DRY_RUN=1` env-var hook in
`tools/ship/release.sh`).

## Things this does *not* do

- **No prebuilt libge.a**. Engine sources (`ge/src/*.cpp`,
  `ge/src/*.mm`, plus `vendor/src/sqlpipe.cpp`) are still compiled
  inline by the consumer's iOS build. Bumping ge itself remains a fast
  iteration loop — bump the submodule SHA in the consumer, no further
  refresh step needed.
- **No CI-side prebuild job**. The prebuild runs only on Marcelo's
  laptop. A future target may add a GHA workflow that produces and
  commits prebuilts when a vendor submodule SHA changes; for now,
  manual.
- **No Android / macOS / simulator slices**. iOS arm64 device only.

## Layout

```
ge/
├── prebuilt/ios-arm64/
│   ├── libbgfx.a           1.1 MB    (LFS)
│   ├── libbx.a             268 KB    (LFS)
│   ├── libbimg.a           111 KB    (LFS)
│   ├── libbox2d.a          490 KB    (LFS)
│   ├── liblunasvg_ge.a     392 KB    (LFS)
│   ├── libplutovg_ge.a     302 KB    (LFS)
│   ├── libsqlite3_ge.a     1.5 MB    (LFS)
│   ├── liblz4_ge.a         76 KB     (LFS)
│   └── libliteparser.a     297 KB    (LFS)
├── headers/
│   ├── bgfx/include/bgfx/…           regular
│   ├── bx/include/{bx,compat,tinystl}/… regular
│   ├── bimg/include/bimg/…           regular
│   ├── box2d/include/box2d/…         regular
│   ├── lunasvg/include/lunasvg.h     regular
│   ├── plutovg/include/plutovg.h     regular
│   └── liteparser/include/{arena,liteparser,liteparser_internal,parse}.h
└── tools/
    ├── prebuild-vendor-ios-arm64.sh
    └── lift-headers.sh
```

LFS routing for `*.a` is configured in `ge/.gitattributes`.
