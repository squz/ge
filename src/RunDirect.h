// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Internal bridge between the modality dispatcher (SessionHost.mm) and the
// wire TU's runServer (src/bridge/SessionHost_server.mm) — 🎯T92.2.2.
//
// Server mode is a hidden-window variant of the direct run loop: one render
// host (DirectRenderHost), one wire (ge's canonical H.264). runServer builds a
// ServerSession, packs its frame sink + active flag into a ServerHook, and
// hands it to runDirectHosted here so the whole loop stays in SessionHost.mm.
// SessionHost.mm never names the ServerSession type (only the type-erased
// std::function / atomic in ServerHook), so it keeps compiling clean under
// GE_DIRECT_ONLY — where the wire TU (and thus runServer) is absent.
#pragma once

#include <ge/SessionHost.h>

#include <atomic>
#include <cstdint>
#include <functional>

namespace ge {

// Server-mode hook grafted onto the direct run loop. When `active` is non-null
// the host hides its window, arms per-frame capture into `sink`, and bypasses
// render-on-demand so the remote player gets a continuous stream. `onStop` runs
// once the loop exits (ServerSession::stop()).
//
// Command-stream (🎯T128): `beforeRender` / `afterRender` arm LiveCapture around
// onRender so SpriteBatch serialises GE2S; when `capturePixels` is non-null and
// false, GPU framebuffer readback is skipped (sprite path does not need it).
struct ServerHook {
    std::function<void(const std::uint8_t*, int, int)> sink;
    std::atomic<bool>* active = nullptr;
    std::function<void()> onStop;
    std::function<void()> beforeRender;
    std::function<void()> afterRender;
    std::atomic<bool>* capturePixels = nullptr; // null ⇒ capture when active
};

// The direct run loop, optionally driving server mode via `server` (null = the
// normal windowed path). Defined in SessionHost.mm; called by runDirect's
// dispatch there and by runServer in the wire TU.
void runDirectHosted(Factory factory, SessionHostConfig config,
                     const ServerHook* server);

} // namespace ge
