// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Manages the server lifecycle: sokol context, optional stream-relay connection,
// H.264 encode pipeline, and per-session state. The game provides a
// factory that creates session state and returns render loop callbacks.
#pragma once

#include <sqlpipe.h>

#include <ge/Linalg.h>
#include <ge/Pass.h>
#include <SDL3/SDL_events.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ge {

enum class DeviceClass : uint8_t {
    Unknown = 0,
    Phone   = 1,
    Tablet  = 2,
    Desktop = 3,
};

// OS memory-pressure level forwarded to RunConfig::onMemoryWarning.
// iOS sends a single warning (mapped to Critical); Android grades into
// five buckets which the engine collapses to three:
//
//   Low      ↔ TRIM_MEMORY_RUNNING_MODERATE
//   Moderate ↔ TRIM_MEMORY_RUNNING_LOW + TRIM_MEMORY_BACKGROUND
//   Critical ↔ TRIM_MEMORY_RUNNING_CRITICAL + TRIM_MEMORY_COMPLETE +
//              UIApplicationDidReceiveMemoryWarningNotification
enum class MemoryPressureLevel : uint8_t {
    Low      = 0,
    Moderate = 1,
    Critical = 2,
};

// Pixel rectangle on the render surface ({x, y} = top-left, {w, h} = size).
//
// Float fields, even though the surface is integer-sized at the
// framebuffer boundary. Game logic — layout math, animation,
// transforms, physics — is float-native, and forcing apps to cast
// every read/write back to their float pipeline costs more than the
// engine pays to convert once at the SDL/sokol boundary. The integer
// values are preserved exactly (a 1080-pixel surface stores 1080.0f).
//
// Corner accessors return la::float2 (linalg's vec<float,2>,
// re-exported in ge::la — see ge/Linalg.h) so they compose with the
// rest of the engine's vector math without an intermediate copy.
struct SafeAreaInsets;

