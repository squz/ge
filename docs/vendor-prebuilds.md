# Prebuilds

ge cooks prebuilt static libs and lifts public headers for vendor
dependencies **plus libge itself** so local and CI builds can link without
recompiling every vendor source on every iteration.

**Phase 1 LFS exit (2026-09):** prebuilt **archives are a local cache only**.
They are **not** committed to git / Git LFS. Cook on your machine (or in CI)
with `make prebuild` / `tools/ensure-prebuilt.sh`. Do **not**
`git add prebuilt/**/*.a`.

What still lives in git:

- **`prebuilt/<platform>/cook.json`** and **`manifest.json`** — cook identity
  + staleness metadata (plain text).
- **`headers/<dep>/include/`** — lifted public-header subset (~14 MB), plain
  files.

Local artefacts (gitignored):

- **`prebuilt/{ios-arm64,ios-arm64-simulator,android-arm64,web-wasm}/lib*.a`**
- **`prebuilt/<platform>-debug/`** — same archives with **`-g -O2
  -fno-omit-frame-pointer`**. Built with `tools/prebuild.sh --debug
  <platform>` or `make prebuild-android-arm64-debug`. Android
  `assembleDebug` selects the debug tree via `cmake/android-arm64.cmake`.

The `_ge` suffix on lunasvg, plutovg, sqlite3, lz4 avoids name clashes
with SDL3's bundled plutosvg/plutovg and any system-installed sqlite3.

The Apple iOS xcodeproj generator
(`ge/tools/ios-build/build_project.rb`) wires headers + local prebuilt
dirs into the project it emits. The consumer's xcodeproj compiles the
game's own sources plus `src/iap_apple.swift`.

## Two archive trees, two cooks

`prebuilt/` is ge's own archives (libge, box2d, lunasvg_ge, sqlite3_ge, …).
`vendor/sdl3/lib/<platform>/` is a **separate** tree holding SDL3 and its
dependency chain:

```
libSDL3.a  libSDL3_image.a  libSDL3_ttf.a
libfreetype.a  libharfbuzz.a  libplutosvg.a  libplutovg.a
```

Both are local caches, never committed, and each has its own cook:

| Tree | Cook | Wired into |
|---|---|---|
| `prebuilt/<platform>/` | `tools/prebuild.sh` via `tools/ensure-prebuilt.sh` | the mobile targets |
| `vendor/sdl3/lib/<platform>/` | `tools/rebuild-sdl-prebuilts.sh` via `tools/ensure-sdl.sh` | the desktop link rules + the iOS lanes |

**How a desktop consumer obtains SDL3 / freetype / harfbuzz / plutosvg /
plutovg:** it does not obtain them — it cooks them, and it does so without
being asked to. `ge/SDL_LIBS` is a prerequisite of every desktop link rule in
`Module.mk` (`$(APP)`, `$(APP_DEBUG)`, `$(ge/TEST_BIN)`, `$(ge/ICON_GEN)`), and
a pattern rule under `vendor/sdl3/lib/macos-arm64/` answers a missing archive by
running `tools/ensure-sdl.sh macos-arm64`. So `make` at a consumer root, in a
recursive clone with no archives anywhere, compiles the app, cooks SDL from the
submodule, and links. There is no separate step and no hand-run script.

`ensure-sdl.sh` fast-exits when the platform's whole set is present, so the
check is free on every subsequent build. Make asks for it once per missing
archive and, under `-j`, concurrently, so the script takes a `mkdir` mutex
under `build/` — the first caller cooks, the rest wait and then find the
archives in place. `GE_ENSURE_SDL_LOCK_TIMEOUT` (seconds, default 1800)
bounds that wait.

`tools/rebuild-sdl-prebuilts.sh` takes the platforms to cook as arguments —
`macos-arm64`, `ios-arm64`, `ios-arm64-simulator`, `android-arm64`,
`xcframeworks`, or `all` (the default, and what it always did before). A
macOS-only consumer therefore pays for macOS only, not for two iOS slices,
Android and the xcframeworks as well.

iOS links this tree through xcodebuild's `LIBRARY_SEARCH_PATHS`
(`tools/ios-build/build_project.rb`), not through make prerequisites, so
`ge/ios`, `ge/ios-release`, `ge/ios-device` and `ge/ios-device-release` call
`ensure-sdl.sh` explicitly, next to their `ensure-prebuilt.sh` call. Android
needs neither: `cmake/android-arm64.cmake` builds SDL3 / SDL3_image / SDL3_ttf
from the submodule with `add_subdirectory`.

