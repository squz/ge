# Handoff: iPadOS 26 letterbox vs iOS 18 full-bleed (direct mode)

**Date:** 2026-07-15  
**Repo:** `squz/ge` (branch work around `cmdstream-passthrough-cache` / stream–direct parity)  
**Goal context:** Stream on M5 should match M4 direct reference; investigation showed **direct mode on M5 already letterboxes**, so streaming is secondary.  
**Constraint from product owner:** Both devices stay in **portrait chassis**; do **not** “fix” by rotating the simulator. App must request landscape and fill glass the way M4 does on iOS 18.6.

---

## 1. Problem statement

### Observed

| Device | Simulator | iOS | Mode | Presentation |
|--------|-----------|-----|------|----------------|
| **M4** | iPad Pro 13-inch (M4) `88FAA2DB-5C85-4DFA-AB07-DB6637924E30` | **18.6** | Direct `com.squz.tiltbuggy` | **Full-bleed.** No black bars. Content appears rotated into the portrait screenshot (dirt band at top, ice vertical, title vertical). No status-bar chrome when immersive. |
| **M5** | iPad Pro 13-inch (M5) `B8B1FF14-B30A-407F-A4EE-344FD3D4E856` | **26.4** | Direct `com.squz.tiltbuggy` (same app class) | **Letterboxed.** Black bars top and bottom. Content upright landscape (dirt left, ice horizontal, title horizontal). |

### What “success” looks like

- Device **chassis remains portrait**.
- Game is **landscape / AnyLandscape** (TiltBuggy: `wire::kOrientationAnyLandscape`, immersive).
- On-glass presentation matches M4: **full surface fill**, same content orientation relative to the frame as the M4 reference (not black gutters + upright letterbox band).
- Oracle image (local, not committed):  
  `ge/.local-ref/m4-direct-immersive.png`  
  (2064×2752 PNG, pose centered, immersive status bar hidden.)

### Explicit non-goals for this handoff

- Do **not** rotate the simulator/device as a workaround.
- Stream player packaging (`Player` / `PlayerLand` / `PlayerPort`) is **not** the primary bug once direct fails the same way on 26.4.
- Byte-identical bitmaps across different GPU/iOS versions are not required; **chrome + fill + content orientation** are.

---

## 2. Why OS version is the key factor (GE prior art)

Authoritative write-up in-repo:

- `AGENTS.md` → section **“iOS orientation lock (iPadOS 26+)”**
- `agents-guide.md` → same topic
- `tools/player_orientation_ios.mm` → banner comment (history of failed approaches)
- `DirectRenderHost.mm` → comment at `send()`: iPadOS 26 ignores Info.plist as sole lock

### Two-knob model (required on iPadOS 26+)

| Knob | Mechanism | Role |
|------|-----------|------|
| **1. Packaging** | `Info.plist` `UISupportedInterfaceOrientations` | Narrows the set iOS may pick **at launch**. Landscape-only plist is required for landscape games. |
| **2. Runtime lock** | `SessionHostConfig.orientation` ≠ 0 → `playerForceOrientation()` | Activates **`prefersInterfaceOrientationLocked`** (Apple **TN3192**, iPadOS 26+) so multitasking cannot re-swivel mid-play. |

**Critical GE finding (do not forget):**

> Knob 2 **freezes whatever orientation launch already settled into**.  
> It is **not** by itself “force landscape while the user holds portrait.”  
> Without knob 1 doing real launch rotation, locking freezes **portrait** → wrong aspect / letterbox.

On **older iOS (e.g. 18.6)**, knob 1 still tends to **rotate the UI into a supported orientation at launch** even if the chassis is portrait.  
On **iPadOS 26+**, multitasking treats iPad apps as resizable; **plist alone is insufficient** as a runtime restriction, and launch/geometry behaviour **diverges** from 18.x — this is exactly the failure class GE already documented for stream players (wrong launch orientation class → DeviceInfo/content aspect disagree → letterbox). Direct is now showing the same class of symptom on 26.4.

### Things already proven *not* to work alone

Documented in `player_orientation_ios.mm` / AGENTS (do not re-discover as “the” fix):

- `UIRequiresFullScreen`
- `SDL_HINT_ORIENTATIONS` alone
- `UIWindowScene requestGeometryUpdate` alone (silent no-op on iPad)
- `setNeedsUpdateOfSupportedInterfaceOrientations` alone
- Info.plist alone on iPadOS 26 (see commit `e0da016` revert of plist-only experiment)
- Boolean lock alone without a correct launch/supported set (commit `5c2f2a5` completed the TN3192 picture for *holding* orientation after launch)

T36 (`9c9b566`, v0.31.0) added **`supportedInterfaceOrientations` swizzle** so specific `kOrientation*` constants narrow the runtime mask (not only “lock current”).

