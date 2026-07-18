// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// SP2I packing of SDL_Event — the real marshalling path for player→server
// input (🎯T156). ServerSession unpacks with these helpers; tests inject
// through the same bytes so direct vs wire cannot silently diverge.

#pragma once

#include <ge/Protocol.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace wire {

// Pack one SP2I frame (header + SDL_Event) as on the player wire.
inline void packSdlEvent(const SDL_Event& ev, std::vector<uint8_t>& out) {
    MessageHeader hdr{kSdlEventMagic, static_cast<uint32_t>(sizeof(SDL_Event))};
    out.resize(sizeof(hdr) + sizeof(SDL_Event));
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    std::memcpy(out.data() + sizeof(hdr), &ev, sizeof(SDL_Event));
}

// Unpack SP2I; returns false if not a well-formed SDL event message.
inline bool unpackSdlEvent(const uint8_t* data, size_t n, SDL_Event& out) {
    if (!data || n < sizeof(MessageHeader) + sizeof(SDL_Event)) return false;
    MessageHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != kSdlEventMagic) return false;
    if (hdr.length < sizeof(SDL_Event)) return false;
    if (n < sizeof(MessageHeader) + hdr.length) return false;
    std::memcpy(&out, data + sizeof(MessageHeader), sizeof(SDL_Event));
    return true;
}

inline bool unpackSdlEvent(const std::vector<uint8_t>& msg, SDL_Event& out) {
    return unpackSdlEvent(msg.data(), msg.size(), out);
}

// Host-side filter used by DirectRenderHost: when AccelSynth owns the
// gesture, external SENSOR_UPDATE must not reach the game.
template <typename Synth>
inline bool shouldDeliverSensorToGame(const Synth* synth, const SDL_Event& e) {
    if (e.type != SDL_EVENT_SENSOR_UPDATE) return true;
    if (synth && synth->ownsSensorStream()) return false;
    return true;
}

} // namespace wire
