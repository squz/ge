// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// RefreshRateBoost — Apple platform implementation (🎯T63).
//
// iOS/iPadOS: holds a CADisplayLink with a high preferredFrameRateRange
// (min=80, max=device max) while a press is in flight. Merely asserting
// a CADisplayLink at this range is enough to keep a ProMotion display in
// its high-refresh state — the display link fires independently of SDL's
// render loop so the OS sees a continuous high-refresh demand signal
// even if the game's own frame loop is capped. This closes the "first
// vsync gap" where iPadOS drops to 60 Hz after a period of inactivity
// and the button-highlight from a tap can't become visible for up to 16 ms.
//
// macOS: no-op. macOS displays don't VRR-throttle on idle, and
// CADisplayLink / UIScreen.maximumFramesPerSecond don't exist on macOS.
//
// iOS Simulator (🎯T91): also a no-op. The Simulator's CADisplayLink shim
// throws an Obj-C exception from -[CADisplayLink preferredFrameRateRange],
// aborting the app on the first mouse-button press routed through
// engagePress(). The boost is meaningless there anyway — the sim composites
// into a Mac window at the host's refresh, with no physical ProMotion display
// to hold high. So the real CADisplayLink path is gated to physical iOS
// devices (TARGET_OS_IOS && !TARGET_OS_SIMULATOR).

#include <ge/RefreshRateBoost.h>

#include <atomic>

#include <spdlog/spdlog.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#endif

namespace ge {

struct RefreshRateBoost::M {
    std::atomic<int> count{0};
    // Warn-once flag for underflow (spurious releasePress).
    std::atomic<bool> warnedUnderflow{false};

#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_SIMULATOR
    // CADisplayLink held while boost is engaged.
    // Access is serialised by the SDL event pump (single-threaded), so
    // no lock is needed around the Obj-C calls.
    CADisplayLink* displayLink = nil;

    void engageBoost() {
        if (displayLink) return;  // already engaged

        // Query the display's max refresh rate (e.g. 120 on ProMotion iPad).
        float maxFps = 60.0f;
        UIScreen* screen = UIScreen.mainScreen;
        if (screen) {
            maxFps = (float)screen.maximumFramesPerSecond;
        }

        // Create a display link that fires a no-op callback.
        // The mere existence of a CADisplayLink with a high
        // preferredFrameRateRange keeps the display in high-refresh mode;
        // we don't need to render in its callback.
        displayLink = [CADisplayLink displayLinkWithTarget:
            [NSObject new] selector:@selector(description)];

        // CAFrameRateRange: (minimum, maximum, preferred).
        // minimum=80 filters out any non-ProMotion device; on 60 Hz
        // hardware iOS ignores preferred and clamps to 60.
        if (@available(iOS 15.0, *)) {
            displayLink.preferredFrameRateRange =
                CAFrameRateRangeMake(80.0f, maxFps, maxFps);
        } else {
            displayLink.preferredFramesPerSecond = NSInteger(maxFps);
        }

        [displayLink addToRunLoop:NSRunLoop.mainRunLoop
                         forMode:NSRunLoopCommonModes];
        SPDLOG_DEBUG("RefreshRateBoost: engaged (maxFps={})", maxFps);
    }

    void releaseBoost() {
        if (!displayLink) return;
        [displayLink invalidate];
        displayLink = nil;
        SPDLOG_DEBUG("RefreshRateBoost: released");
    }
#else
    // macOS, iOS Simulator, and other Apple targets: no-op (see 🎯T91 note
    // at the top — the simulator throws on preferredFrameRateRange and has
    // no physical display to boost).
    void engageBoost() {}
    void releaseBoost() {}
#endif
};

RefreshRateBoost::RefreshRateBoost() : m_(std::make_unique<M>()) {}

RefreshRateBoost::~RefreshRateBoost() {
    drainPresses();
}

void RefreshRateBoost::engagePress() {
    int prev = m_->count.fetch_add(1);
    if (prev == 0) {
        m_->engageBoost();
    }
}

void RefreshRateBoost::releasePress() {
    int prev = m_->count.load();
    if (prev <= 0) {
        if (!m_->warnedUnderflow.exchange(true)) {
            SPDLOG_WARN("RefreshRateBoost: spurious releasePress (count already 0); "
                        "ignoring. Check SDL event pairing.");
        }
        return;
    }
    int next = m_->count.fetch_sub(1) - 1;
    if (next == 0) {
        m_->releaseBoost();
    }
}

void RefreshRateBoost::drainPresses() {
    int old = m_->count.exchange(0);
    if (old > 0) {
        m_->releaseBoost();
        SPDLOG_DEBUG("RefreshRateBoost: drain (was count={})", old);
    }
}

int RefreshRateBoost::pressCount() const noexcept {
    return m_->count.load();
}

} // namespace ge