**Staleness is not tracked here.** `ensure-sdl.sh` asks only "is the archive
there?", where `ensure-prebuilt.sh` asks the much stronger "was it cooked from
the sources on disk now?" (`verify-manifest.py` + `verify-cook.py`). After
bumping the SDL submodule, re-cook by hand:

```bash
rm -rf vendor/sdl3/lib/macos-arm64      # or the whole lib/ tree
tools/rebuild-sdl-prebuilts.sh macos-arm64
```

## Why this exists

Before T71, multimaze2's GHA workflow checked out ge with
`submodules: recursive`, which pulled ~150 MB of nested vendor repos and
recompiled them every CI run. T71 moved vendor compilation to a cook
keyed off submodule SHAs. Archives were temporarily shipped via Git LFS;
that path is retired because Squz org LFS quota was exhausted by
historical cook OIDs.

Today: cook locally (or cache in Actions as a follow-up). Consumer CI
should **not** LFS-smudge ge archives.

## Debug prebuilts (native stepping)

```bash
tools/prebuild.sh --debug android-arm64
# or
make prebuild-android-arm64-debug

# True no-opt (only if you know you want it):
GE_PREBUILD_OPT=0 tools/prebuild.sh --debug android-arm64
```

`make ge/android` (assembleDebug) auto-refreshes the debug tree via
`ensure-prebuilt.sh --debug` and links it. Release packages keep using
`prebuilt/android-arm64/`.

### Co-cook is mandatory (no partial refresh)

Partial `--libge-only` cooks are **gone**. Every run rebuilds **all**
archives under the same flags.

Hard gates:

