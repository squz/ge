# Findings: iPadOS 26 letterbox vs iOS 18 full-bleed (direct mode)

**Date:** 2026-07-15
**Responds to:** [`ios26-orientation-letterbox-handoff.md`](ios26-orientation-letterbox-handoff.md)
**Status:** Root cause identified and empirically confirmed. No engine bug. No app-side fix exists on iPadOS 26.4; test-environment fix and product options below.

---

## 1. Verdict

**The letterbox is produced by iPadOS 26's windowing system ("Windowed Apps" mode, the OS default on 26+), outside the app process.** Under it, iPadOS no longer rotates a fixed-orientation app's interface into a supported orientation at launch. The app gets a true full-resolution landscape scene — exactly what the engine logs — and the **system compositor** scales that scene down and centers it in the mismatched portrait glass.

Nothing in ge's orientation stack is malfunctioning. The plist, the TN3192 swizzles, and `requestGeometryUpdate` all execute as designed; the composition step they cannot reach is where the bars come from. Switching the OS to **Full-Screen Apps** mode restores the iOS 18.6 behavior wholesale — same binary, same portrait chassis, zero bars, launch rotation included.

## 2. The handoff's §4 contradiction, dissolved arithmetically

The handoff flagged a contradiction: engine logs claim a landscape `2752×2064` drawable while simctl captures a `2064×2752` portrait frame with letterbox bands. Both are true simultaneously:

```
scale     = 2064 / 2752 = 0.75          (fit landscape scene to portrait width)
content   = 2064 × (2064 × 0.75) = 2064 × 1548
bands     = (2752 − 1548) / 2 = 602 px top and bottom
```

Measured on the M5 baseline capture: **top band 602 px, bottom band 602 px, content 1548 px** — an exact match. The drawable is genuinely 2752×2064; the presented glass is that scene scaled ×0.75 and letterboxed by the OS. Handoff hypothesis 1 is confirmed; hypotheses 2–4 are ruled out (SDL/Metal/UIKit geometry all agree with each other — instrumenting them would have shown no divergence, because the divergence is in composition, not geometry).

## 3. Experiment matrix

All runs: fresh **iPad Pro 13-inch (M5)** simulator, **iOS 26.4**, chassis **portrait**, same `TiltBuggy.app` build (Xcode 26.6 / iphonesimulator 26.5 SDK, landscape-only plist, `AnyLandscape` + immersive). Plist variants were patched into a copy of the built app and ad-hoc re-signed — no rebuild, so the binary is byte-identical across rows.

| # | Variant | OS mode | Result |
|---|---------|---------|--------|
| 1 | As built (control) | Windowed Apps (default) | **Letterboxed.** 602/602 px bands, upright landscape content |
| 2 | + `UIRequiresFullScreen=YES` | Windowed Apps | **Letterboxed.** Pixel-identical to #1 except a home-indicator strip |
| 3 | + `UIDesignRequiresCompatibility=YES` | Windowed Apps | **Letterboxed.** Pixel-identical to #2 |
| 4 | As built (control) | **Full-Screen Apps** | **Full-bleed.** 0 px bands; content rotated within the portrait capture — the M4 / iOS 18.6 presentation class |

Notes on #4: the `AnyLandscape` lock rotated the *entire system UI* to landscape at launch (Settings still rendered landscape after the game quit). That is the 18.x-style launch rotation returning wholesale — knob 1 + knob 2 behave exactly as documented once the windowing system is out of the picture.

Captures (session scratchpad, not committed): `m5-baseline.png`, `testsim-control.png`, `testsim-rfs.png`, `testsim-design.png`, `testsim-fullscreenmode.png`.

## 4. Why there is no app-side fix (Apple's position)