### Three stream players (🎯T154.3) — context only

Knob 1 is **out-of-band packaging** (cannot be set purely from SessionConfig at attach). That is why GE ships **Player / PlayerLand / PlayerPort** with different plists.  
**For this bug, focus on direct TiltBuggy**, which already has landscape-only plist + `AnyLandscape` + immersive.

---

## 3. App configuration under test (TiltBuggy direct)

```text
SessionHostConfig:
  orientation = wire::kOrientationAnyLandscape   // 0xFE
  immersive   = true
  sensors     = kSensorAccelerometer

Info.plist UISupportedInterfaceOrientations:
  LandscapeLeft, LandscapeRight only

Bundle ID: com.squz.tiltbuggy
```

Build path used in investigation:

```bash
# Engine prebuild for sim
tools/prebuild.sh --libge-only ios-arm64-simulator

# App
cd sample/tiltbuggy && make ge/ios
# Product:
# sample/tiltbuggy/ios/build/Build/Products/Debug-iphonesimulator/TiltBuggy.app
```

Install/launch (sim, no chassis rotate):

```bash
M5=B8B1FF14-B30A-407F-A4EE-344FD3D4E856
xcrun simctl install "$M5" …/TiltBuggy.app
xcrun simctl launch --console-pty "$M5" com.squz.tiltbuggy
xcrun simctl io "$M5" screenshot /path/to/out.png
```

Pose durability (can leave car off-center across launches):

```text
~/Library/Developer/CoreSimulator/Devices/<UDID>/data/Containers/Data/Application/<…>/
  Library/Application Support/squz/tiltbuggy/game.db
→ DELETE FROM pose;   # before launch if you need center
```

---

## 4. Evidence from the last direct deploy on M5 (still letterboxed)

After rebuilding `libge` (orientation + immersive) and redeploying **direct** TiltBuggy to M5:

### Logs (engine claims landscape surface)

```text
playerForceOrientation: swizzled orientation hooks on UIViewController
playerForceOrientation: swizzled orientation hooks on SDL_uikitviewcontroller
SokolContext: 2752x2064 Metal (Apple iOS simulator GPU)
DirectRenderHost: 2752x2064
applyImmersive(iOS): statusBar hidden=true
playerForceOrientation: lock AnyLandscape (mask=0x18)
tiltbuggy: arena aspect → 1.407 (drawSafe 1376x978)
```

### Screenshot (presentation still wrong)

- Dimensions: **2064×2752** (portrait chassis capture — same as M4 ref size class).
- **Black letterbox** bands top and bottom.
- Content **upright landscape** (dirt on left, ice horizontal).
- Contrast M4 ref: **no black bars**, content **rotated** in the portrait bitmap (dirt on top, ice vertical).

### Contradiction to resolve

| Source | Claims |
|--------|--------|
| Sokol / DirectRenderHost | Drawable **2752×2064** (landscape) |
| simctl screenshot | Frame **2064×2752** with **letterbox** of upright landscape content |

So either:

1. The engine’s pixel size is **not** the same as the **presented** full-screen glass (window scene / letterboxing outside the metal layer), or  
2. On 26.4 the UI is **not** completing a true landscape geometry transition the way 18.6 did, while SDL still reports landscape-ish sizes, or  
3. Screenshot composition on 26.4 differs — but M4’s full-bleed rotated capture shows that “portrait simctl + landscape app” *can* look correct without black bars.

**Black bars are the smoking gun:** something on the presented surface is not full-bleed.

---

## 5. Code paths that matter

### Direct orientation call chain

```text
ge::run(...)
  → runDirectHosted
       → DirectRenderHost host(config)     // SokolContext creates SDL window
       → applyImmersive(config.immersive)
       → factory(...)
       → host.send(SessionConfig)          // playerForceOrientation(orientation)
```

- Window creation: `src/SokolContext.mm`  
  iOS flags: `SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY` (**not** `RESIZABLE`).
- Orientation lock: `tools/player_orientation_ios.mm` → `playerForceOrientation`  
  Linked into **libge** for direct iOS (`GE_SRC_DIRECT_IOS` includes `GE_SRC_ORIENTATION_IOS` in `tools/ge-sources.mk`).
- Immersive: `src/Immersive_apple.mm` (status bar hide; separate from letterbox).

### Stream path (secondary; same OS issue can appear)

```text
player_core
  → SessionConfig from wire
  → SDL_HINT_ORIENTATIONS
  → PlayerRender (window) + applyImmersive + playerForceOrientation
  → fillDeviceInfo → server retargets content aspect
  → PlayerRender::fitContentRect letterboxes if content aspect ≠ window aspect
```

