// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 Gesture-hint service — skeletal gesture timelines with event tags.
//
// Games hint touch gestures (tap, swipe, pinch, …) with an animated hand.
// This header is the rendering-agnostic core: a gesture compiles into a
// keyframed `Clip` — per-pointer tracks of {position in unit gesture
// space, contact, press, opacity} plus a list of tagged events — and a
// `Player` advances the clip with dt, exposing per-frame `PointerState`s
// and firing tags (`contact`, `release`, `apex`, `pinch-start`, …)
// through a callback at the exact timeline moment. The game maps unit
// gesture space onto whatever screen rect it likes and either renders
// its own art from the pointer states (data-only tier) or hands the
// Player to ge's default SDF hand renderer (`<ge/hint_hand.h>`).
//
// Trajectory is a runtime parameter (`Params.from` / `Params.to`), never
// baked — a swipe hint can run from *this* card to *that* slot. Motion is
// interpolated continuously, so it samples at whatever rate the display
// runs (ProMotion-friendly); nothing is quantized to authored frames.
//
// Tracks are deliberately a variant (`Track::Payload`): pointer tracks
// today, a device-pose track (🎯T170.1 — tilt-the-phone hints) joins the
// same machinery without an API break.
//
// Coordinates: unit gesture space is the (0..1)×(0..1) square, y-down
// (SDL screen convention). Pointers may travel slightly outside it
// during approach/exit flourishes; contact only happens inside.

#pragma once

#include <ge/Linalg.h>

#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace ge::hint {

enum class Kind {
    Tap,
    DoubleTap,
    LongPress,
    Swipe,
    Drag,
    PinchZoom,
    PinchRotate,
};

// Well-known tag strings. Compared by content, so consumers may also
// match against literals.
namespace tag {
inline constexpr std::string_view contact    = "contact";     // finger meets glass
inline constexpr std::string_view release    = "release";     // finger leaves glass
inline constexpr std::string_view apex       = "apex";        // midpoint of a swipe/drag stroke
inline constexpr std::string_view holdStart  = "hold-start";  // long-press threshold reached
inline constexpr std::string_view pinchStart = "pinch-start"; // both fingers down (pinch clips)
inline constexpr std::string_view pinchEnd   = "pinch-end";   // pinch fingers released
}  // namespace tag

// Per-frame state of one hint pointer, in unit gesture space.
struct PointerState {
    la::float2 pos{0.5f, 0.5f};
    bool       contact = false;  // touching the surface right now
    float      press   = 0.0f;   // 0..1 press-down amount (drives finger curl / squash)
    float      opacity = 0.0f;   // 0..1 overall fade of the hint art
};

// 🎯T170.1 Device-pose payload for device-motion hints (tilt). Present in
// the API now so the track variant is stable; no built-in clip emits it yet.
struct DevicePose {
    float roll    = 0.0f;  // radians, +ve = right edge dips
    float pitch   = 0.0f;  // radians, +ve = top edge tips away
    float opacity = 0.0f;
};

// Gesture parameterisation. Positions are in unit gesture space.
//   Tap/DoubleTap/LongPress: `from` is the touch point (`to` unused).
//   Swipe/Drag:              stroke runs `from` → `to`.
//   PinchZoom:               fingers spread from around the midpoint of
//                            (from,to) out to from/to (zoom-in visual).
//   PinchRotate:             fingers orbit the midpoint of (from,to),
//                            starting at from/to.
struct Params {
    la::float2 from{0.5f, 0.6f};
    la::float2 to{0.5f, 0.4f};
    bool       loop    = true;
    float      speed   = 1.0f;  // time scale; 2 = twice as fast
    float      loopGap = 0.5f;  // seconds of blank between loop iterations
};

// ── Clip: the compiled timeline ─────────────────────────────────────
// Consumers normally construct a Player directly from (Kind, Params);
// Clip is exposed for tests and future custom/authored gestures.

enum class Ease { Step, Linear, InOut, Out, In };

struct Key {
    float      t = 0.0f;    // seconds from clip start
    la::float2 pos{};
    bool       contact = false;
    float      press   = 0.0f;
    float      opacity = 0.0f;
    // Interpolation of pos/press/opacity over the segment *ending* at
    // this key. `contact` always steps (it flips at the key's own time).
    Ease ease = Ease::InOut;
};

struct TagEvent {
    float            t = 0.0f;
    std::string_view tag;  // one of hint::tag::* (static storage)
};

struct Track {
    // Variant by role, not std::variant: pointer tracks carry keys below;
    // a 🎯T170.1 device-pose track will carry pose keys in a sibling field.
    std::vector<Key> keys;  // ascending t; first key states the initial value
};

struct Clip {
    float                 duration = 0.0f;  // seconds, before Params.speed
    std::vector<Track>    tracks;           // one per pointer
    std::vector<TagEvent> tags;             // ascending t
};

// Compile a built-in gesture into a clip. Exposed for tests.
Clip makeClip(Kind kind, const Params& params);

// ── Player ──────────────────────────────────────────────────────────

class Player {
public:
    Player() = default;
    Player(Kind kind, Params params = {});

    // Advance the timeline. Fires onTag for every tag whose time was
    // crossed this step, in order. dt is wall-clock seconds.
    void update(float dt);

    // Restart from t = 0 (tags fire again).
    void reset();

    // Fired from within update(), after pointer states are refreshed —
    // a `contact` handler reading pointers() sees contact == true at the
    // touch position. The string_view has static storage.
    std::function<void(std::string_view)> onTag;

    // Current per-pointer states, valid after the first update().
    // One entry per pointer (two for pinch gestures).
    std::span<const PointerState> pointers() const { return states_; }

    Kind  kind() const { return kind_; }
    // False once a non-looping clip has played out (looping: always true).
    bool  active() const;
    float time() const { return t_; }
    // Clip duration in seconds at the configured speed (excludes loopGap).
    float duration() const;

private:
    void  sample();
    void  fireTags(float from, float to);  // (from, to] in clip time

    Kind                      kind_ = Kind::Tap;
    Params                    params_{};
    Clip                      clip_{};
    float                     t_       = 0.0f;  // clip-local seconds (unscaled)
    bool                      started_ = false;
    std::vector<PointerState> states_;
};

}  // namespace ge::hint
