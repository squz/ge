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
- **`tools/verify-cook.py`** — link-time verifier (Android cmake FATAL_ERROR;
  iOS Xcode "Verify prebuilt cook" script phase; `ensure-prebuilt.sh`).

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

Do **not** enable LFS smudge for ge prebuilts. Options:

1. Cook in CI (`make prebuild` with the right SDK/NDK), or
2. Restore archives from an Actions cache / artefact (follow-up).

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
├── headers/                      (plain files in git)
└── tools/
    ├── prebuild.sh
    ├── ensure-prebuilt.sh        (local cook when stale; do not commit outputs)
    ├── verify-cook.py            (link-time archive↔cook gate)
    └── verify-prebuilds.py       (rejects committed binaries)
```

`.gitattributes` keeps `prebuilt/**/cook.json` and `manifest.json` as
text. Binary LFS rules are gone. `scripts/hooks/pre-commit` rejects
staging `prebuilt/**/*.a` (and related patterns).
