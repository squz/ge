// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// RenderHost — the boundary between the engine subsystem and the render
// subsystem.
//
// The engine subsystem (game logic + ge::run) is platform-agnostic: it
// receives abstract events and submits bgfx draw calls. It does not know
// whether the render output is going to a local window or being captured
// and streamed to a remote player.
//
// Two concrete implementations:
//
//   DirectRenderHost — owns a real SDL window + Metal/Vulkan surface; bgfx
//     draws straight to it; SDL events come from local input. Used by
//     distribution builds (one binary, no ged, no wire).
//
//   ServerWireBridge — owns a headless framebuffer + H.264 encoder + per-
//     session WebSocket wire to a remote player; bgfx draws into the
//     framebuffer; encoded frames stream out; SDL events arrive from
//     the wire. Used by the brokered (ged + player) modality.
//
// The PlayerWireBridge (player-side counterpart of ServerWireBridge) is
// not a RenderHost — it wraps a DirectRenderHost, intercepts its events
// for transmission, and feeds decoded frames back as textures to display.
#pragma once

#include <ge/Linalg.h>

#include <ge/Protocol.h>
#include <ge/SessionHost.h>

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <functional>

namespace ge {

class RenderHost {
public:
    virtual ~RenderHost() = default;

    // Render dimensions (logical pixels). Valid after host-specific init.
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual DeviceClass deviceClass() const = 0;

    // One-shot init info from engine to render subsystem (sensors,
    // orientation, aspect lock). Sent before the first frame.
    virtual void send(const wire::SessionConfig&) = 0;

    // Register the engine's event handler. The host invokes it for each
    // incoming SDL event (whatever the source — local input or wire RX).
    virtual void setEventHandler(std::function<void(const SDL_Event&)>) = 0;

    // Drain pending events through the registered handler.
    virtual void pumpEvents() = 0;

    // Per-frame pre-render refresh, called once each frame before onUpdate /
    // onRender. Adopts any staged resize and updates the live Context's
    // per-frame fields (dimensions, safe-area insets, parallax, presentation
    // tilt) so the callbacks observe current values. It does NOT open a render
    // pass (🎯T101): consumers open exactly one swapchain pass per frame via
    // Context::swapchainPass() inside onRender, and all sg_begin/end_pass +
    // commit + present (DirectRenderHost) or encode + transmit (ServerWireBridge)
    // live inside that ge::Pass's lifetime, not here.
    virtual void refreshFrame() = 0;

    // True when the render subsystem has signaled shutdown (window close,
    // wire closed, SIGINT, etc.).
    virtual bool shouldQuit() const = 0;

    // True while the render path is suspended (Android: activity in
    // background, swap chain torn down). The engine's run loop must
    // skip beginFrame / onRender / bgfx::frame / endFrame while this
    // is true — bgfx::frame() against a dead Android swap chain crashes.
    // Default false; only DirectRenderHost on Android ever returns true.
    virtual bool paused() const { return false; }

    // Display-cutout-only insets in pt — drives Context::drawSafeRectInPts.
    // Default zeros for hosts with no chrome concept (desktop) or
    // pre-🎯T37-followup wire mode.
    virtual SafeAreaInsets drawSafeInsetsInPts() const { return {}; }
    // Full input-safe insets in pt (cutouts + gesture / tappable zones) —
    // drives Context::uiSafeRectInPts.
    virtual SafeAreaInsets uiSafeInsetsInPts() const { return {}; }

    // The session's live Context. Each host owns its Context — db
    // setup, device-class and dimensions are all host-specific — and
    // refreshes its per-frame fields inside beginFrame() so callbacks
    // observe current state through the same shared object. Run loops
    // stay out of Context construction.
    //
    // Valid once the host is ready — DirectRenderHost from
    // construction; ServerWireBridge after initialize() (which the
    // run loop calls when DeviceInfo arrives).
    virtual const Context& context() const = 0;
};

} // namespace ge
