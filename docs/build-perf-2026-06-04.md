# Build performance audit — 2026-06-04

## Summary
- Full `ios-arm64-simulator` prebuild: 87.54s → 24.00s.
- Source-only `ios-arm64-simulator` prebuild: 87.54s full rebuild baseline → 13.27s with `make prebuild-libge-ios-arm64-simulator`.
- All-platform source-only `make prebuild-libge`: 26.01s.
- All-platform full `make prebuild`: 35.31s total after platform-level and per-platform parallelism.
- Repeat simulator full rebuild produced identical archive hashes after enabling deterministic archive timestamps.

## Baseline
Command:

```bash
/usr/bin/time -p make prebuild-ios-arm64-simulator
```

Result:

```text
real 87.54
user 73.15
sys  5.07
```

The script rebuilt every vendor archive plus `libge.a` serially. CPU use was below one effective core for most of the run.

## Findings
- High: `tools/prebuild.sh` rebuilt vendor archives for every ge source/header edit.
- High: per-platform compile loops were serial even though object compilation is independent.
- Medium: archive outputs changed when inputs did not, because Apple static archives were not built with deterministic timestamps.
- Medium: iOS app local development had no source-mode escape hatch; device builds linked stale `libge.a` until prebuilts were refreshed.
- Low: iOS simulator manifests recorded Android toolchain metadata.

## Applied
- Added `tools/prebuild.sh --libge-only <platform>` plus Make targets:
  - `make prebuild-libge`
  - `make prebuild-libge-ios-arm64`
  - `make prebuild-libge-ios-arm64-simulator`
  - `make prebuild-libge-android-arm64`
- Added `tools/write-manifest.py --merge-existing-inputs` so libge-only manifests preserve vendor input hashes and still detect stale vendor archives.
- Parallelized per-file compilation inside each platform prebuild with `GE_PREBUILD_JOBS`, defaulting to host CPU count.
- Set `ZERO_AR_DATE=1` for deterministic Apple archives.
- Added `engine_mode: :source` to the iOS project generator so local apps can compile ge sources inline while release/CI keeps the default `engine_mode: :prebuilt`.
- Fixed simulator manifest toolchain reporting to use `iphonesimulator`.

## After
Source-only simulator:

```bash
/usr/bin/time -p make prebuild-libge-ios-arm64-simulator
```

```text
real 13.27
user 55.76
sys  5.21
```

Full simulator:

```bash
/usr/bin/time -p make prebuild-ios-arm64-simulator
```

```text
real 24.00
user 72.73
sys  5.95
```

Full all-platform:

```bash
/usr/bin/time -p make prebuild
```

```text
real 35.31
user 253.11
sys  18.03
```

Libge-only all-platform:

```bash
/usr/bin/time -p make prebuild-libge
```

```text
real 26.01
user 210.16
sys  12.98
```

## Verification
- `python3 tools/verify-prebuilds.py`
- `make python-test`
- `make ruby-test`

## Deferred
- CI-side prebuild automation remains manual by design.
- The project generator exposes source mode, but consuming apps still need to opt into it explicitly during local development.
