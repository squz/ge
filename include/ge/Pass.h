// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::Pass (🎯T101) — a move-only RAII render pass.
//
// Construct to begin a render pass; destroy to end it. sokol allows only one
// pass active at a time, so Pass is meant to be stack-scoped:
//
//     {
//         ge::Pass off = ge::offscreenPass(myOffscreenDesc);   // sg_begin_pass
//         // draws into the offscreen target
//     }                                                        // sg_end_pass
//
//     auto p = ctx.swapchainPass();   // acquires drawable + opens swapchain pass
//     // composite / final draws
//     // p's destructor ends the pass, commits, and presents
//
// The swapchain variant is obtained from ge::Context::swapchainPass() (built by
// the render host so the platform-specific drawable / commit / present logic
// stays in the host, not duplicated here). Exactly one swapchain Pass per frame.
//
// This header names no rendering backend in its *includes* — `sg_pass` is only
// forward-declared, so SessionHost.h (which returns a Pass from
// Context::swapchainPass) doesn't drag sokol_gfx.h across the public surface.
// Building an offscreen `sg_pass` descriptor needs sokol, so callers of
// offscreenPass() include sokol_gfx.h themselves (they already do — they own
// the offscreen attachments).
#pragma once

#include <functional>
#include <memory>

struct sg_pass;  // sokol_gfx.h; full definition only where a desc is built

namespace ge {

class Pass {
public:
    ~Pass();
    Pass(Pass&&) noexcept;
    Pass& operator=(Pass&&) noexcept;
    Pass(const Pass&)            = delete;
    Pass& operator=(const Pass&) = delete;

    // Internal: hosts and the offscreen factory build a Pass from its impl.
    // Not part of the consumer-facing surface (the impl type is opaque).
    struct M;
    explicit Pass(std::unique_ptr<M> impl) noexcept;

private:
    std::unique_ptr<M> m_;  // nullptr when moved-from → dtor is a no-op
};

// Offscreen render pass: begins on construction (sg_begin_pass(&desc)), ends on
// destruction (sg_end_pass). The caller owns the target attachments described
// by `desc`. Must not overlap another live Pass.
Pass offscreenPass(const struct sg_pass& desc);

// Build a Pass that runs `onEnd` exactly once when it (or the value it moves
// into) is destroyed. The render hosts use this for the swapchain Pass:
// `onEnd` wraps their platform end-pass + commit + present sequence. The pass
// is assumed already begun by the caller before this returns.
Pass makePass(std::function<void()> onEnd);

} // namespace ge
