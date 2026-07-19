# OrientProbe — orientation / letterbox / accelerometer research harness

Bespoke minimal probe apps used to ground-truth OS orientation behavior for
`docs/orientation-report.md` (§6 validation matrix, §7 results). No ge
dependency — pure UIKit / pure android.view — so what they measure is the OS,
not the engine.

## iOS (`main.swift`, `build.sh`)

One Swift binary, seven `.app` packagings differing only in Info.plist:

| Variant | Plist orientations | Extra key | Binary |
|---|---|---|---|
| ProbeLand | LandscapeL+R | — | ios16 |
| ProbePort | Portrait+UpsideDown | — | ios16 |
| ProbeAll | all four | — | ios16 |
| ProbeLandRFS | LandscapeL+R | `UIRequiresFullScreen` | ios16 |
| ProbeLandDRC | LandscapeL+R | `UIDesignRequiresCompatibility` | ios16 |
| ProbeAll26 | all four | — | ios26 (freeze-capable) |
| ProbeLand26 | LandscapeL+R | — | ios26 (freeze-capable) |

Runtime behavior via env (`SIMCTL_CHILD_*`):
`PROBE_SUPPORTED=land|port|all` (runtime `supportedInterfaceOrientations`),
`PROBE_FREEZE=1` (TN3192 `prefersInterfaceOrientationLocked`, ios26 binaries
only), `PROBE_FREEZE_DELAY=<sec>`.

The view paints scene-edge markers (red = top, blue = leading, yellow =
corners) and a metrics label (screen/window pt, interface + device orientation,
`effectiveGeometry.isInterfaceOrientationLocked`, safe insets), and prints a
`METRICS` line per second.

```bash
bash build.sh
xcrun simctl install <udid> build/ProbeLand.app
SIMCTL_CHILD_PROBE_FREEZE=1 xcrun simctl launch <udid> com.squz.orientprobe.ProbeLand26
xcrun simctl io <udid> screenshot out.png
```

Rotate the sim via mobile-mcp `mobile_set_orientation` (or Simulator ⌘←/→).

## Android (`android/`)

One APK (no gradle — aapt2 + javac + d8 + apksigner), four activities
differing only in `android:screenOrientation`: MainActivity (unspecified),
LandActivity (`landscape`), SensorLandActivity (`sensorLandscape`),
PortActivity (`portrait`). Same edge markers; overlay + logcat tag
`OrientProbe` report display rotation, `WindowMetrics` current-vs-maximum
bounds (letterbox detector: `win != display`), multi-window flag, and raw
device-frame accelerometer.

```bash
bash android/build.sh
adb install -r android/out/OrientProbe.apk
adb shell 'am start -n "com.squz.orientprobe/com.squz.orientprobe.Probe\$SensorLandActivity"'
adb logcat -s OrientProbe:I
# tablet display-compat posture on any device:
adb shell wm set-ignore-orientation-request true   # (remember to restore: false)
# force a rotation preference without gravity (partial hardware workaround):
adb shell settings put system accelerometer_rotation 0
adb shell settings put system user_rotation 1      # restore accelerometer_rotation 1 after
# emulator: rotate the virtual device (sensors included):
adb emu rotate
```

**targetSdk matters**: this manifest targets SDK 36, which on Android 16 large
screens gets orientation requests *discarded via resize* (full-display window
in the user rotation). Rebuild with `targetSdkVersion=35` to observe the
letterbox-compat path ge apps currently get (ge template targets 35).

## Findings

See `docs/orientation-report.md` §7. Headlines proven with these probes:
iPadOS 26 Windowed-Apps compositor letterbox (cures on physical rotation;
Full-Screen-Apps mode restores 18.x behavior); TN3192 freeze **pins** the
letterbox (rotation stops curing it); `UIRequiresFullScreen` /
`UIDesignRequiresCompatibility` inert; Android `sensorLandscape` = free live
landscape↔landscape flips; `ignoreOrientationRequest` default-true on real
tablets; Android 16 + targetSdk 36 discards tablet orientation locks by
resizing instead of letterboxing.
