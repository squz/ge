# Orientation, Rotation & Letterboxing — State of Play

*2026-07-19. Synthesized from session histories (mnemo, ge + spyder + yourworld2), git
archaeology of both repos, and a full code/docs sweep. Companion primary sources:
`tools/player_orientation_ios.mm` (banner), `docs/ios26-orientation-letterbox-findings.md`,
`docs/ios26-orientation-letterbox-handoff.md`, ge `AGENTS.md` § "iOS orientation lock".*

*Status of claims: everything in §1–§4 is sourced from recorded history or current code.
§6 lists the assertions selected for ground-up re-validation on simulators/emulators;
the results of that validation will be appended as §7.*

---

## 0. Executive summary

Five months of work (Feb–Jul 2026) turned orientation from a naive
"list all four in the plist and let UIKit rotate" posture into a mostly-understood,
two-knob doctrine with a runtime-authoritative lock on iOS, three purpose-packaged
stream players, and a definitively root-caused (but **unresolved by design**) iPadOS 26
windowed-mode letterbox. The big structural finding of this review:

1. **iOS is solved-with-asterisks.** The two-knob model (packaging plist + TN3192
   freeze swizzle) holds orientation reliably on iOS/iPadOS ≤18 and on 26 in
   Full-Screen-Apps mode. On iPadOS 26 in the default **Windowed Apps** mode, a
   fixed-orientation app on a portrait-held iPad is letterboxed *by the system
   compositor*, outside the process, with **no app-side escape hatch** — measured,
   arithmetic-proven, and confirmed by Apple guidance. The only in-engine answer
   (content self-rotation) exists as a declared-but-unimplemented API.
2. **Android is essentially unimplemented for direct mode.** `playerForceOrientation`
   is a no-op stub on Android, no `android:screenOrientation` is ever emitted, and
   `SessionHostConfig.orientation` is silently ignored — a landscape tilt game
   free-rotates on Android today. Everything we know about Android locking
   (manifest attribute, tablet system-letterboxing, SDL window-size ordering) came
   from the *player/stream* side and from TiltCal, and none of it has been folded
   into the direct-mode engine.
3. **The accelerometer/orientation coupling has one correct principle** — remap
   sensor frames by *live* display orientation, never by the requested lock — and it
   was arrived at twice (once as a self-correction in May, once in the S24 squish
   analysis). It is enforced on iOS direct mode; on Android it runs but interacts
   badly with the missing lock (dimension of the "tablet flip" UX problem).
4. **Several wire/API surfaces are dead or vestigial** (`AspectLock` defined and never
   dispatched; `SafeAreaUpdate` defined and unused — rotation re-sends full
   `DeviceInfo`; `playerContentRotationCwDegrees` declared, stub-only).
5. The end-state the project wants — *a game declares "I am a portrait game" or "I am
   a landscape game" and ge nails down every consequence* — is within reach: the
   knowledge exists, but it is spread across doctrine docs, three plists, a manifest
   template that doesn't participate, and two repos. §5 sketches the contract.

---

## 1. How we got here — timeline

### Phase 0 — naive rotation and engine-side hacks (Feb–Mar 2026)
- `c73e8a1` (Feb): first iOS port lists all four orientations in Info.plist; UIKit
  rotates the view. Works until a game cares.
- `6fc73ac` (Mar): `portraitLock` added to `RunConfig` — keep the surface at portrait
  dims, transform touches back. Engine-side workaround.
- `2d92e99` (Mar): `portraitLock` removed in favour of clip-space counter-rotation —
  a second engine-side attempt. Both later superseded; both are ancestors of the
  still-unbuilt "self-rotation" idea that iPadOS 26 has revived (§4.1).

### Phase 1 — the plist revert and the swizzle (Apr 12–13, 2026)
- **`e0da016`** ("revert Info.plist portrait-only experiment"): plist-only locking
  proven not to work on iPadOS 26 — the canonical failure commit. Its message
  already names counter-rotation as "the only viable approach" — prescient.
- **`5c2f2a5`**: the **TN3192 swizzle** — `prefersInterfaceOrientationLocked`
  swizzled to return YES on request; Apple's official replacement for
  `UIRequiresFullScreen`. Tested holding on iPadOS 26.4 sim. The winning mechanism.
- Also tried and abandoned in the same window: an SDL3 rotation-suppression patch
  (`599a8e5`/`708eeed`, added then reverted).

### Phase 2 — doctrine, Android manifest, accel frames (May 2, 2026 — `b912fe0`, v0.2.0/v0.3.0)
- The **two-knob model** written down with tripwires (plist warnings, CLAUDE.md
  section, the `player_orientation_ios.mm` banner): knob 1 = packaging plist narrows
  the *launch* set; knob 2 = swizzle freezes the *post-launch* swivel. Neither alone
  suffices on iPadOS 26+.
- `DirectRenderHost::send` → `playerForceOrientation` wired; lock baked into libge
  (`e561bb7`) so direct apps get it for free.
- **Accelerometer frame remap** (`rotateAccelToScreen`): initially keyed off
  `cfg.orientation`, self-corrected to **live `SDL_GetCurrentDisplayOrientation()`** —
  the request doesn't prove the OS rotated, and the no-lock case needs remap too.
