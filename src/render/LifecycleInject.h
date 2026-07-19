// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Engine-internal injection entry point for the app-channel (🎯T92.2).
//
// Background / foreground / quit are injected straight through SDL_PushEvent
// (the engine already handles those SDL events in pumpEvents). The memory-
// pressure path is different: on iOS, SDL also posts SDL_EVENT_LOW_MEMORY for
// the same OS notification the T45 observer already watches, so pushing an
// SDL event would double-fire onMemoryWarning. Instead appchannel routes the
// injected warning through here; DirectRenderHost.mm owns the pending-warning
// atomic that pumpEvents drains on the game thread — exactly the path the
// real iOS/Android notifications take.

#pragma once

#include <ge/SessionHost.h>  // ge::MemoryPressureLevel

namespace ge::detail {

// Thread-safe. Stores `level` as the pending memory-pressure warning; the
// next DirectRenderHost::pumpEvents on the game thread drains it and invokes
// the consumer's onMemoryWarning. Harmless if no host is running (the atomic
// is simply overwritten before anything reads it).
void injectMemoryWarning(MemoryPressureLevel level);

// 🎯T154 viewer lifecycle over the wire (ServerSession → same atomics as OS).
void injectBackPressed();
void injectAudioFocus(bool gained);
// Viewer foreground/background: when streaming, gates paused() like host OS.
void injectViewerBackgrounded(bool backgrounded);
bool viewerBackgrounded();

} // namespace ge::detail
