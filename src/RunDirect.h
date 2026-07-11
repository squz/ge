// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Internal bridge between the modality dispatcher (SessionHost.mm) and the
// wire TU's runServer (src/bridge/SessionHost_server.mm) — 🎯T92.2.2.
//
// Server mode historically used ServerHook + runDirectHosted (single player).
// Multi-session is now implemented entirely in SessionHost_server.mm; this
// hook remains for any residual single-sink tooling.
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
struct ServerHook {
    std::function<void(const std::uint8_t*, int, int)> sink;
    std::atomic<bool>* active = nullptr;
    std::function<void()> onStop;
};

// The direct run loop, optionally driving server mode via `server` (null = the
// normal windowed path). Defined in SessionHost.mm; called by runDirect's
// dispatch there and by runServer in the wire TU.
void runDirectHosted(Factory factory, SessionHostConfig config,
                     const ServerHook* server);

} // namespace ge