Player had extra issues (e.g. `SDL_WINDOW_RESIZABLE` on mobile; DeviceInfo dimension swap before settle). **Do not start there** if direct on M5 still letterboxes.

### SDL trap: glass VC overrides orientation APIs

`vendor/.../SDL/src/video/uikit/SDL_uikitviewcontroller.m`:

```objc
- (NSUInteger)supportedInterfaceOrientations {
    return UIKit_GetSupportedOrientations(window);
}
```

That **does not call super**. A swizzle only on `UIViewController` **never runs** for the real glass VC.

`UIKit_GetSupportedOrientations` (`SDL_uikitwindow.m`) combines:

- App/plist valid orientations  
- `SDL_HINT_ORIENTATIONS`  
- Fallback: if **RESIZABLE** and no hint mask → **all orientations**  
- Else floating w/h → landscape or portrait mask  

**Implication:** Runtime lock that only swizzles `UIViewController` is incomplete for SDL-hosted glass (both direct Metal and player Renderer). Recent work attempted to also swizzle `SDL_uikitviewcontroller` by name; **M5 direct still letterboxed after that**, so either the swizzle is insufficient for *rotation* on 26.4, or presentation mismatch is elsewhere.

### Immersive vs letterbox

Immersive hides status bar (non-deterministic chrome). **It does not explain black letterbox bands.** Treat as orthogonal once chrome is gone.

---

## 6. Work already attempted (this investigation)

| Attempt | Outcome |
|---------|---------|
| Stream PlayerLand on M5 + cmdstream | Letterbox; distracted from root cause |
| Manually rotate M5 sim | Ruled out by product owner; do not use |
| iOS immersive implementation (`Immersive_apple.mm`) | Status bar can hide; **not** the letterbox fix |
| Swizzle `SDL_uikitviewcontroller` for orientation (in addition to `UIViewController`) | Logs show swizzle armed; **M5 direct still letterboxed** |
| Drop `SDL_WINDOW_RESIZABLE` on mobile PlayerRender | Player-only; irrelevant while direct fails |
| DeviceInfo settle wait in `player_core` | Stream-only; not the direct failure |
| Rebuild libge + redeploy **direct** TiltBuggy to M5 | Logs 2752×2064 + AnyLandscape lock; **screenshot still letterboxed** |

### Reference artifact

- M4 oracle: `ge/.local-ref/m4-direct-immersive.png` (gitignored local ref; full-res)  
- Scratch captures from goal work (if still present): under implementer scratch dir used by the goal harness (paths like `…/grok-goal-…/implementer/m5-direct.png`)

---

## 7. Hypotheses ranked for the next agent

1. **iPadOS 26 does not complete launch/runtime geometry into true full-screen landscape** while chassis is portrait, even with landscape plist + TN3192 lock + supported-orientations swizzle. Engine then draws a landscape-aspect surface that is **composited with letterboxing** into a portrait scene (or vice versa). M4/18.6 still performs launch rotation so drawable and glass agree full-bleed.

2. **SDL reports landscape `GetWindowSizeInPixels` / Metal `drawableSize` that does not match `UIWindow` / `UIWindowScene` bounds** on 26.4 → game fills “logical landscape” but the OS presents letterboxed.

3. **Swizzle targets or timing are still wrong** for the VC that UIKit actually consults on 26.4 (e.g. child VC, scene-level API, or need for `requestGeometryUpdate` *with* a correct supported mask *after* the metal view is attached — alone it’s a no-op; stack may be incomplete).

4. **Less likely:** pure game letterbox math (TiltBuggy arena) — M4 full-bleed with same game argues against app-level content letterbox as the sole cause.

5. **Ruled out as primary:** stream-only player packaging; chassis rotation as required fix.

---

## 8. Recommended investigation plan (next agent)

### A. Same binary, two OS versions (highest value)

Install **identical** `TiltBuggy.app` (same build) on:

- M4 @ 18.6  
- M5 @ 26.4  

Chassis **portrait** on both. Capture:

1. simctl screenshot  
2. Log lines: `SokolContext: WxH`, `playerForceOrientation`, immersive  
3. **New instrumentation** (add temporarily if needed):

```text
After host construct + after playerForceOrientation (+ ~200ms settle):
  - SDL_GetWindowSize / SizeInPixels
  - SDL_GetWindowSafeArea
  - UIKit: key window.bounds, screen.bounds
  - UIWindowScene.effectiveGeometry / interfaceOrientation (iOS 16+)
  - CAMetalLayer.drawableSize
  - UIDevice.orientation vs interface orientation
```

Compare M4 vs M5 tables. The first field that **diverges** while screenshots diverge is the lead.

### B. Confirm what the black bars are

