// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T177 Gesture-hint input driver — the same timeline that draws the
// hand drives real touch input.
//
// A `hint::Player` is a pure timeline; the SDF hand (`<ge/hint_hand.h>`)
// is one consumer of its pointer states. `InputDriver` is a second,
// STRICTLY OPT-IN consumer that converts the same interpolated states
// into synthetic SDL finger events (down / motion / up; pinch = two
// fingers), delivered through the app's normal event path — so a drag
// demo rotates a GlobeController globe and a tap demo presses a
// ge::Button via the exact production input handling, with zero
// game-side correlation code and no possibility of drift between what
// is drawn and what is injected.
//
// Opt-in per Player, presentation and injection independent: a hint
// that *suggests* a gesture the user should make (yourworld2's
// hint-direction hand) draws with no driver; a self-playing demo
// attaches one. Never couple implicitly — driving mutates real game
// state, and not all state is cheaply revertible.
//
// Usage (after `player.update(dt)` each frame, same areaPts as the
// renderer so the finger touches exactly what the hand shows):
//
//     driver.update(ctx, player, areaPts);
//
// Synthetic events carry `kSyntheticTouchId` and finger ids from a
// reserved range — `isSyntheticFinger(id)` lets a game filter
// consequences (e.g. a tap demo that must not press a real button).
// Tag events still fire from the Player, and land the same frame the
// corresponding synthetic event is emitted (the event reaches onEvent
// on the next pump) — so "snapshot at contact, restore after release"
// orchestration sees the snapshot strictly before the input applies.
//
// Interruption: call `cancel()` when real input arrives (or the demo
// ends) — held synthetic fingers are lifted cleanly so the game is
// never left mid-drag. Automatic real-input interruption policy is the
// scenario layer's job (🎯T178).

#pragma once

#include <ge/SessionHost.h>  // ge::Context, ge::Rect
#include <ge/hint.h>

#include <SDL3/SDL_events.h>

#include <functional>
#include <vector>

namespace ge::hint {

// Reserved synthetic touch device id ('SP2G') — real SDL touch devices
// never use it, mirroring ge::kMouseId / wire::kSdlEventMagic style.
inline constexpr SDL_TouchID kSyntheticTouchId = SDL_TouchID(0x53503247);

// Synthetic finger ids: kSyntheticFingerBase + pointer index.
inline constexpr SDL_FingerID kSyntheticFingerBase = SDL_FingerID(0x5350324700ULL);

// True iff `id` came from an InputDriver.
bool isSyntheticFinger(SDL_FingerID id);

class InputDriver {
public:
    // Where events go. Defaults to SDL_PushEvent — the production event
    // queue. Tests (and the 🎯T178 scenario layer) may capture instead.
    std::function<void(const SDL_Event&)> sink;

    // Diff the player's pointer states against the previous frame and
    // emit finger events for the transitions. Call once per frame,
    // after player.update(dt), with the renderer's areaPts.
    void update(const Context& ctx, const Player& player, const Rect& areaPts);

    // Lift any held synthetic fingers immediately (emits FINGER_UP at
    // the last position). Use on real-input interruption or demo end.
    void cancel();

    // True while any synthetic finger is down.
    bool active() const;

private:
    struct Finger {
        bool       down = false;
        la::float2 lastNorm{0.0f, 0.0f};  // 0..1 against the full surface
    };
    std::vector<Finger> fingers_;

    void emit(uint32_t type, int index, la::float2 norm);
};

}  // namespace ge::hint