- **Always full cook** — `prebuild.sh` never reuses vendor archives.
- **Separate trees** — `prebuilt/<platform>/` vs `prebuilt/<platform>-debug/`.
- **`cook.json`** — SHA-256 of every `.a` from that cook.
- **`tools/verify-cook.py`** — link-time *integrity* verifier: do the `.a`
  on disk match `cook.json`? (Android cmake FATAL_ERROR; iOS Xcode "Verify
  prebuilt cook" script phase; `ensure-prebuilt.sh`).
- **`tools/verify-manifest.py`** — the *staleness* oracle: were these
  archives cooked from the sources on disk now? Checks the manifest's
  recorded script hashes, submodule SHAs, input hashes and the Android NDK
  pin (🎯T100). This is what `ensure-prebuilt.sh` consults before declaring
  a tree fresh.

Three separate questions, three tools — do not merge them again:

| Question | Tool |
|---|---|
| Were these archives cooked from *these* sources? | `verify-manifest.py` |
| Do the archives on disk match the cook that produced them? | `verify-cook.py` |
| Is anyone committing binaries to git? | `verify-prebuilds.py` |

Only scripts that can change **archive bytes** (`prebuild.sh`,
`lift-headers.sh`) are recorded in the manifest's `scripts` set. Verifiers
and `write-manifest.py` are deliberately excluded — listing them made
"we changed how we check" mean "every consumer rebuilds everything".

## Refresh workflow

```bash
# 1. Bump a vendor submodule (example).
cd ge/vendor/github.com/bkaradzic/bgfx
git fetch && git checkout <new-sha>
cd ../../../../..

# 2. Re-prebuild + re-lift locally (do NOT commit .a files).
make prebuild
make ge/lift-headers

# 3. Commit submodule bump + headers + cook/manifest text only.
git add vendor/github.com/bkaradzic/bgfx headers/ \
  prebuilt/**/cook.json prebuilt/**/manifest.json
git commit -m "bgfx: bump to <new-sha>; refresh headers + cook manifests"
# Never: git add prebuilt/**/*.a
```

Both scripts assume submodules are initialized
(`git submodule update --init --recursive`). For ordinary ge source
edits, prefer `make ge/ios` / `ge/ios-device` / `ge/android` so
`ensure-prebuilt.sh` full-cooks when stale.

## Android NDK ABI pin (🎯T100)

The `android-arm64` prebuilt must stay on **NDK r27** (or ≤ every
consumer's NDK). Static archives bake libc++ exception-ABI references;
a newer cook NDK than the consumer breaks the final link.

- `tools/prebuild.sh` — `GE_ANDROID_NDK_MAJOR` (default `27`)
- Manifest `toolchain.ndk_path` records the cook NDK; link-time
  `verify-cook.py` / cmake gates still apply locally.

**Bumping the pin:** change `GE_ANDROID_NDK_MAJOR` in `tools/prebuild.sh`
(and any pin asserts), re-cook locally, confirm consumers still link.

## Why submodules are kept

Submodules under `ge/vendor/github.com/<org>/<repo>/` remain the source
of truth for vendor versions. Prebuilt `.a` files are **derived local
artefacts**, not the source of truth in git.

Local iteration:

- **`make ge/ios` / `ge/ios-device` / `ge/android`** — `ensure-prebuilt.sh`
  full-cooks when manifest/cook is stale.
- iOS `engine_mode: :source` — compile ge sources into the app while
  still linking vendor prebuilts (cook gate still applies).

Escape hatch: `GE_SKIP_ENSURE_PREBUILT=1 make ge/ios` skips the check
(do not use casually).

## Consumer CI

Do **not** enable LFS smudge for ge prebuilts — there is nothing behind
those pointers any more.

Consumers need no per-app change. `make ge/ios`, `ge/ios-device`,
`ge/ios-release`, `ge/ios-device-release`, `ge/android`,
`ge/android-release` and `ge/android-bundle` all run
`tools/ensure-prebuilt.sh` for the platform they link (🎯T181.3), so a
fresh clone with an empty `prebuilt/` tree cooks once and then builds.
Plain `make` (the desktop leg) cooks `vendor/sdl3/lib/macos-arm64` the same
way, through `tools/ensure-sdl.sh` (🎯T190 / 🎯T195) — see "Two archive trees,
two cooks" above.
That check sits *after* each target's `ios/` / `android/` scaffolding
guard, so a missing project still fails immediately rather than after a
full cook. `GE_SKIP_ENSURE_PREBUILT=1` opts out and will happily link a
stale tree.

The first build on a fresh clone therefore pays a full co-cook, and
consumer CI must check ge out **recursively** so the vendor sources are
present to cook from.

**There is no download path, by decision.** Publishing archives as
GitHub Release assets and resolving them from an exact tag were once
planned as 🎯T181.1 / 🎯T181.2; both were cancelled on 2026-09-20 in
favour of source-only. With a handful of developers barely touching
these trees, a per-consumer cook is cheaper than operating a release-
asset pipeline and a version-pinned resolver — and it removes the
release pointer from the ingest path entirely, so a game never waits on
a ge tag to pick up engine changes. Revisit only if a full cook stops
being affordable.

Checkout ge with `submodules: true` (not recursive) for headers +
cook/manifest text only.

## Things this does *not* do

- **No committing archives** — gitignore + pre-commit + CI reject `.a`.
- **No history rewrite in Phase 1** — tip drops tracked binaries; Phase 2
  (`git filter-repo` + GitHub Support LFS purge) is a separate cutover.
- **No CI-side prebuild job yet** — cook remains laptop/CI-local for now.
- **No prebuilt Swift bridge** — `src/iap_apple.swift` still compiles in
  the consumer xcodeproj.

## Layout

```
ge/
├── prebuilt/<platform>/          (local .a gitignored; cook.json + manifest.json tracked)
├── vendor/sdl3/lib/<platform>/   (local .a gitignored; SDL3 + freetype/harfbuzz/plutosvg/plutovg)
├── headers/                      (plain files in git)
└── tools/
    ├── prebuild.sh
    ├── ensure-prebuilt.sh        (cooks when stale; wired into every mobile target)
    ├── rebuild-sdl-prebuilts.sh  (the vendor SDL cook; takes platforms as arguments)
    ├── ensure-sdl.sh             (cooks when missing; wired into the desktop link + iOS lanes)
    ├── verify-manifest.py        (staleness: sources ↔ manifest)
    ├── verify-cook.py            (integrity: archives ↔ cook.json)
    └── verify-prebuilds.py       (rejects committed binaries)
```

`.gitattributes` keeps `prebuilt/**/cook.json` and `manifest.json` as
text. Binary LFS rules are gone. `scripts/hooks/pre-commit` rejects
staging `prebuilt/**/*.a` (and related patterns).