- **Android**: established that orientation is a *manifest attribute*
  (`android:screenOrientation` — `fullSensor` / `sensorLandscape` / `landscape`), no
  swizzle equivalent; TiltCal set `fullSensor`. (This knowledge never became engine
  machinery — see §4.2.)
- Three-variant player bundles identified and deferred as 🎯T35 (seed of T154.3).

### Phase 3 — 🎯T36: the constants become authoritative (May 24, 2026 — `9c9b566`, v0.31.0)
- Trigger: the **multimaze2 incident** — a portrait tilt game shipped *landscape* on
  iPad because its plist allowed all four and the engine's boolean-style lock froze
  whatever UIKit picked at launch.
- Fix: a **second swizzle** on `supportedInterfaceOrientations` narrowing to the
  requested class; `kOrientationAnyLandscape` (0xFE) added for flip-the-device tilt
  games; loud `SPDLOG_WARN` on plist mismatch. The per-constant behaviour table in
  AGENTS.md dates from here.

### Phase 4 — the stream letterbox campaign (Jul 11–15, 2026)
- Jul 11: Android Pixel Tablet grind — a landscape-locked Activity is letterboxed *by
  the system* (`ignoreOrientationRequest` display-compat) inside the portrait
  display. Decision: for AnyLandscape **don't lock the Activity; rotate the texture**
  to fill the portrait window. Work explicitly parked by the user before re-grinding
  old ground.
- Jul 14: `b667bf8` — cmdstream letterbox fix: server ships its swapchain size in
  `FrameBegin`; player aspect-fits NDC into that rect (object-fit: contain) instead
  of stretching. 🎯T154.3 filed, then amended the same day to require
  orientation-variant *player packaging*.
- Jul 15: **`d32c1c4` — Player / PlayerLand / PlayerPort**: three iOS player
  products, same code, different plists/bundle IDs, because knob 1 is out-of-band
  packaging that cannot be set at attach time. Structural oracle
  `check-player-variants.sh`. Plus lock-before-window-create timing (`5034704`) and
  a 40×25 ms orientation settle loop in `player_core.cpp` so the server never
  renders landscape into a still-portrait glass.
