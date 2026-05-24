// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// RefreshRateBoost — desktop no-op implementation (🎯T63).
//
// Desktop displays (macOS, Linux, Windows) don't VRR-throttle on idle —
// the GPU presents as fast as vsync allows regardless of touch activity.
// Nothing to do here.

#include <ge/RefreshRateBoost.h>

#include <atomic>

namespace ge {

struct RefreshRateBoost::M {
    std::atomic<int> count{0};
    std::atomic<bool> warnedUnderflow{false};
};

RefreshRateBoost::RefreshRateBoost() : m_(std::make_unique<M>()) {}
RefreshRateBoost::~RefreshRateBoost() = default;

void RefreshRateBoost::engagePress() {
    m_->count.fetch_add(1);
}

void RefreshRateBoost::releasePress() {
    if (m_->count.load() <= 0) {
        // Still track underflow for test correctness.
        m_->warnedUnderflow.store(true);
        return;
    }
    m_->count.fetch_sub(1);
}

void RefreshRateBoost::drainPresses() {
    m_->count.store(0);
}

int RefreshRateBoost::pressCount() const noexcept {
    return m_->count.load();
}

} // namespace ge