struct Rect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    // Constructors. The 4-arg form takes `{x, y, w, h}` directly.
    // The two tagged forms disambiguate via designated init at the
    // call site:
    //   Rect r1{1, 2, 3, 4};                              // 4 floats
    //   Rect r2{{.origin = {1, 2}, .size   = {3, 4}}};    // OriginSize
    //   Rect r3{{.a      = {1, 2}, .b      = {5, 6}}};    // Corners (sign-preserving)
    // Without designated names, `Rect{{1,2}, {3,4}}` is ambiguous —
    // the compiler forces the user to name the field whose meaning
    // matters. Brace-eliding the inner struct literal is fine when
    // the field names are present.
    struct Corners    { la::float2 a, b; };
    struct OriginSize { la::float2 origin, size; };

    constexpr Rect() = default;
    constexpr Rect(float x_, float y_, float w_, float h_)
        : x(x_), y(y_), w(w_), h(h_) {}
    constexpr Rect(Corners c)
        : x(c.a.x), y(c.a.y),
          w(c.b.x - c.a.x), h(c.b.y - c.a.y) {}
    constexpr Rect(OriginSize os)
        : x(os.origin.x), y(os.origin.y),
          w(os.size.x),   h(os.size.y) {}

    // Corner accessors — direction-agnostic naming. The first index is
    // the position along x (0 = origin, 1 = far edge); the second is the
    // position along y. So x0y0() is always the origin corner, x1y1() is
    // always the far corner, regardless of whether the surrounding
    // coordinate system treats +y as down or up.
    //
    // Methods on Rect compute their formulas as written — they don't
    // sanity-check signs. For positive-w/h inputs (the conventional
    // case), `bbox`, `intersect`, `contains`, etc. behave as their
    // names suggest. For signed inputs, the same arithmetic produces
    // a well-defined but non-conventional result that the caller may
    // or may not find useful. `normalized()` is a convenience for
    // callers who explicitly want positive-w/h form.
    constexpr la::float2 x0y0() const { return {x,     y    }; }
    constexpr la::float2 x1y0() const { return {x + w, y    }; }
    constexpr la::float2 x0y1() const { return {x,     y + h}; }
    constexpr la::float2 x1y1() const { return {x + w, y + h}; }

    // Size and center helpers.
    constexpr la::float2 size()        const { return {w, h}; }
    constexpr la::float2 halfExtents() const { return {w * 0.5f, h * 0.5f}; }
    constexpr la::float2 center()      const { return {x + w * 0.5f, y + h * 0.5f}; }

    // Signed area: w * h. Sign reflects the winding implied by the field
    // signs (negative h or negative w produces negative area).
    constexpr float area() const { return w * h; }

    // Aspect ratio (w / h). Caller's responsibility to handle h == 0.
    constexpr float aspect() const { return w / h; }

    // True when w or h is exactly zero (`area() == 0`). Signed-area
    // rects (one negative dimension) are *not* empty — they cover a
    // region with negative winding, which `bbox`/`intersect` accept
    // and process via their min/max formulas without sign-judging.
    constexpr bool empty() const { return w == 0 || h == 0; }

    // Contextual-bool: true iff non-empty. Marked `explicit` so the
    // conversion only fires in boolean contexts (`if (r) ...`,
    // `while (r) ...`, `!r`) — no surprise int conversions.
    constexpr explicit operator bool() const { return !empty(); }
    constexpr bool operator!() const { return empty(); }

    // Normalized form: positive w/h occupying the same region. Useful
    // when you've constructed a Rect via `Corners` from arbitrary-
    // order points and want the conventional form before passing to
    // `bbox`/`intersect`/`contains`.
    constexpr Rect normalized() const {
        const float nx = w >= 0 ? x : x + w;
        const float ny = h >= 0 ? y : y + h;
        const float nw = w >= 0 ? w : -w;
        const float nh = h >= 0 ? h : -h;
        return Rect{nx, ny, nw, nh};
    }

    // Half-open hit-test: includes [x, x+w) × [y, y+h). Matches SDL/sokol
    // pixel coord convention so adjacent rects tile without overlap.
    constexpr bool contains(la::float2 p) const {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }
    // True when `other` is entirely inside this rect.
    constexpr bool contains(const Rect& other) const {
        return !other.empty() && !empty()
            && other.x >= x && other.y >= y
            && other.x + other.w <= x + w
            && other.y + other.h <= y + h;
    }
    // True when this and `other` overlap on a positive-area region.
    // Boundary-touching does not count as overlap.
    constexpr bool intersects(const Rect& other) const {
        return !empty() && !other.empty()
            && x < other.x + other.w && other.x < x + w
            && y < other.y + other.h && other.y < y + h;
    }
    // Overlap rect, or an empty rect if there is no overlap. Defined
    // out-of-class below so the impl can use ternary-min/max without
    // pulling <algorithm> into this header's transitive closure.
    constexpr Rect intersect(const Rect& other) const;
    // Bounding rect of the two. Treats empty rects as "nothing" — if
    // either is empty the other is returned unchanged.
    constexpr Rect bbox(const Rect& other) const;

    // Translated copy.
    constexpr Rect translated(la::float2 d) const {
        return Rect{x + d.x, y + d.y, w, h};
    }

    // Copy-with-mutation accessors.
    constexpr Rect withOrigin(la::float2 origin) const {
        return Rect{origin.x, origin.y, w, h};
    }
    constexpr Rect withSize(la::float2 sz) const {
        return Rect{x, y, sz.x, sz.y};
    }

    // Component-wise add, no semantic interpretation. Caller decides
    // what the four delta components mean. Translate: `r.adjusted({dx, dy, 0, 0})`.
    // Asymmetric edge-tweak: e.g. shrink top by 5 → `r.adjusted({0, 5, 0, -5})`.
    // Distinct from translated() / inset() / outset() because those bake
    // semantics in; adjusted() is the raw four-`+`s primitive.
    constexpr Rect adjusted(Rect d) const {
        return Rect{x + d.x, y + d.y, w + d.w, h + d.h};
    }

    // Uniform scale of all four fields. Useful for converting between
    // coordinate systems related by a single scalar (e.g. pixel-rect to
    // normalized UV: `pixelRect / atlasSize`).
    constexpr Rect operator*(float s) const { return Rect{x * s, y * s, w * s, h * s}; }
    constexpr Rect operator/(float s) const { return *this * (1.0f / s); }

    // Parameters for `scaled`. `scale` is per-axis (or scalar — see the
    // companion overload below); `center` is the pivot expressed in
    // normalized rect-local coords:
    //   {0, 0}    — top-left corner
    //   {1, 1}    — bottom-right corner
    //   {0.5, 0.5}— rect's own center (the default)
    //
    // The two-struct split exists so call sites can write
    // `r.scaled({.scale = {2, 1}})` (non-uniform) and
    // `r.scaled({.scale = 2.f})`   (uniform) without naming the type.
    // Overload resolution picks the right struct because linalg's
    // vec<T,2>(const T&) broadcast ctor is `explicit` (linalg.h:229),
    // blocking float→float2 implicit conversion in copy-init. If a
    // future linalg upgrade drops `explicit` on that ctor the call sites
    // become ambiguous; collapse the two structs to one with a
    // std::variant<float, la::float2> scale field as the migration.
    struct ScalingVec {
        la::float2 scale;
        la::float2 center = {0.5f, 0.5f};
    };
    struct ScalingScalar {
        float      scale;
        la::float2 center = {0.5f, 0.5f};
    };

    constexpr Rect scaled(ScalingVec s) const {
        const float px = x + w * s.center.x;
        const float py = y + h * s.center.y;
        const float nw = w * s.scale.x;
        const float nh = h * s.scale.y;
        return Rect{px - s.center.x * nw, py - s.center.y * nh, nw, nh};
    }
    constexpr Rect scaled(ScalingScalar s) const {
        return scaled(ScalingVec{.scale = {s.scale, s.scale}, .center = s.center});
    }

    // Factory: rect of size `sz` whose center is at `c`. Kept as a
    // semantic factory — doesn't reduce cleanly to a ctor + transform.
    static constexpr Rect centered(la::float2 c, la::float2 sz) {
        return Rect{c.x - sz.x * 0.5f, c.y - sz.y * 0.5f, sz.x, sz.y};
    }

    // Aspect-fit: returns the largest rect with `content`'s aspect
    // ratio that fits inside `*this`, centered. Letterboxes /
    // pillarboxes as needed. Only `content`'s w/h aspect matters; its
    // position is ignored. Equivalent to CSS `object-fit: contain`.
    // Result is always a sub-rect of `*this` (assuming non-degenerate
    // sizes). On degenerate `content` (zero w or h) the result has
    // zero size at `center()` — propagates the divide cleanly without
    // asserting, matching the rest of the sign-honest Rect API.
    constexpr Rect fitInside(la::float2 content) const {
        const la::float2 sz = content * la::minelem(size() / content);
        return centered(center(), sz);
    }

    // Aspect-fill: returns the smallest rect with `content`'s aspect
    // ratio that covers `*this`, centered. Overflows `*this` on the
    // non-binding axis. Equivalent to CSS `object-fit: cover`. Same
    // degenerate-content semantics as fitInside.
    constexpr Rect fillInside(la::float2 content) const {
        const la::float2 sz = content * la::maxelem(size() / content);
        return centered(center(), sz);
    }
};