- Jul 15 (later): **Samsung S24 squish** — SDL creates the window at the device's
  current *portrait* dims, orientation lock lands after, content scales into the
  wrong-aspect surface → doubled-aspect squish. Fix direction chosen
  (`player_orientation_android.cpp`, don't lock Activity, rotate texture) — **never
  verified full-bleed on the S24** (§4 loose end).

### Phase 5 — iPadOS 26 root cause (Jul 15, 2026 — session 949040b8)
Direct-mode TiltBuggy letterboxes on the M5 sim @ iPadOS 26.4 while full-bleeding on
M4 @ iOS 18.6, same binary, correct landscape drawable (2752×2064) in the logs.
Root cause, measured and arithmetic-proven:

> iPadOS 26's default multitasking mode is **"Windowed Apps"**. The OS no longer
> rotates a fixed-orientation app's interface into a supported orientation at
> launch; the app gets a true full-resolution landscape scene and the **system
> compositor scales that scene into the mismatched portrait glass**. The 602 px
> bands are exactly a 2752×2064 scene scaled ×0.75 into 2064-wide glass. Nothing in
> ge's stack malfunctions; **no swizzle/geometry work can fix it** — the letterbox
> happens outside the app.

- `UIRequiresFullScreen=YES`: measured pixel-identical no-op on 26.4.
- `UIDesignRequiresCompatibility=YES`: no-op (opts out of Liquid Glass *design* only).
- OS Settings → Multitasking & Gestures → **Full-Screen Apps** fully restores the
  iOS 18 behaviour (no known `defaults` key to script it).
- Apple's position (TN3192 + DTS forum): live with it. The only literal
  "portrait chassis, full glass" path is engine **self-rotation** (render the
  landscape world rotated 90° into a portrait scene; remap input/tilt/safe-area) —
  assessed as a substantial target, filed nowhere, awaiting a product decision.

### Phase 6 — sensor authority and transport-transparency (Jul 17–18, 2026 — 🎯T156)
"Stream is a transport, not a modality." One sensor authority per virtual device,
decided by **declared hardware capability** (`kCapHasAccelerometer`), never by
build flag or stream-vs-direct branch. The player is pure glass: it opens a real
accelerometer if present, remaps samples hardware→screen frame via the **live**
display orientation, forwards raw; AccelSynth (Shift-mouse synthetic tilt) exists
only server-side where no hardware accel exists. DeviceInfo/content-surface
authority follows the seat (T156.3).

---

## 2. The model — how orientation actually works, per platform

### 2.1 iOS: the two-knob law
| Knob | Mechanism | Sets | Fails alone because |
|---|---|---|---|
| 1 — packaging | `Info.plist UISupportedInterfaceOrientations` | the *launch* orientation set | iPadOS 26 treats every iPad app as resizable; swivel re-rotates mid-play; on 26 the launch set is not even binding for window shape |
| 2 — runtime | `playerForceOrientation()` → dual swizzle: `supportedInterfaceOrientations` (narrow) + `prefersInterfaceOrientationLocked` (freeze, TN3192) | post-launch rotation | freezes *whatever iOS picked at launch* — wrong class if the plist wasn't narrowed |

Operational order (the part that took longest to learn): set the narrowed mask with
freeze **off** → `setNeedsUpdate…` + `requestGeometryUpdate` so UIKit rotates *into*
the class → only once the interface matches, freeze **on**. Freezing early freezes
portrait and blocks geometry updates — the exact iPadOS 26 failure mode. Both
`UIViewController` **and** `SDL_uikitviewcontroller` must be swizzled (SDL overrides
without calling super). Retry cadence 0/0.05/0.20 s, freeze-arm at 0.50 s,
force-freeze on timeout.

Per-constant semantics (v0.31.0+): `kOrientationPortrait` / `PortraitFlipped` /
`Landscape` / `LandscapeFlipped` lock the named class; `kOrientationAnyLandscape`
(0xFE) locks at launch to whichever landscape iOS picks, user can't rotate mid-play.

### 2.2 iPadOS 26 windowed mode: the hard boundary
Under the 26.x default "Windowed Apps" mode, orientation *holding* still works, but
**glass shape does not follow the app**: a landscape-only app on a portrait-held
iPad renders a genuine landscape scene that the compositor letterboxes into portrait
glass. Rotating the physical device to landscape gives full bleed. There is no
plist key, swizzle, or geometry request that overrides this. The decision space:
1. **Accept** (recommended in the findings doc): portrait-held = letterboxed, rotate
   for full bleed — this is now how every landscape-only game behaves on 26.
   Optional "rotate your iPad" hint driven by `UIWindowScene.effectiveGeometry`.
2. **Self-rotate**: declare all four orientations, render the landscape world
   rotated 90° into the portrait scene, remap touch/tilt/safe-area/parallax. The
   API surface for this exists but is unimplemented (§4.1).

### 2.3 Android: manifest attribute + system letterboxing
- Locking = `android:screenOrientation` on the `<activity>` — packaging metadata,
  the moral equivalent of knob 1. There is no knob-2 equivalent and none is needed
  for *holding*; `sensorLandscape` gives exactly the AnyLandscape semantics
  (both landscapes, live flip, no portrait) **with** OS-driven 180° flips.
- **But**: on tablets/foldables with `ignoreOrientationRequest` display-compat
  (Pixel Tablet observed), the *system* letterboxes a landscape-locked Activity
  inside the portrait display — black bars the app cannot remove. Same shape as the
  iPadOS 26 problem: the OS refuses to let packaging dictate glass shape.
- Chosen direction (player-side, Jul 11/15): don't lock the Activity for
  AnyLandscape; keep the surface at the OS's shape and **rotate the content
  texture** to fill it. Also: SDL applies orientation *after* creating the window at
  current-device dims — lock (or hint) must land **before** window creation or the
  surface is sized portrait and content squishes (S24).
- **ge direct mode implements none of this today** (§4.2).

### 2.4 Accelerometer/tilt coupling
- One law: sensor samples are remapped hardware-frame → screen-frame by the **live**
  display orientation (`rotateAccelToScreen(SDL_GetCurrentDisplayOrientation(), …)`),
  never by the requested lock. Touch/mouse are *not* remapped (OS delivers them in
  UI frame).
- Parallax (T9) sources: iOS `CMMotionManager.deviceMotion.attitude`; Android
  `TYPE_GAME_ROTATION_VECTOR` via `GeActivity.getAttitude()`; desktop/web stub.
- Sensor authority (T156.2): per virtual device, by declared capability. Glass with
  a real accelerometer forwards raw samples (already screen-framed); AccelSynth only
  where no hardware accel exists; the player never runs a tilt model.

### 2.5 Stream modality
- Knob 1 for the stream is the **player packaging**: Player (don't-care) /
  PlayerLand / PlayerPort — same code, different plists/bundle IDs — because the
  launch-window aspect fixes the UIKit orientation mask *before any wire message
  arrives*. Knob 2 rides the wire: `SessionConfig.orientation` →
  `playerForceOrientation` on the glass, applied in three overlapping places plus a
  settle loop to beat OS timing.
- Content fill: server resizes its hidden window to the viewer's aspect
  (`DirectRenderHost` aspect-retarget) and the player aspect-fits
  (object-fit: contain) — cmdstream via `FrameBegin` swapchain dims, H.264 via
  rotated/centered min-scale blit. `wire::AspectLock` plays **no part** (dead slot).
- Rotation on the glass re-sends full `DeviceInfo` (not `SafeAreaUpdate`, which is
  defined but unused).

### 2.6 Letterboxing mechanics — who letterboxes, where
| Layer | When | Controllable? |
|---|---|---|
| ge content projection (`ortho::letterbox`, arena rebuild to drawSafe aspect) | content aspect ≠ surface aspect | yes — game/engine choice; tiltbuggy *reshapes the arena* instead of boxing |
| Player fit (contain) | stream content aspect ≠ glass aspect | yes — could fill/crop instead; currently contain |
| **iPadOS 26 compositor** | fixed-orientation app, mismatched glass, Windowed mode | **no** |
| **Android display-compat** | orientation-locked Activity on `ignoreOrientationRequest` device | **no** (only by not locking the Activity) |

The pattern across both OSes: *the OS increasingly refuses to reshape the glass to
the app; apps that insist on a shape get boxed.* Every durable mitigation we found
moves the adaptation **into content space** (reshape the arena, rotate the texture,
accept-and-hint) rather than fighting the window system.

---

## 3. Where things stand — the five dimensions

**(1) Android vs iOS.** iOS: full runtime lock stack, authoritative constants,
accel remap enforced. Android: rotation *handling* good (configChanges, cutouts,
attitude, immersive) but **zero lock support in direct mode** — no manifest
attribute emitted, stub `playerForceOrientation`, `SessionHostConfig.orientation`
silently ignored. The asymmetry is the single biggest implementation gap.

**(2) OS versions.** iOS ≤15: legacy `setNeedsUpdate…` fallback. iOS 16+:
`requestGeometryUpdate`. iPadOS 26: TN3192 `prefersInterfaceOrientationLocked` is
mandatory; `UIRequiresFullScreen` inert; Windowed-mode compositor letterbox is new
and unfixable app-side. Watch items: scene-based lifecycle becomes mandatory "next
major release" with the latest SDK (SDL3's UIKit backend doesn't adopt it); iOS 27
launch-screen requirement. Android: behaviour varies by *device class/OEM*
(`ignoreOrientationRequest` on tablets, OEM rotation-lock defaults) more than API
level.

**(3) iPadOS windowed modality.** Root-caused, proven, no app-side fix. Product
decision (accept vs self-rotate) **open**. Sim workaround: manual Full-Screen-Apps
toggle (no scriptable key found).

**(4) Accelerometer apps and the one-time lock.** Portrait games: trivial —
portrait plist + `kOrientationPortrait`, done. Landscape tablet games: lock
achieved via AnyLandscape, but two UX defects remain: (a) on iPadOS 26 windowed,
portrait-held = letterboxed until rotation; (b) **the 180° landscape flip** — with
the interface frozen at the launch landscape, physically flipping the tablet leaves
content upside-down and tilt inverted; allowing the OS to rotate mid-play instead
causes a disruptive animation and coordinate flip during gameplay. Neither current
behaviour is right; the missing piece is a deliberate policy (e.g. permit
landscape↔landscape remap only at quiescent moments, atomically, with the accel
frame flipped in the same frame — this is a design item for §5, not yet built).
Note Android's `sensorLandscape` gives OS-driven flips for free; iOS's freeze
forbids them — so today the two platforms would *feel different* even if Android
locking existed.

**(5) Android letterboxing.** Unavoidable *if the Activity insists on a shape* on
display-compat devices; avoidable by not insisting — free-rotate Activity +
content-side rotation/reshape. The chosen direction (rotate texture into the
portrait window) exists for the stream player; direct mode has nothing. Minimizing
impairment = content-space adaptation + safe-rect-aware arena reshaping (tiltbuggy
pattern), not window-space fighting.

---

## 4. Loose ends (consolidated)

### 4.1 Decisions pending
1. **iPadOS 26 windowed letterbox**: accept (+optional rotate-hint) vs implement
   self-rotation. `playerContentRotationCwDegrees()` /
   `playerApplyLandscapeOnPortraitChassis()` are declared and documented in
   `tools/player_orientation.h` but implemented only as stubs, called nowhere.
2. **AnyLandscape 180°-flip policy** (dimension 4b above): no design exists.
3. **`AspectLock`**: dead wire slot — fold into SessionConfig (the T36-era
   proposal), implement, or delete.

### 4.2 Implementation gaps
4. **Android direct-mode orientation lock**: nothing exists — no manifest attr
   emission from `init-android.sh`/template, no runtime path, no target tracking it
   (T154.3 covers player packaging only). A locked game free-rotates on Android.
5. **🎯T120**: `SDL_HINT_ORIENTATIONS` is only set when `orientation != 0`; an
   unlocked direct iOS app lets SDL infer the mask from window w/h — `{1024,768}`
   silently locks landscape on iPad.
6. **T36's recommended startup warning** (SessionHostConfig.orientation vs
   plist/launch mismatch — the multimaze2 silent failure) was never built. The
   `.mm` warns, but only iOS-side at force time.
7. **S24 doubled-aspect squish**: fix direction chosen, **never verified**.
8. Wire-mode parallax/attitude streaming and safe-area push: deferred (T9/T37
   follow-ups).
9. yourworld2 (sibling repo): landscape round-trip rotation bug
   (stale-surface-dims in `safeAreaToPortrait`); DeviceInfo orientation never
   stored in State until first rotation — both unconfirmed-fixed.

### 4.3 Hygiene / doc rot
10. AGENTS.md still documents `tools/ios` + three players in-tree; `tools/ios/` is
    empty (players live in spyder). AGENTS.md also lacks the "windowed-mode
    letterbox, no app opt-out" clause from the findings doc.
11. Stale `UIRequiresFullScreen` comments in `tools/ios-template/Info.plist.in`
    and tiltbuggy's plist (inert on 26).
12. `docs/ios26-orientation-letterbox-findings.md` commit status was left
    uncertain during another agent's WIP (now present in-tree — verify committed).
13. `src/Attitude_android.cpp:7` references the removed per-app Activity template.
14. iOS packaging templates hardcode landscape-only; a portrait consumer
    hand-edits with no cross-check (see gap 6).
15. `SafeAreaUpdate` defined but unused (rotation re-sends DeviceInfo) — dead
    surface to remove or adopt.
16. **Possible constant-mapping skew**: ge maps `kOrientationLandscape` →
    `UIInterfaceOrientationMaskLandscapeRight` (documented SDL↔UIKit name swap),
    while the spyder player reportedly maps the same constant to SDL hint
    `"LandscapeLeft"`. May be two correct spellings of the same physical
    orientation across naming systems, or a real 180° disagreement — flagged for
    empirical validation (V10).

### 4.4 Knowledge gaps
17. Whether the iPadOS 26 windowed letterbox reproduces identically on **physical**
    hardware @ 26.x (all evidence is simulator).
18. Simulator/emulator fidelity in this domain generally — sim host-orientation
    prefs, simctl screenshot compositing, and emulator display-compat behaviour
    all diverged from expectations at least once. §6 exists to bound this.

---

## 5. The destination: a declarative orientation contract

What the end state should look like (discussion basis, not yet a target):

```cpp
ge::run(factory, {
    .orientation = ge::OrientationClass::Landscape,  // or Portrait, FreeRotate
    ...
});
```

One declaration; ge derives **everything**, per platform:

| Consequence | Portrait game | Landscape game |
|---|---|---|
| iOS plist (`UISupportedInterfaceOrientations`) | Portrait (+flipped, tablet-only?) | LandscapeL+R | 
| iOS runtime | narrow+freeze per two-knob law | same, AnyLandscape semantics |
| iPadOS 26 windowed | native shape — no issue | accept-letterbox + rotate-hint, or self-rotate (policy flag) |
| Android manifest | `screenOrientation="portrait"` (or `sensorPortrait`) | `sensorLandscape` — but only on non-display-compat devices; else free-rotate + content rotation |
| Android surface timing | hint/lock before window create | same |
| 180° flip policy | n/a (or sensor flips) | quiescent-moment atomic remap incl. accel frame |
| Accel/tilt remap | live-orientation law (already enforced) | same |
| Letterbox mitigation | content-space (safe-rect arena reshape) | same |
| Stream packaging | PlayerPort | PlayerLand |
| Build-time check | plist/manifest ↔ declaration cross-check, loud failure | same |

The packaging halves (plist, manifest) must be **generated or verified by the build**
(the `ge/app-icons` / `wire-icons.py` pattern: idempotent patch + refuse-on-drift),
because knob 1 cannot be set at runtime and silent mismatch is exactly the
multimaze2 failure. The doctrine already exists in prose; the contract turns it
into machinery.

---

## 6. Assertions selected for ground-up validation (phase 2)

Each will be tested with bespoke minimal apps on simulators/emulators; results to be
appended as §7 with pass/fail/notes per OS version.

| # | Assertion (as currently believed) | Method |
|---|---|---|
| V1 | iPadOS 26 sim, Windowed mode: landscape-only-plist app on portrait chassis is compositor-letterboxed; app scene is true landscape | minimal UIKit app, edge-painting + `effectiveGeometry` logging; simctl screenshot band arithmetic |
| V2 | Same app, Full-Screen-Apps mode: full-bleed, iOS-18-like | toggle + rerun |
| V3 | iOS 18 sim: plist-only landscape lock rotates interface at launch, full-bleed | same app on 18.x runtime |
| V4 | Plist-only lock on iPadOS 26 does not survive swivel (knob-1-alone failure) | rotate sim, observe |
| V5 | Swizzle-alone (all-four plist + TN3192 freeze) freezes the launch orientation, including the wrong one | swizzle variant of test app |
| V6 | `UIRequiresFullScreen` and `UIDesignRequiresCompatibility` are inert on 26.4 | plist variants, pixel-compare |
| V7 | iPhone (sim): portrait lock is trivial and total; upside-down excluded by default on notch devices | portrait variant on iPhone sim |
| V8 | Android emu: `sensorLandscape` on a phone AVD = both landscapes, live 180° flip, no portrait, no letterbox | minimal Android app logging Display rotation + WindowMetrics |
| V9 | Android tablet/foldable AVD (or resizable AVD with display-compat): landscape-locked Activity is system-letterboxed | same app, tablet AVD, `ignoreOrientationRequest` toggles via `wm` / `adb shell cmd window` |
| V10 | `kOrientationLandscape` maps to the same physical orientation across ge mask, SDL hint, and (if implemented) Android — no 180° skew | instrumented ge test app on sim + code trace |
| V11 | SDL window-size→orientation inference (T120 footgun): unlocked SDL iOS app with landscape default size locks landscape on iPad | minimal SDL/ge app, no hint, `{1024,768}` |
| V12 | Accel remap by live orientation: flipping a `sensorLandscape` Android app 180° flips reported gravity sign atomically with the rotation | sensor-logging app on emu (rotate via console) |
| V13 | Sim host-orientation prefs don't affect the windowed-mode letterbox (the "M4 magic" is the OS version, not host state) | M4-style 18.x vs 26.x runtimes, same host |

---

## 7. Empirical results (2026-07-19, `research/orientprobe/`)

Probes: pure-UIKit and pure-android.view apps with edge markers + live metrics
(no ge/SDL in the loop, so results measure the OS). Devices: iPad Pro 13" (M4)
sim @ iOS 18.6, iPad Pro 13" (M5) sim @ iPadOS 26.4, iPhone 17 sim @ 26.4,
Pixel 9 Pro XL emulator @ Android 16 (API 36), physical Pixel Tablet (API 36)
and Galaxy Tab A9 (API 36) for hardware cross-checks.

### 7.1 Verdicts

| # | Assertion | Verdict |
|---|---|---|
| V1 | 26.4 windowed: landscape-plist app on portrait chassis compositor-letterboxed, true landscape scene | **CONFIRMED** — scene 1376×1032 pt upright-scaled into portrait glass; band arithmetic matches (602 px) |
| V2 | Full-Screen-Apps mode restores 18.x behavior | **CONFIRMED** — same app/chassis full-bleed rotated after the Settings toggle; pixel-equivalent to M4@18.6 |
| V3 | 18.6: plist-only landscape rotates at launch, full-bleed | **CONFIRMED** |
| V4 | Rotation cures / re-causes the 26.4 letterbox live | **CONFIRMED** both directions. **Refinement**: the landscape-only plist *holds* the interface class through swivel on 26.4 (ifaceOri stays 3) — the April-era claim "iPadOS 26 ignores plist restrictions" is wrong as stated; what the plist can't control is *glass shape* |
| V5 | TN3192 freeze holds launch orientation | **CONFIRMED** — all-four plist + freeze: interface pinned portrait through rotation, `effectiveGeometry.isInterfaceOrientationLocked=true`; scene stays panel-aligned so **no letterbox ever** for portrait-frozen-on-portrait-panel |
| **V5b** | *(new)* freeze + chassis-mismatched scene | **NEW FINDING — the freeze PINS the letterbox.** Landscape plist + freeze launched portrait: letterboxed; rotating to landscape **no longer cures it** (V4's cure disappears). The freeze pins the compositor mapping, not just the interface class. ⇒ ge's `playerForceOrientation` **force-freeze-on-timeout is a live bug** on 26 windowed: a landscape game launched portrait-held letterboxes *permanently* |
| V6 | `UIRequiresFullScreen`, `UIDesignRequiresCompatibility` inert on 26.4 | **CONFIRMED** — both letterbox identically to control |
| V7 | iPhone portrait lock trivial | **CONFIRMED** — portrait plist alone: full-bleed, never rotates, no letterbox concept |
| V8 | Android `sensorLandscape` on phone | **CONFIRMED** — display rotates to landscape full-bleed from portrait hold; across a full rotation cycle stays in {rot 1, rot 3}, holds last landscape through portrait |
| V9 | `ignoreOrientationRequest` letterbox | **CONFIRMED** (phone-class emu, flag on): landscape-locked activity = 1344×1008 card centered in 1344×2992 display (~66 % black); `win != display` is a clean programmatic detector; physical rotation to landscape cures it (full-bleed) |
| **V9b** | *(new)* Android 16 large-screen + targetSdk 36 | **NEW FINDING — locks are DISCARDED by resize, not letterboxed.** Physical Pixel Tablet, forced portrait rotation: `landscape` *and* `sensorLandscape` activities get a **full-display portrait window** (1600×2560, rot=0) — request ignored entirely, app must render portrait. ge's Android template targets **SDK 35** (still letterbox-era); the targetSdk 36 bump Play will eventually force flips tablet behavior from "boxed" to "handed a portrait surface" |
| V10 | Constant-mapping skew | **CONFIRMED REAL** — ge `SDL_ORIENTATION_LANDSCAPE → MaskLandscapeRight` (correct SDL↔UIKit swap) vs spyder player `kOrientationLandscape → hint "LandscapeLeft"` (opposite). Latent today (swizzle overrides the hint), but the two paths contradict |
| V11 | SDL window-size mask inference (T120) | **NOT RUN** — needs a ge/SDL build; follow-up |
| V12 | Accel frame vs rotation | **CONFIRMED** — samples always device-hardware-frame; gravity sign flips exactly with the 180° display flip; live-rotation remap (`rotateAccelToScreen`) is the correct and sufficient policy |
| V13 | Host-pref vs OS-version | **CONFIRMED implicitly** — same host, same binary: 18.6 vs 26.4 differ; the Settings mode toggle alone flips 26.4's behavior |
| — | Real-tablet defaults | Pixel Tablet **and** Tab A9 ship `ignoreOrientationRequest=true` — the letterboxing/discarding posture is the tablet factory default, not an emulator artifact |

Also observed: simctl screenshots are always panel-native (2064×2752) with the
compositor transform baked in — band-measuring works; Android `screencap`
follows the rotated framebuffer (2992×1344), so `WindowMetrics` is the better
Android instrument.

### 7.2 The observed OS algorithm (pseudo-code model)

```text
# ---------- iOS / iPadOS: scene class ----------
allowed = plist.UISupportedInterfaceOrientations ∩ runtime supportedInterfaceOrientations
at launch:
    scene.class = member of `allowed` (prefers current device orientation if allowed;
                  otherwise an OS-preferred member — glass is NOT rotated to match on iPad 26 windowed)
on device rotation:
    if newClass ∈ allowed and not frozen: scene rotates (mid-play rotation)
    else: scene class unchanged

# ---------- iOS / iPadOS: glass mapping (per frame) ----------
if iPhone, or iPad in Full-Screen-Apps mode, or iPad ≤ 18:
    glass = panel driven in scene.class            # full-bleed always (V3, V5, V7)
elif iPad 26 Windowed-Apps:
    if not frozen (prefersInterfaceOrientationLocked == false):
        glass = scene scaled into CURRENT chassis-held class   # letterbox iff mismatch;
                                                               # live-updates with device (V1, V4)
    else:  # TN3192 freeze armed
        glass = scene scaled into chassis class AS OF FREEZE TIME   # PINNED (V5b):
                                                                    # rotation no longer re-maps
# UIRequiresFullScreen / UIDesignRequiresCompatibility: no effect anywhere (V6)

# ---------- Android: window for an activity requesting orientation R ----------
if device honors R (phone; or tablet flat with no gravity vote; or ignoreOrientationRequest=false):
    display rotates to R; window = full display            # sensor* variants track the device
                                                           # within R's class, incl. 180° flips (V8)
elif ignoreOrientationRequest and targetSdk ≤ 35:
    window = R-aspect box centered in current display      # compat letterbox (V9);
                                                           # cures when device physically enters R
else:  # ignoreOrientationRequest and targetSdk ≥ 36 (Android 16 large screens)
    R is DISCARDED; window = full display in user/sensor rotation   # app handed any aspect (V9b)

# ---------- accelerometer (both platforms) ----------
samples are ALWAYS device-hardware-frame (V12)
screen_frame_sample = rotate(sample, live display rotation)   # never key on the requested lock
```

### 7.3 Consequences for the ge design

1. **Freeze policy must be chassis-gated.** Never arm TN3192 while the device
   orientation class disagrees with the scene class; drop the
   freeze-on-timeout fallback. Corollary: deferring the freeze until the
   device first settles in the target class *is* the "accept whichever
   landscape the player is already holding, then lock it" behavior — one
   policy fixes the pinned-letterbox bug and delivers accept-current-landscape.
2. **Portrait games are done.** Portrait plist (+`kOrientationPortrait`) is
   sufficient on every OS tested; the freeze adds nothing but risk on iPad
   (a portrait scene on a portrait panel never letterboxes, frozen or not).
3. **Landscape games cannot force glass shape anywhere anymore.** All three
   OSes (iPadOS 26 windowed, Android ≤15-style compat, Android 16 resize)
   refuse; the durable posture is: allow both landscapes, follow the OS's
   mapping, adapt in content space, and hint the player to rotate. On
   Android 16 tablets the game will be handed a genuine portrait surface —
   content-side rotation (the dormant `playerApplyLandscapeOnPortraitChassis`
   idea) stops being optional there.
4. **Fix the V10 hint/mask skew** (spyder `kOrientationLandscape` hint string)
   before it stops being latent.
5. **Keep accel remap keyed on live rotation** — validated as exactly right,
   including atomic 180° flips.

## 8. Round-2 addenda (2026-07-19, follow-up questions)

**8.1 Two-knob hard data (V7b).** All-four plist + *static* runtime
`supportedInterfaceOrientations = .landscape`, no kick, no freeze: on **both**
18.6 and 26.4 the scene launched **portrait** (26.4: ifaceOri=1 full-bleed
portrait; 18.6: ifaceOri=2 — the mask was not even consulted at launch).
Conclusion, now measured: **the plist (or an explicit
`setNeedsUpdate…`/`requestGeometryUpdate` kick) governs the launch class; a
static runtime mask alone governs nothing until UIKit re-evaluates.** So the
knobs are really three: plist = launch class; mask + kick = post-launch class
change; freeze = hold. Metadata-only, code-only, and mask-without-kick all
fail in distinct ways. This also explains the historical "you can pin the
current orientation but not choose one" era: pre-T36 ge froze without
narrowing+kicking, which is exactly the pin-only behavior.

**8.2 Safe areas under letterbox (new dimension).** In the 26.4
compositor-letterboxed state the app reports the **same** safe insets as
full-bleed (t≈32/b≈25 pt) — while the status bar and home indicator visibly
sit in the black bands *outside* the scene. Under compositor letterbox the
insets are scene-space fictions: nothing overlaps the content, and a game
insetting its HUD by them double-margins. Same holds for Android's compat
card (chrome outside the box). Consequence for ge: `drawSafe`/`uiSafe` rects
are only meaningful when the scene owns the glass; under letterbox the honest
values are zero (or "unknown"), and the engine should detect boxed state
(iOS: chassis-vs-scene class mismatch in windowed mode; Android:
`WindowMetrics current != maximum`) and adjust what it reports to games.

**8.3 Launch-state dependence** is now fully modeled: launch class = plist ∩
(device orientation if allowed, else OS pick); glass never rotates to match
the scene on iPad-26-windowed / Android-compat — the *device* must move.
Landscape-from-portrait is achievable for the scene (plist narrowing), but
never for the glass. The old counter-rotation dead end is unnecessary on iOS;
its only justified successor is content-rotation on Android-16 tablets where
the OS hands the app a portrait surface outright.

**8.4 "Prefer landscape, accept portrait" (new class).** No OS expresses
soft preference natively (`userLandscape` etc. are still hard locks). It has
to be an app/engine behavior: declare *all* orientations to the OS, render
adaptively per current glass aspect. On Android-16 tablets this adaptive mode
is effectively forced. Candidate contract: `OrientationClass::{Portrait,
Landscape, PreferLandscape, FreeRotate}` where PreferLandscape = follow the
glass, adapt content, hint to rotate when portrait.

**8.5 Policy oracle idea.** The §7.2 pseudo-code is implementable as a pure
`ge::orientation::Policy` module: inputs (platform, OS mode, declared class,
plist/manifest set, device orientation, scene class, freeze state) → outputs
(expected scene class, expected glass mapping incl. boxed-or-not, actions:
kick / arm-freeze / release-freeze, safe-inset trust). One implementation
drives iOS (`playerForceOrientation` becomes a thin actuator), Android
(`setRequestedOrientation` policy), *and* tests: the probe apps (or the
engine's own metrics) report observed state, the oracle predicts it,
divergence = OS drift or bug. Single home for all orientation logic; the
doctrine becomes executable.

**8.7 External verification (web sweep, 2026-07-19).** Official docs/forums
confirm F1 (windowed-mode letterbox, no app opt-out; TN3192 + DevForums
802210), F2 (TN3192 lock is a *preference* that freezes current orientation —
consistent with the pinning we measured; it is not an imperative
force-to-orientation), F3 (plist still constrains the scene, trending to
"preference"), F4 (Android 16 fill-not-letterbox on sw≥600dp for sdk 36;
generic opt-out `PROPERTY_COMPAT_ALLOW_RESTRICTED_RESIZABILITY` **sunsets at
API 37**), F6 (sensorLandscape 180° flips; sensor frame never rotates —
`remapCoordinateSystem` is the app's job), F7 (iPhone unaffected), F9 (SDL
source: VC doesn't call super; hint-unset mask inferred from window w/h —
T120's footgun is confirmed from source), F10 (iOS 27). Not externally
sourced: F5's flat-device mechanism (documented concept is the accelerometer
"Z-lockout" holding the *last* orientation) and F8's double-margin insets
(novel measurement; possibly entangled with live iOS 26 safe-area regression
FB20281983 — stale `safeAreaInsets` until backgrounding).

New load-bearing facts the sweep surfaced:
1. **`android:appCategory="game"` permanently exempts games** from the
   Android 16/17 orientation-ignore (does not sunset at API 37). **ge sets it
   nowhere** — adding it to the manifest template preserves tablet
   orientation locks durably and demotes §7.3's "adaptive is forced" to
   "adaptive is the quality path; the lock remains available".
2. **iOS 27 SDK (~June 2026) hard-mandates UIScene adoption** (app won't
   launch) **and a launch screen** (App Store rejects). **Follow-up sweep:
   already de-risked.** SDL fixed scene adoption for the 3.4.0 milestone
   (issue libsdl-org/SDL#12680, closed): `SDL_uikitappdelegate.m` now
   implements `application:configurationForConnectingSceneSession:` +
   `SDLUIKitSceneDelegate` (programmatic adoption — satisfies the mandate
   without an Info.plist `UIApplicationSceneManifest`). ge vendors exactly
   SDL `release-3.4.0` and the code is present in the tree; `UILaunchScreen`
   is already declared in `tools/ios-template/Info.plist.in` and all sample
   plists (TN3208 satisfied). Residual action: one smoke test at first
   Xcode-27 build; fallback if it ever asserts is a 10-line
   `UIApplicationSceneManifest` block in the plist template. Keep SDL pinned
   ≥ 3.4.0. (Stale SDL Discourse threads claiming SDL3 lacks UIScene predate
   the 3.4.0 fix.)
3. iOS 27 partially rehabilitates `UIRequiresFullScreen` on iPhone as
   "discrete resizing" (full-quality re-render per size step).
4. `UIWindowScene.effectiveGeometry.isInterfaceOrientationLocked` +
   `didUpdateEffectiveGeometry` are the modern lock-state read-back — cleaner
   than notification inference for the policy oracle's observation side.
5. iOS 26 safe-area regression (FB20281983): re-query insets on foreground as
   a workaround; directly relevant to ge's `SafeAreaInsets` plumbing.

**8.6 Reconciling "iPad/Pixel honor orientation now, no letterbox".**
Both letterboxes are *posture-gated*, so casual observation usually misses
them: the box appears only while the chassis is actively held in the
mismatched class (iPad 26 windowed portrait-held; Pixel Tablet held portrait
with sensor vote). Flat-on-desk, held-landscape, or rotated-after-launch all
present full-bleed. The July-11 "stuck with letterboxing" era additionally
conflated three distinct boxes: (1) the OS compositor/compat box (posture-
gated, cures on rotation — unless frozen, §7.1 V5b), (2) the stream player's
content-fit box (aspect mismatch between server surface and glass — fixed by
`b667bf8` + host aspect retarget), and (3) the SDL portrait-first window
sizing squish (S24). Fixing (2)/(3) plus posture changes is why the fleet
looks healthy today without any Pixel config change. OS updates likely also
shifted the Pixel Tablet toward Android-16 resize-not-letterbox semantics
for sdk-36 apps (ge apps target 35, still compat-boxed when held wrong).

