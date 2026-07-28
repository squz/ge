// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T177 InputDriver — pointer states → synthetic SDL finger events.
//
// Coordinate contract: PointerState.pos is unit gesture space; the
// renderer maps it onto areaPts. SDL finger coords are normalized 0..1
// against the FULL surface (ge::input::fromSdl denormalizes with
// fullRectInPts) — so the driver inverts exactly that mapping:
//   norm = (area.origin + pos * area.size) / full.size
// Same areaPts as the renderer ⇒ the finger lands where the hand shows.

#include <ge/hint_input.h>

#include <SDL3/SDL.h>

#include <algorithm>

namespace ge::hint {

bool isSyntheticFinger(SDL_FingerID id) {
    return id >= kSyntheticFingerBase &&
           id < kSyntheticFingerBase + 16;  // more fingers than any clip uses
}

void InputDriver::emit(uint32_t type, int index, la::float2 norm) {
    SDL_Event evt{};
    evt.type             = type;
    evt.tfinger.type     = static_cast<SDL_EventType>(type);
    evt.tfinger.timestamp = SDL_GetTicksNS();
    evt.tfinger.touchID  = kSyntheticTouchId;
    evt.tfinger.fingerID = kSyntheticFingerBase + index;
    evt.tfinger.x        = norm.x;
    evt.tfinger.y        = norm.y;
    Finger& f            = fingers_[size_t(index)];
    evt.tfinger.dx       = f.down ? norm.x - f.lastNorm.x : 0.0f;
    evt.tfinger.dy       = f.down ? norm.y - f.lastNorm.y : 0.0f;
    evt.tfinger.pressure = 1.0f;
    if (sink) sink(evt);
    else SDL_PushEvent(&evt);
}

void InputDriver::update(const Context& ctx, const Player& player, const Rect& areaPts) {
    const auto ps   = player.pointers();
    const Rect full = ctx.fullRectInPts();
    if (full.w <= 0.0f || full.h <= 0.0f) return;

    if (fingers_.size() < ps.size()) fingers_.resize(ps.size());

    // A shrinking pointer set (player swapped mid-flight) lifts extras.
    for (size_t i = ps.size(); i < fingers_.size(); ++i) {
        if (!fingers_[i].down) continue;
        emit(SDL_EVENT_FINGER_UP, int(i), fingers_[i].lastNorm);
        fingers_[i].down = false;
    }

    for (size_t i = 0; i < ps.size(); ++i) {
        const PointerState& p = ps[i];
        Finger&             f = fingers_[i];
        la::float2 norm{
            std::clamp((areaPts.x + p.pos.x * areaPts.w) / full.w, 0.0f, 1.0f),
            std::clamp((areaPts.y + p.pos.y * areaPts.h) / full.h, 0.0f, 1.0f)};

        if (p.contact && !f.down) {
            emit(SDL_EVENT_FINGER_DOWN, int(i), norm);
            f.down     = true;
            f.lastNorm = norm;
        } else if (p.contact && f.down) {
            if (norm.x != f.lastNorm.x || norm.y != f.lastNorm.y) {
                emit(SDL_EVENT_FINGER_MOTION, int(i), norm);
                f.lastNorm = norm;
            }
        } else if (!p.contact && f.down) {
            emit(SDL_EVENT_FINGER_UP, int(i), norm);
            f.down     = false;
            f.lastNorm = norm;
        }
    }
}

void InputDriver::cancel() {
    for (size_t i = 0; i < fingers_.size(); ++i) {
        if (!fingers_[i].down) continue;
        emit(SDL_EVENT_FINGER_UP, int(i), fingers_[i].lastNorm);
        fingers_[i].down = false;
    }
}

bool InputDriver::active() const {
    return std::any_of(fingers_.begin(), fingers_.end(),
                       [](const Finger& f) { return f.down; });
}

}  // namespace ge::hint
