> **Historical / superseded.** The Dawn command-stream "ge Remote" path and the `ged` broker are no longer the supported dev loop. Use **spyder** + direct mode (or server-mode H.264 through spyder's stream relay). Ports below that say 3030 reflect the spyder default; this document is kept only as archaeology.

# ge Remote - Mobile Player Setup

ge Remote is a mobile companion app that renders the game on an iOS or Android device. The game runs as a headless server on your Mac, streaming WebGPU draw calls over the local network to the phone/tablet via Dawn's wire protocol.

You don't need to modify ge Remote to develop the game. The mobile app is a thin player - all game logic runs in the parent repo. This guide covers how to build the player, install it on a device, and connect it to your development server.

## How it works

1. `make run` starts the game as a wire server and prints a QR code in the terminal
2. ge Remote on the phone scans the QR code to discover the server's IP and port
3. The server streams all rendering commands over TCP; the phone replays them on its GPU
4. Touch input on the phone is sent back to the server

When the phone disconnects (app backgrounded, Wi-Fi drop, etc.), the server waits for a new connection and resumes from where it left off.

## Prerequisites

Both platforms require a one-time Dawn cross-compilation. The prebuilt libraries are committed to the repo via Git LFS, so **if you just cloned the repo you already have them** and can skip the "Build dependencies" step.

You only need to re-run `build-deps.sh` if you update the Dawn version (the commit hash at the top of each script).

### Common

- macOS with Xcode Command Line Tools
- The `ge` submodule initialised: `git submodule update --init`

### iOS

- Xcode (full install, not just CLI tools)
- An Apple Developer account (free is fine for personal devices)

### Android

- [Android Studio](https://developer.android.com/studio) with:
  - Android SDK (API 35)
  - Android NDK (any recent version, e.g. 27+)
  - CMake (via SDK Manager > SDK Tools)
- A USB-connected Android device with Developer Mode enabled
- Java 17+ (bundled with Android Studio, or install separately)

## Desktop player

For quick iteration without a phone, use the desktop player:

```bash
make player             # Build
bin/player              # Run (connects to localhost:3030 by default)
```

Run `make run` in another terminal to start the server. The desktop player connects automatically without QR scanning.

## iOS

### Build dependencies (one-time)

Skip this if the prebuilt libraries already exist at `ge/vendor/dawn/lib/ios-arm64/`.

```bash
cd ge/tools/ios
bash build-deps.sh      # Cross-compiles Dawn and SDL3 for iOS (~20 min)
```

### Generate Xcode project

```bash
make ge/ios
```

This runs CMake to generate `ge/tools/ios/build/xcode/Player.xcodeproj`.

### Build and run

1. Open `ge/tools/ios/build/xcode/Player.xcodeproj` in Xcode
2. Select your iOS device as the build target
3. Set the development team in Signing & Capabilities (your Apple ID)
4. Build and run (Cmd+R)

The app opens a camera viewfinder for QR scanning.

## Android

### Build dependencies (one-time)

Skip this if the prebuilt libraries already exist at `ge/vendor/dawn/lib/android-arm64/`.

```bash
cd ge/tools/android
bash build-deps.sh      # Cross-compiles Dawn for Android arm64 (~20 min)
```

The script auto-detects the NDK from `~/Library/Android/sdk/ndk/`. Set `ANDROID_NDK_ROOT` if yours is elsewhere.

### Configure local SDK path

Create `ge/tools/android/local.properties` (git-ignored) with your SDK path:

```
sdk.dir=/Users/YOUR_USERNAME/Library/Android/sdk
```

### Build and install

```bash
make ge/android         # Build APK
adb install ge/tools/android/app/build/outputs/apk/debug/app-debug.apk
```

Or build and install in one step:

```bash
cd ge/tools/android
./gradlew installDebug
```

Launch "ge player" from the app drawer. It opens Google's barcode scanner for QR scanning.

## Connecting to the game server

In a separate terminal:

```bash
make run
```

The server prints a QR code in the terminal. Scan it with the phone. The game should appear within a few seconds.

Both devices must be on the same Wi-Fi network. The QR code encodes `ge-remote://<lan-ip>:3030`.

## Troubleshooting

### Phone doesn't connect after scanning

- Check that port 3030 isn't blocked by a firewall
- Verify the server's LAN IP is reachable from the phone (`ping` from the phone)

### Android: "SDK location not found"

Create `ge/tools/android/local.properties` with `sdk.dir=<path to your Android SDK>`.

### Android: QR scanner doesn't open

Google Code Scanner requires Google Play Services. It won't work on emulators or devices without Play Services.

### build-deps.sh fails

The script clones Dawn into a local build directory on first run. If it fails partway through, delete the build directory and retry:

```bash
rm -rf ge/tools/ios/build    # or ge/tools/android/build
```
