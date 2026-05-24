// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Manages the server lifecycle: bgfx context, ged sideband connection,
// H.264 encode pipeline, and per-session state. The game provides a
// factory that creates session state and returns render loop callbacks.
#pragma once

#include <sqlpipe.h>

#include <ge/Linalg.h>
#include <SDL3/SDL_events.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
// engine pays to convert once at the SDL/bgfx boundary. The integer
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

    // Half-open hit-test: includes [x, x+w) × [y, y+h). Matches SDL/bgfx
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
// screen-coord (y-down per SDL/bgfx), `y0` is the top inset and `y1`
// is the bottom inset; the rename is a deliberate move away from
// y-axis-named field labels.
//
// Most consumers should reach for `Context::drawSafeRect()` /
// `uiSafeRect()` — the rect API is the natural shape for "lay out
// inside the safe region". The per-edge accessors
// (`Context::drawSafeInsets()` / `uiSafeInsets()`, 🎯T37) are for
// code that needs to align against a specific chrome edge — e.g.
// "place the top bar flush with the camera notch's bottom edge",
// where `uiSafeInsets().y0` is exactly the number you want.
//
// To apply a `SafeAreaInsets` to a `Rect` directly, compose
// `Rect::adjusted` with a `Corners`-constructed delta, which is
// what `Context::drawSafeRect` and `Context::uiSafeRect` do
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
    Rect drawSafeRect() const;
    // The largest rectangle in which interactive UI is safe — excludes
    // display cutouts AND OS-reserved gesture / tappable zones (back-
    // swipe, recent-apps, status pull, etc). Use for **buttons,
    // sliders, drag handles**: anything that takes input. Smaller
    // than drawSafeRect on devices with system gestures.
    Rect uiSafeRect() const;
    // The full bgfx backbuffer rectangle — origin (0,0), full surface
    // size. Use when you genuinely want to bleed past every safe area
    // (full-surface backgrounds, decorative imagery that surrounds
    // chrome). On desktop this is identical to the other two rects.
    Rect fullRect() const;

    // Per-edge insets in render-surface pixels (🎯T37). Most consumers
    // want the rect accessors above; reach for these only when the
    // task is "align with a specific chrome edge" (e.g. flush against
    // the camera notch's bottom edge = `uiSafeInsets().y0`). All four
    // edges are 0 on desktop and on wire-mode sessions until the
    // player→server safe-area plumbing lands.
    SafeAreaInsets drawSafeInsets() const;
    SafeAreaInsets uiSafeInsets()   const;

    DeviceClass deviceClass() const;

    // Pixels per OS-style point. iPhone @3x ≈ 3.0, iPad @2x ≈ 2.0,
    // desktop @1x = 1.0. Use for fixed-physical sizing — touch
    // targets, body text, anything where the constraint is "must be
    // at least N physical mm". A 44pt button is `44 * pixelsPerPt()`
    // pixels wide.
    float pixelsPerPt() const;
    // Reciprocal of pixelsPerPt — for translating raw pixel coords
    // (touch events, bgfx viewport) back into pt for layout math.
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

    // The engine-provided database.
    std::shared_ptr<sqlpipe::Database> db() const;

    // Engine-internal: refresh per-frame state. Apps should not call
    // these — the run loop wires them up before each onUpdate /
    // onRender pair so accessors above always read live values.
    void setDimensions(int surfaceWidth, int surfaceHeight);
    void setDrawSafeInsets(SafeAreaInsets);
    void setUiSafeInsets(SafeAreaInsets);
    void setPixelsPerPt(float);
    void setDeviceUiScale(float);
    void setParallax(la::float2);

private:
    struct M;
    std::shared_ptr<M> m;
};

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
};

// Configuration for the session host.
struct SessionHostConfig {
    // Render dimensions. In headless mode, 0×0 means "wait for the
    // player's DeviceInfo and adopt its dimensions (halved)".
    int width  = 0;
    int height = 0;
    bool headless = true;  // true = H.264 server, false = native window

    // App identity for persistent DB path (via SDL_GetPrefPath).
    // Bundled (non-headless) mode opens a persistent file; headless
    // (server) mode always uses :memory: — persistence is owned by
    // the player and synced via sqlpipe.
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
    //   * iOS — partial; iPhone landscape already auto-hides the
    //     status bar. iPad Split View / Slide Over additionally
    //     requires `UIRequiresFullScreen` in the app's Info.plist
    //     (read by iOS at launch; cannot be toggled at runtime).
    //   * Desktop — no-op.
    bool immersive = false;
};

// Factory receives platform context and returns render loop callbacks.
using Factory = std::function<RunConfig(Context)>;

// Blocks until SIGINT or all sessions end.
void run(Factory factory, const SessionHostConfig& config = {});

} // namespace ge