constexpr Rect Rect::intersect(const Rect& other) const {
    if (empty() || other.empty()) return {};
    const float l = (x          > other.x        ) ? x          : other.x;
    const float t = (y          > other.y        ) ? y          : other.y;
    const float r = (x + w      < other.x + other.w) ? x + w     : other.x + other.w;
    const float b = (y + h      < other.y + other.h) ? y + h     : other.y + other.h;
    if (r <= l || b <= t) return {};
    return Rect{l, t, r - l, b - t};
}

constexpr Rect Rect::bbox(const Rect& other) const {
    if (empty()) return other;
    if (other.empty()) return *this;
    const float l = (x          < other.x        ) ? x          : other.x;
    const float t = (y          < other.y        ) ? y          : other.y;
    const float r = (x + w      > other.x + other.w) ? x + w     : other.x + other.w;
    const float b = (y + h      > other.y + other.h) ? y + h     : other.y + other.h;
    return Rect{l, t, r - l, b - t};
}

constexpr bool operator==(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
constexpr bool operator!=(const Rect& a, const Rect& b) { return !(a == b); }

// Per-edge insets of a safe-area-style boundary within the render
// surface. Direction-agnostic: `y0` is the inset on the smaller-y
// edge, `y1` on the larger-y edge, similarly for x. In the engine's
// screen-coord (y-down per SDL/sokol), `y0` is the top inset and `y1`
// is the bottom inset; the rename is a deliberate move away from
// y-axis-named field labels.
//
// Most consumers should reach for `Context::drawSafeRectInPts()` /
// `uiSafeRectInPts()` — the rect API is the natural shape for "lay out
// inside the safe region". The per-edge accessors
// (`Context::drawSafeInsetsInPts()` / `uiSafeInsetsInPts()`, 🎯T37)
// are for code that needs to align against a specific chrome edge —
// e.g. "place the top bar flush with the camera notch's bottom edge",
// where `uiSafeInsetsInPts().y0` is exactly the number you want.
//
// All values are in pt after 🎯T60 — the engine multiplies through
// pixelsPerPt internally; consumers stay in pt-space.
//
// To apply a `SafeAreaInsets` to a `Rect` directly, compose
// `Rect::adjusted` with a `Corners`-constructed delta, which is what
// `Context::drawSafeRectInPts` and `Context::uiSafeRectInPts` do
// internally.
struct SafeAreaInsets {
    float y0 = 0;
    float y1 = 0;
    float x0 = 0;
    float x1 = 0;
};

// Platform context provided to the game by ge::run() — once at session
// start (passed to the factory) and once per frame (passed to onRender).
// Cheaply copyable (shared_ptr internals); accessors below return live
// values that the engine updates each frame, so apps that capture a
// Context by value still see current state through the same shared M.
//
// Three rect accessors: each game must consciously choose which one
// suits the work it's about to draw or place. There is no `rect()`
// shortcut — the API forces the question rather than picking a default
// that's wrong half the time.
class Context {
public:
    Context(int surfaceWidth, int surfaceHeight, DeviceClass deviceClass,
            const std::string& dbPath,
            const std::string& schemaDdl = {});

    // The largest rectangle on the surface that no device chrome will
    // physically obscure — excludes only display cutouts (camera
    // notch, Dynamic Island, status bar when visible). Use for
    // **visuals**: the maze, the playfield, art that should be
    // entirely seen by the user. Visuals here can extend into gesture
    // zones — a glow that paints under a swipe-up area is fine, since
    // glows don't take input.
    // Returned in point space (🎯T60). All three rect accessors are in
    // points; multiply by pixelsPerPt() to convert to pixel coords for
    // sokol viewport calls.
    Rect drawSafeRectInPts() const;
    // The largest rectangle in which interactive UI is safe — excludes
    // display cutouts AND OS-reserved gesture / tappable zones (back-
    // swipe, recent-apps, status pull, etc). Use for **buttons,
    // sliders, drag handles**: anything that takes input. Smaller
    // than drawSafeRectInPts() on devices with system gestures.
    Rect uiSafeRectInPts() const;
    // Content surface rectangle — origin (0,0), full **content** size in
    // points (🎯T154). This is the game layout / sokol swapchain space,
    // NOT the viewer's native phone resolution under stream. The server
    // keeps a fixed content surface; the player letterboxes. Safe insets
    // map viewer chrome into this space. Multiply by pixelsPerPt() for
    // content pixel coords.
    Rect fullRectInPts() const;

    // Per-edge safe-area insets in pt (🎯T37 + 🎯T60 unified). Most
    // games consume the rect accessors above; reach for these only
    // when the task is "align flush with a specific chrome edge" —
    // e.g. `uiSafeInsetsInPts().y0` is the exact pt offset from the
    // top of the surface to the bottom of the camera notch. All four
    // edges are 0 on desktop. Under stream (server backend), insets come
    // from the player's DeviceInfo / SafeAreaUpdate mapped into content
    // space — same accessors, different discovery source (compile-time
    // backend: app vs server; no public runtime switch).
    SafeAreaInsets drawSafeInsetsInPts() const;
    SafeAreaInsets uiSafeInsetsInPts()   const;

    DeviceClass deviceClass() const;

    // Pixels per OS-style point. iPhone @3x ≈ 3.0, iPad @2x ≈ 2.0,
    // desktop @1x = 1.0. Use for fixed-physical sizing — touch
    // targets, body text, anything where the constraint is "must be
    // at least N physical mm". A 44pt button is `44 * pixelsPerPt()`
    // pixels wide.
    float pixelsPerPt() const;
    // Reciprocal of pixelsPerPt — for translating raw pixel coords
    // (touch events, sokol viewport) back into pt for layout math.
    float ptsPerPixel() const;
    // Form-factor multiplier, sublinear in screen size: sqrt of the
    // device's short-side mm relative to a reference phone (65mm,
    // iPhone Pro Max class). 1.0 on the reference phone, ~1.55 on
    // an 11" iPad. Use for chrome icons, headlines, presentation art
    // that should grow on tablets without blowing up linearly.
    //
    // Typical use: `gear_px = 28 * pixelsPerPt() * deviceUiScale()`.
    // Returns 1.0 on desktop (form-factor scaling doesn't apply when
    // the user controls the window size), and 1.0 in wire mode until
    // the player→server pixelRatio plumbing populates it.
    float deviceUiScale() const;

    // Current parallax delta in screen-local radians: x is rotation
    // around the screen-X axis (forward/back tilt), y is rotation
    // around the screen-Y axis (left/right tilt). Already scaled by
    // SessionHostConfig.parallaxFactor and absorbed by the engine's
    // EMA baseline (sustained tilts settle to zero, so the value is
    // always small).
    //
    // Returns {0, 0} when parallaxFactor == 0, on platforms with no
    // attitude sensor (desktop), and during the first ~τ seconds of a
    // session while the baseline is converging.
    //
    // Typical use: `cameraOffset += ctx.parallax() * depth;` for a
    // 2D parallax shift, or feed straight into a small rotation matrix
    // for a 3D scene tilt.
    la::float2 parallax() const;

    // 🎯T94 Presentation tilt: the tilt to apply to the *rendered view* as a
    // surrogate for physical device tilt, as a radians angle-vector (direction
    // = tilt direction, magnitude = tilt angle). Feed straight into
    // `ge::ortho::tilt(aspect, ctx.presentationTilt())`.
    //
    // This is AccelSynth's contribution, so it is {0, 0} whenever a real
    // hardware accelerometer is present (synth never runs) — the view tilts
    // only on synth platforms (desktop / sim / emu), never on a device whose
    // screen is already physically tilted. Distinct from gameplay gravity
    // (which is real-or-synth and uniform) and from parallax() (a subtle
    // always-on camera offset that does run on real devices).
    la::float2 presentationTilt() const;

    // 🎯T111 Smoothed frame-rate / frame-time, in frames-per-second and
    // seconds (frameTime == 1 / fps). An EMA of the run-loop dt — the same
    // value the debug overlay's FPS readout shows — refreshed every frame
    // before onRender, like parallax() and the safe rects. Both read 0 until
    // the first frame is timed.
    //
    // These are for PASSIVE DISPLAY (a HUD counter). To adapt the running
    // render path to performance, use RunConfig::onMetrics instead — a polled
    // read inside onRender invites a stateless `if (fps < x) simplify()` that
    // snaps back the moment simplifying lifts fps over the line. ge reports;
    // the app decides.
    float fps() const;
    float frameTime() const;

    // The engine-provided database.
    std::shared_ptr<sqlpipe::Database> db() const;

    // 🎯T101 Open this frame's swapchain render pass. Call once at the top of
    // onRender, before any draws; the returned ge::Pass holds the pass open for
    // its lifetime (typically the whole onRender body) and, on destruction,
    // ends the pass + commits + presents (direct mode) or encodes + transmits
    // (wire mode). Open any ge::offscreenPass blocks *before* this. Exactly one
    // swapchain pass per frame.
    Pass swapchainPass() const;

    // 🎯T132 Render-on-demand (opt-in; default is continuous, every frame).
    // setContinuousRendering(false) tells ge to stop drawing a static screen:
    // the run loop renders only when input arrives or requestRedraw() is called,
    // and idles (blocks on its event source, never busy-spins) in between —
    // saving GPU, present, and battery on a still screen. requestRedraw() is
    // thread-safe (call it from a timer / async / IAP callback) and renders
    // exactly one frame. Const because they mutate shared per-session state, not
    // the handle, so they work on the `const Context&` onRender/onUpdate receive.
    // Direct-mode (desktop / iOS / Android) only; the brokered/streaming path is
    // unaffected (🎯T34).
    void setContinuousRendering(bool on) const;
    bool continuousRendering() const;
    void requestRedraw() const;

    // 🎯T131.1 Render trigger — a *level* predicate ge polls each frame in
    // render-on-demand mode. While ANY registered trigger returns true the run
    // loop keeps rendering and does not idle; when all return false (and no
    // redraw / resuming input is pending) the loop idles. Use for sustained
    // activity whose end is a level condition — a physics sim settling (box2d
    // world asleep), an animation reaching rest. For *discrete* transitions (a
    // button press) call requestRedraw() instead — that's the one redraw bit
    // raised imperatively; a trigger is that same bit raised declaratively while
    // a predicate holds. Register once per session (e.g. in your factory); the
    // predicate runs on the game thread, so keep it cheap and non-throwing. It
    // may gate on game state, so "only while playing" is
    // `[&]{ return playing && world.anyAwake(); }` — no unregister needed.
    // Const (mutates shared per-session state, like the methods above).
    void addRenderTrigger(std::function<bool()> active) const;

    // 🎯T131.4 Render-on-demand by State diff — the zero-bookkeeping tier for a
    // screen whose render is a pure function of some State (a menu, a level-
    // picker). Provide a cheap generation counter that *changes* whenever the
    // render-relevant State changes — an O(1) value, e.g. a mutation counter
    // (the tweak::generation() pattern), a persistent-structure root id, or a
    // hash. ge renders only when it differs from the last presented frame's
    // value and idles when it's stable: no per-change requestRedraw, no trigger
    // predicate — the consumer never writes "turn rendering on/off". It's a
    // render trigger under the hood, so it composes with addRenderTrigger /
    // input. Register once per session. Const (mutates shared per-session state).
    //
    // Contract: render must be a pure function of the diffed State, with no
    // ambient time / RNG — the same discipline as 🎯T124's deterministic
    // headless render — and incidental fields (timestamps, frame counters) must
    // be excluded from the generation, or the screen never idles. Async state
    // changes that occur while the loop is already idle still need an explicit
    // requestRedraw() (nothing is polling to notice them).
    void renderWhenStateChanges(std::function<uint64_t()> generation) const;

    // 🎯T131.5 Monotonic count of frames presented — the swapchain present at the
    // end of each rendered frame. Flat across wall-clock ⇒ the screen idled;
    // advancing ⇒ rendering. This is the ground-truth signal that render-on-
    // demand dropped a static screen's presents to ~0/sec: onMetrics / fps() go
    // *stale* (not zero) when idle, but this number simply stops.
    uint64_t framesPresented() const;

    // SDL_EVENT_USER code identifying ge's redraw-wake event — requestRedraw()
    // pushes it to wake an idle loop; the host filters it out before onEvent.
    static constexpr int32_t kRedrawEventCode = 0x47455244;  // 'GERD'

    // Engine-internal: refresh per-frame state. Apps should not call
    // these — the run loop wires them up before each onUpdate /
    // onRender pair so accessors above always read live values.
    //
    // setSurfaceDimensions takes pixels (the GPU-facing unit); the rect
    // accessors divide by pixelsPerPt to expose pt-space values.
    //
    // setDrawSafeInsets / setUiSafeInsets take pt-space insets — render
    // hosts must convert from pixel insets (SDL_GetWindowSafeArea etc.)
    // before calling these.
    void setSurfaceDimensions(int surfacePxW, int surfacePxH);
    void setDrawSafeInsets(SafeAreaInsets inPt);
    void setUiSafeInsets(SafeAreaInsets inPt);
    void setDeviceClass(DeviceClass);
    void setPixelsPerPt(float);
    void setDeviceUiScale(float);
    void setParallax(la::float2);
    void setPresentationTilt(la::float2);
    // 🎯T111 Feed one frame's wall-clock dt (seconds) into the frame-time EMA
    // behind fps() / frameTime(). The host calls this each frame from its
    // per-frame refresh; dt outside (0, 1)s is ignored (0 / multi-frame stall).
    void recordFrameTime(float dt);
    // 🎯T101 The host installs its swapchain-pass factory here at session
    // start; Context::swapchainPass() invokes it.
    void setSwapchainPassFn(std::function<Pass()>);

    // 🎯T132 Engine-internal redraw bookkeeping. markRedraw() sets the pending
    // flag without a wake (the host calls it while already draining events);
    // redrawPending() peeks; takeRedraw() reads-and-clears for the loop's
    // render decision.
    void markRedraw() const;
    bool redrawPending() const;
    bool takeRedraw() const;

    // 🎯T131.1 Engine-internal: true if any registered render trigger is active.
    // The run loop / pumpEvents poll this to decide render-vs-idle in
    // render-on-demand mode (continuous mode never consults it).
    bool anyRenderTriggerActive() const;
    // 🎯T131.5 Engine-internal: the render host calls this at each swapchain
    // present to advance framesPresented().
    void recordPresent() const;

private:
    struct M;
    std::shared_ptr<M> m;
};

// 🎯T111 Engine-reported performance metrics, delivered via RunConfig::onMetrics.
// A growing bundle: fps is the first field; later fields (dropped frames, GPU
// time, render backend, resident memory, …) are added with default initialisers
// so existing `const Metrics&` handlers keep compiling. All times are in seconds,
// consistent with dt / onUpdate.
struct Metrics {
    float fps       = 0.0f;  // smoothed frames per second
    float frameTime = 0.0f;  // smoothed frame duration, seconds (== 1 / fps)
};

// 🎯T111 onMetrics delivery gate (pure). Returns whether a metrics report
// should fire for `fps`, given the `lastReportedFps` and the relative
// `threshold`: every frame when threshold <= 0; a baseline on the first valid
// reading (lastReportedFps <= 0); otherwise when |fps - lastReportedFps| >=
// threshold * lastReportedFps. Never fires for a non-positive fps. Gating
// against the last *reported* value (not the last frame) means no chatter at a
// boundary. The run loop owns lastReportedFps and sets it to `fps` whenever
// this returns true.
inline bool shouldReportMetrics(float fps, float lastReportedFps, float threshold) {
    if (fps <= 0.0f) return false;
    if (threshold <= 0.0f) return true;        // every frame
    if (lastReportedFps <= 0.0f) return true;  // baseline report
    const float dev = fps >= lastReportedFps ? fps - lastReportedFps
                                             : lastReportedFps - fps;
    return dev >= threshold * lastReportedFps;
}

// Render loop callbacks — the game's only interface with the engine.
//
// onRender receives the same Context the factory was given; the engine
// has refreshed its width / height / safeArea before each call so apps
// can pull whatever per-frame state they need without capturing extra
// values themselves. Future per-frame fields (parallax delta, tilt,
// frame index, …) join Context here, not the signature.
struct RunConfig {
    std::function<void(float dt)> onUpdate;
    std::function<void(const Context&)> onRender;
    std::function<void(const SDL_Event&)> onEvent;
    std::function<void()> onShutdown;

    // System back-press (Android Back button / predictive-back gesture,
    // 🎯T44). When set, the engine consumes the gesture and runs this
    // callback on the game thread; OS default (Activity.finish) is
    // suppressed. When unset (the default), the OS handles back —
    // typically by moving the app to background. iOS is a no-op in
    // practice because the immersive flag suppresses edge-swipe-back.
    //
    // Use to surface a pause menu, confirm exit, or step back through
    // an in-game stack.
    std::function<void()> onBackPressed;

    // OS memory-pressure warning (🎯T45). Fires on the game thread
    // when the platform reports memory pressure: iOS sends a single
    // warning (always Critical); Android grades into Low / Moderate /
    // Critical (see MemoryPressureLevel). Engine drops its own caches
    // first, so by the time this fires there's no engine-internal
    // slack left — the game's response is layered on top.
    //
    // Recommended action: drop high-cost caches (texture mips, audio
    // decoders, font glyph atlases). On Critical, drop everything not
    // needed for the next few frames; on Low, only drop genuinely
    // cold caches.
    std::function<void(MemoryPressureLevel)> onMemoryWarning;

    // 🎯T111 Engine performance metrics (fps first). Fires on the game thread,
    // between frames — after the per-frame refresh, before onRender — so a
    // quality decision made here applies to the very next frame's draw. It
    // fires when smoothed fps deviates from the last reported value by at least
    // SessionHostConfig.metricsReportThreshold (0 => every frame); a baseline
    // report fires on the first valid reading.
    //
    // Use it to adapt the running render path — drop or simplify non-critical
    // visuals when fps falls — and keep your OWN hysteresis: ge reports, your
    // app decides; the engine never steps quality itself. A robust consumer
    // remembers that it simplified and only restores above a *higher* fps (a
    // two-threshold band), so a recovery caused by simplifying doesn't snap
    // back to the expensive path.
    //
    // Unset (the default) => no callback; the engine still maintains the EMA
    // behind Context::fps() / frameTime() and the debug overlay.
    std::function<void(const Metrics&)> onMetrics;
};

// Configuration for the session host.
struct SessionHostConfig {
    // Initial content-surface size (server swapchain / encode size). Under
    // stream the host retargets the content *aspect* to the attached player's
    // DeviceInfo so the glass is filled (no letterbox gutters). Pixel budget
    // keeps the longer edge of this initial size. Direct builds use the
    // window's native size after creation.
    int width  = 0;
    int height = 0;
    bool headless = true;  // true = H.264 server, false = native window

    // 🎯T124 Headless render-to-PNG: create the host window unmapped
    // (SDL_WINDOW_HIDDEN). Set automatically by ge::renderToPng; you rarely set
    // it directly. No effect in headless (server) mode.
    bool hidden = false;

    // 🎯T92.2.2 Server mode is a BUILD VARIANT, not a config field: a server
    // build (GE_SERVER_BUILD) makes ge::run a hidden-window DirectRenderHost that
    // streams ge's canonical H.264 wire to a player via spyder's relay. The relay
    // address is read from the GE_SERVER env inside runServer; the app's config
    // and code are identical to its desktop build.

    // App identity for persistent DB path (via SDL_GetPrefPath) on **direct**
    // builds only. Server builds (`GE_SERVER_BUILD`) always open :memory: —
    // durable state is player-authoritative (sqlpipe GE2T; 🎯T154).
    const char* orgName = nullptr;
    const char* appName = nullptr;

    // Schema DDL for the engine-provided database.
    // Passed to sqlpipe::Database; sqlift diffs against existing
    // schema and auto-migrates. Leave empty to skip schema setup.
    std::string schemaDdl;

    // Session requirements — sent to the player on attachment, BEFORE
    // DeviceInfo, so the player can apply orientation/sensor hints before
    // creating its window. Game-wide, not per-session.
    uint8_t sensors = 0;      // Bitmask: wire::kSensorAccelerometer
    uint8_t orientation = 0;  // wire::kOrientation* value, 0 = no lock

    // Initial screen-saver policy. Default: false (let the device sleep
    // normally, per OS convention). Set true for games where the primary
    // input is the accelerometer — otherwise the device may sleep
    // mid-play because the screen isn't being touched. Games that need
    // per-state control (e.g. saver enabled in menus, disabled during
    // play) should leave this false and call SDL_DisableScreenSaver /
    // SDL_EnableScreenSaver at the appropriate moments.
    bool disableScreenSaver = false;

    // Device-tilt parallax. 0 = off (default). > 0 enables Core Motion
    // (iOS) / SensorManager TYPE_GAME_ROTATION_VECTOR (Android) and
    // scales the resulting screen-XY tilt before the engine exposes it
    // via Context::parallax(). The same float controls both opt-in and
    // sensitivity — collapsing what would otherwise be a separate bool
    // + sensitivity float into one knob.
    //
    // The engine maintains an EMA baseline (τ ≈ 1.0s) so sustained
    // tilts become the new neutral; only recent motion produces a
    // non-zero parallax. Apps multiply ctx.parallax() by their own
    // depth/sensitivity to produce camera offsets, layer shifts, or
    // scene rotations.
    //
    // Platform support:
    //   * iOS — CMMotionManager.deviceMotion.attitude (sensor-fused
    //     gyro + accel; drift-free, captures vertical-axis twist).
    //   * Android — Sensor.TYPE_GAME_ROTATION_VECTOR via JNI; same
    //     fused-quaternion semantics, no magnetometer dependency
    //     so indoor magnetic interference doesn't muddy the signal.
    //   * Desktop — no-op (Context::parallax() returns {0, 0}).
    float parallaxFactor = 0.0f;

    // Take the entire screen — hide system chrome (status bar, gesture
    // / navigation bar) so the game owns the full surface. Reduces
    // safe-area insets to display cutouts only (camera notch, Dynamic
    // Island), exposing the largest meaningful Context::rect() the
    // device can offer.
    //
    // Platform support:
    //   * Android — applied at runtime via WindowInsetsController
    //     (immersive sticky). Brief flicker on launch as bars hide.
    //   * iOS — hides status bar via prefersStatusBarHidden swizzle
    //     (Immersive_apple.mm) so stream and direct match pixel-for-
    //     pixel; status-bar contents (clock, battery) are otherwise
    //     non-deterministic. iPad Split View / Slide Over still needs
    //     `UIRequiresFullScreen` in Info.plist (launch-time only).
    //   * Desktop — no-op.
    bool immersive = false;

    // 🎯T111 RunConfig::onMetrics delivery cadence. The callback fires when
    // smoothed fps changes from the last reported value by at least this
    // fraction (0.1 = 10%). 0 => fire every frame. Ignored when onMetrics is
    // unset. Relative (not absolute) so the same value behaves consistently at
    // 30 or 120 fps.
    float metricsReportThreshold = 0.1f;

    // 🎯T136 Crash diagnostics. When true (default), the direct run loop wraps
    // the consumer callbacks (onUpdate / onRender / onEvent) so an uncaught
    // exception is logged through the app-channel logger before it propagates,
    // and installs last-gasp fatal-signal handlers (SIGSEGV / SIGABRT / SIGBUS
    // / SIGILL / SIGFPE) that log signal + backtrace before re-raising the OS
    // default. Set false for a tool/test that wants raw aborts and untouched
    // exception propagation. No effect on the brokered/streaming path.
    bool crashDiagnostics = true;
};

// Factory receives platform context and returns render loop callbacks.
using Factory = std::function<RunConfig(Context)>;

// Blocks until SIGINT or all sessions end.
void run(Factory factory, const SessionHostConfig& config = {});

// 🎯T124 Headless one-shot render. Builds a hidden-window direct host (no stream broker,
// no run loop, nothing shown), runs `factory`, invokes `prepare` (restore your
// State here — it runs after the factory and before the single onRender),
// renders exactly one frame, and writes it to `outPath` as a PNG. Returns true
// on success. The app's onRender must open ctx.swapchainPass() as usual.
bool renderToPng(Factory factory, SessionHostConfig config,
                 const std::function<void()>& prepare,
                 const std::string& outPath);

// 🎯T124 One item in a headless render batch: a state-restore callback and the
// PNG it should produce.
struct RenderItem {
    std::function<void()> prepare;   // restore this item's State (after the factory)
    std::string           outPath;   // PNG output path
};

// 🎯T124 Render many states against ONE host (one sokol setup; the factory runs
// once), amortising startup across a corpus. Each item's prepare() restores its
// State, then a frame is rendered and written. A failing or throwing item is
// logged and skipped — one bad fixture doesn't zero the run. Returns the count
// of items successfully written. This is what `render --batch` drives.
int renderBatch(Factory factory, SessionHostConfig config,
                const std::vector<RenderItem>& items);

} // namespace ge
