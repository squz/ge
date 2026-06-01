// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Engine-internal screenshot bridge for the app-channel (🎯T92.6).
//
// The framebuffer readback must run on the game thread, inside the render
// host's endFrame after the GPU has rendered the frame (the swapchain
// drawable / default FBO is only valid then). The app-channel's screenshot_app
// handler runs on the channel worker thread, so it can't read the framebuffer
// directly. captureFrameRGBA bridges the two: it arms a one-shot capture and
// blocks until DirectRenderHost::endFrame services it on the game thread.

#pragma once

#include <cstdint>
#include <vector>

namespace ge::detail {

// Capture the next presented frame as RGBA8, top-down, tightly packed.
// Called from the app-channel worker thread; blocks until the render loop
// captures the frame (or ~2 s timeout). Returns false — and leaves `rgba`
// untouched — if nothing was captured (timeout, no render host, or the app is
// backgrounded so endFrame isn't running). Single in-flight request at a time.
bool captureFrameRGBA(std::vector<std::uint8_t>& rgba, int& w, int& h);

// True while a capture is armed. DirectRenderHost::endFrame polls this to
// decide whether to arm SokolContext::captureNextFrame for the current frame.
bool screenshotArmed();

// Deliver captured pixels from the SokolContext capture sink (game thread).
void deliverScreenshot(const std::uint8_t* rgba, int w, int h);

} // namespace ge::detail