- Engine clear color vs system letterbox: e.g. clear to magenta for one frame; if bars stay black, OS composition; if bars go magenta, engine `fit`/viewport.

### C. Re-read GE law before inventing knobs

- Do not “solve” by rotating the sim.  
- Do not assume plist alone or lock alone.  
- Do not re-try the listed no-ops *in isolation*.  
- Prefer fixing **one shared orientation/presentation path** used by direct (and then stream).

### D. Acceptance for a fix

With chassis portrait:

1. M5 direct screenshot: **no black letterbox**, content orientation vs frame matches M4 reference class (full-bleed; rotated presentation in portrait capture is OK if that is how 18.6 looks).  
2. Logs: consistent drawable + UIKit geometry after settle.  
3. Immersive: no status-bar chrome.  
4. Pose center optional for visual compare (clear `pose` table if needed).  
5. Stream parity can follow once direct-on-26.4 matches; not a prerequisite to claim this bug fixed.

---

## 9. File index

| Path | Why |
|------|-----|
| `AGENTS.md` (§ iOS orientation lock) | Two-knob model, iPadOS 26, three players |
| `agents-guide.md` | Condensed same |
| `tools/player_orientation_ios.mm` | Swizzles, history, `playerForceOrientation` |
| `tools/player_orientation.h` | C API |
| `src/render/DirectRenderHost.mm` | `send()` → force orientation |
| `src/SokolContext.mm` | iOS window flags / size |
| `src/Immersive_apple.mm` | Status bar only |
| `src/SessionHost.mm` | `runDirectHosted` order: host → immersive → send |
| `sample/tiltbuggy/src/main.cpp` | `orientation` / `immersive` config |
| `sample/tiltbuggy/ios/…/Info.plist` | Landscape-only packaging |
| `vendor/.../SDL/.../SDL_uikitviewcontroller.m` | Overrides `supportedInterfaceOrientations` |
| `vendor/.../SDL/.../SDL_uikitwindow.m` | `UIKit_GetSupportedOrientations` |
| `docs/device-api-migration.md` / T154.3 notes in `bullseye.yaml` | Stream letterbox residual, packaging variants |
| `.local-ref/m4-direct-immersive.png` | M4 oracle (local) |

### Useful git archaeology

| Commit | Note |
|--------|------|
| `e0da016` | Revert plist-only experiment |
| `5c2f2a5` | TN3192 `prefersInterfaceOrientationLocked` |
| `9c9b566` | T36: specific orientation constants + supportedOrientations swizzle |

---

## 10. One-paragraph summary for the next agent

**Direct TiltBuggy on iPadOS 26.4 (M5) letterboxes while the chassis stays portrait; the same product on iOS 18.6 (M4) full-bleeds with rotated content in the portrait screenshot.** GE’s documented model is two knobs (landscape plist + TN3192 runtime lock); on 26.x the lock freezes launch settlement and plist alone no longer guarantees the 18.x launch-rotate behaviour. Engine logs on M5 report landscape `2752×2064`, but simctl still shows black bars and upright landscape content — so either drawable size and presented glass disagree, or 26.4 never completes a full-bleed landscape geometry transition. Streaming is a red herring until direct-on-26.4 matches the M4 reference. Next work: instrument SDL vs UIKit vs Metal geometry on both OS versions with one binary, identify the first diverging field, and fix the shared iOS presentation path without rotating the device.

---

## 11. 2026-07-15 follow-up: app force works; simctl letterbox is host composite

Same TiltBuggy binary on M4 (iOS 18.6) and M5 (iOS 26.4), chassis portrait-class:

| Metric | M4 | M5 |
|--------|----|----|
| UIScreen.bounds (pts) | 1376×1032 landscape | 1376×1032 landscape |
| nativeBounds | 2064×2752 portrait | 2064×2752 portrait |
| interfaceOrientation | 3 (LandscapeRight) | 3 (LandscapeRight) |
| window.bounds | 1376×1032 (= screen) | 1376×1032 (= screen) |
| SokolContext | 2752×2064 | 2752×2064 |
| simctl PNG | full-bleed rotated | scale-to-fit letterbox |

**Conclusion:** Direct mode **does force landscape glass** on M5 with portrait chassis.
There are **no gutters between content and UIKit screen edges** (window fills bounds).
Black bars in `simctl io screenshot` on iOS 26.4 are the **Simulator host** mapping a
landscape guest framebuffer onto portrait `nativeBounds` (scale-to-fit). iOS 18.6 maps
the same guest metrics with a 90° full-bleed composite. Further UIKit lock tweaks cannot
change that once bounds already equal the landscape screen.

Code improvements retained: early `playerForceOrientation` before `CreateWindow`,
SDL glass-VC swizzle, TN3192 freeze only after interface matches mask.