- **`UIRequiresFullScreen`** is deprecated in iPadOS 26. Apple's Metal-on-iPadOS guidance is explicit: *"Because `UIRequiresFullScreen` is deprecated, you can no longer opt out of iPad multitasking and dynamic resizing."* Measured: no effect (#2).
- **`UIRequiresFullScreenIgnoredStartingWithVersion`** (TN3192, added 2026-02) governs the *opposite* direction — when older OSes stop honoring the key. Irrelevant here.
- **`UIDesignRequiresCompatibility`** opts out of the Liquid Glass *design*, not the windowing system. Measured: no effect (#3).
- **`prefersInterfaceOrientationLocked`** (TN3192) is only "freeze the currently presented orientation while this VC is visible." It is not "force landscape while the glass is portrait." An Apple DTS engineer pointed a developer with this exact problem at TN3192; the developer reported the property had no effect on presentation and concluded letterboxing was the only option. The system also explicitly does not guarantee the preference is honored.
- **TN3192's sanctioned migration** is: support **all** orientations, adopt resizable scenes, use Auto Layout / respond to `windowScene(_:didUpdateEffectiveGeometry:)`. In that world a fixed-aspect game letterboxes (or adapts) by design; the technote's own example is a game pausing when the orientation lock engages, not forcing one.

So the two-knob model in AGENTS.md gains a third clause on iPadOS 26+:

> Under **Windowed Apps** mode (the 26+ default), a scene whose orientation class mismatches the display orientation is **letterboxed by the system compositor**, and no packaging key or runtime API forces full-bleed. Knob 1 still sets the scene's orientation class; knob 2 still prevents mid-play swivel; neither can rotate the glass.

Why M4 differs: iOS 18.6 has no windowing system — the pre-26 full-screen model launch-rotates the interface, so scene and glass always agree.

Things this investigation confirms are **dead ends** (do not re-try): `UIRequiresFullScreen`, `UIDesignRequiresCompatibility`, additional swizzle targets, `requestGeometryUpdate` permutations, timing changes to the existing lock stack.

## 5. Recommendations

### 5.1 Unblock stream/direct parity now (test environment)

Set the M5 (and any 26.x oracle) simulator to **Settings → Multitasking & Gestures → Full-Screen Apps**. Chassis stays portrait — this is not the ruled-out "rotate the simulator"; it aligns the OS multitasking mode with the full-screen-game UX the M4 oracle assumes. With it, M5-direct matches the M4 reference class and stream-vs-direct comparison is meaningful again.

No public `defaults` key for the setting was found; it is a one-time manual toggle per simulator. Document it wherever 26.x sims are provisioned (smoke-test / matrix docs, `tools/spyder-pool.yaml` notes).

### 5.2 Product decision for real devices on iPadOS 26+ (default = Windowed)

Two options; they can be staged (ship a, file b as a target if the presentation is ever demanded):

a. **Accept the platform behavior** *(recommended)*. Letterboxed while held portrait; full-bleed once the user rotates to landscape. Every landscape-only game on iPadOS 26 behaves this way, so users will expect it. Optionally detect the mismatch (`UIWindowScene.effectiveGeometry`, or portrait framebuffer vs landscape scene) and show a "rotate your iPad" hint. Prior 🎯T154.3 notes already observed the letterbox resolving on rotation; one short test (Windowed mode + landscape chassis) would close that loop formally.

b. **Engine "self-rotation" mode.** Declare all four orientations so the scene always matches the glass; render the landscape world rotated 90° into the portrait scene; remap input, tilt, safe-area insets, and parallax accordingly. The only path to literal "portrait chassis, full glass" on 26+, and a substantial engine target — file only if the product owner insists on that presentation.

### 5.3 Documentation updates (ride the next PR that touches this area)

- AGENTS.md "iOS orientation lock (iPadOS 26+)": add the third clause from §4 (windowed-mode letterbox; no app opt-out on 26.4; Full-Screen Apps mode restores 18.x presentation and launch rotation).
- `sample/tiltbuggy/ios/Info.plist` and `tools/ios-template/Info.plist.in`: the trailing comment claiming `UIRequiresFullScreen` makes immersive take effect on iPad is stale on 26+ — the key is deprecated and measured inert.
- The banner in `tools/player_orientation_ios.mm`: note that on 26.4 the lock stack governs orientation *class and swivel*, not glass fill, under Windowed mode.

### 5.4 Watch items (file as targets)

- **Scene-based lifecycle**: TN3192 / Metal guidance say it becomes mandatory "in the next major release" when building with the latest SDK. SDL3's UIKit backend does not adopt it — upstream exposure for every ge iOS build at the iOS 27 SDK bump.
- **Launch screen**: required for App Store submission from iOS 27. TiltBuggy already ships `UILaunchScreen`; verify consumers do too.

## 6. Sources

- [TN3192: Migrating your iPad app from the deprecated UIRequiresFullScreen key](https://developer.apple.com/documentation/technotes/tn3192-Migrating-your-app-from-the-deprecated-UIRequiresFullScreen-key)
- [Managing your Metal app window in iPadOS](https://developer.apple.com/documentation/metal/managing-your-metal-app-window-in-ipados)
- [Forced Orientation + iOS 26 — Apple Developer Forums](https://developer.apple.com/forums/thread/802210)
- [UIRequiresFullScreen alternative for iPadOS 26+ — Apple Developer Forums](https://developer.apple.com/forums/thread/802069)
- [UIDesignRequiresCompatibility — Apple Developer Documentation](https://developer.apple.com/documentation/bundleresources/information-property-list/uidesignrequirescompatibility)
- [Turn Windowed Apps on or off on your iPad — Apple Support](https://support.apple.com/en-us/123635)
