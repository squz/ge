// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::Pass (🎯T101) implementation.
//
// The impl is just a teardown closure. Putting onEnd in ~M means every RAII
// and move operation falls out of std::unique_ptr for free: ~Pass ends the
// pass, move-assignment ends the current pass before adopting the new one, and
// a moved-from Pass holds a null impl whose teardown never runs.
//
// Offscreen passes are pure sokol (sg_begin_pass / sg_end_pass). The swapchain
// variant is built by the render host (see DirectRenderHost / ServerWireBridge)
// which supplies an onEnd closure wrapping its platform-specific
// end-pass + commit + present (or encode + transmit) sequence — so that
// delicate code lives in one place and isn't duplicated here.

#include <ge/Pass.h>

#include "sokol_gfx.h"

#include <functional>

namespace ge {

struct Pass::M {
    std::function<void()> onEnd;
    ~M() { if (onEnd) onEnd(); }
};

Pass::Pass(std::unique_ptr<M> impl) noexcept : m_(std::move(impl)) {}
Pass::~Pass()                                 = default;
Pass::Pass(Pass&&) noexcept                   = default;
Pass& Pass::operator=(Pass&&) noexcept        = default;

Pass makePass(std::function<void()> onEnd) {
    auto m   = std::make_unique<Pass::M>();
    m->onEnd = std::move(onEnd);
    return Pass{std::move(m)};
}

Pass offscreenPass(const sg_pass& desc) {
    sg_begin_pass(&desc);
    return makePass([] { sg_end_pass(); });
}

} // namespace ge
