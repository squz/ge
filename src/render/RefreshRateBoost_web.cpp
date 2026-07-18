// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T157: RefreshRateBoost for the web build — press counting only, no
// platform boost. The browser owns the display cadence (requestAnimationFrame
// follows the compositor; there is no page-accessible VRR / refresh-rate
// request), so engage/release keep the shared counter semantics and the
// boost itself is a no-op, like desktop.

#if defined(__EMSCRIPTEN__)

#include <ge/RefreshRateBoost.h>

#include <spdlog/spdlog.h>

namespace ge {

struct RefreshRateBoost::M {
    int  count = 0;
    bool warnedUnderflow = false;
};

RefreshRateBoost::RefreshRateBoost() : m_(new M) {}
RefreshRateBoost::~RefreshRateBoost() = default;

void RefreshRateBoost::engagePress() {
    ++m_->count;
}

void RefreshRateBoost::releasePress() {
    if (m_->count == 0) {
        if (!m_->warnedUnderflow) {
            m_->warnedUnderflow = true;
            SPDLOG_WARN("RefreshRateBoost: spurious releasePress (count already 0); "
                        "ignoring. Check SDL event pairing.");
        }
        return;
    }
    --m_->count;
}

void RefreshRateBoost::drainPresses() {
    m_->count = 0;
}

int RefreshRateBoost::pressCount() const noexcept {
    return m_->count;
}

} // namespace ge

#endif // __EMSCRIPTEN__
