// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// RefreshRateBoost — Android platform implementation (🎯T63).
//
// Uses GeActivity.setFrameRateBoost(boolean) via JNI which calls
// Surface.setFrameRate(maxRefreshRate, FRAME_RATE_COMPATIBILITY_DEFAULT)
// on API 30+. On older API levels the method is a no-op (guarded by
// Build.VERSION.SDK_INT check in Java).
//
// JNI hop is synchronous but cheap: the method only changes a platform
// hint, not a surface property that triggers a compose cycle.

#include <ge/RefreshRateBoost.h>

#include <atomic>

#include <spdlog/spdlog.h>

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL.h>
#endif

namespace ge {

struct RefreshRateBoost::M {
    std::atomic<int> count{0};
    std::atomic<bool> warnedUnderflow{false};

#if defined(__ANDROID__)
    void callJavaBoost(bool active) {
        // SDL3 provides SDL_GetAndroidJNIEnv() to get the JNIEnv for the
        // current thread, and SDL_GetAndroidActivity() to get the Activity
        // jobject. Both are available after SDL_Init.
        JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
        jobject activity = (jobject)SDL_GetAndroidActivity();
        if (!env || !activity) {
            SPDLOG_WARN("RefreshRateBoost: no JNIEnv/Activity available");
            return;
        }
        jclass cls = env->GetObjectClass(activity);
        jmethodID mid = env->GetMethodID(cls, "setFrameRateBoost", "(Z)V");
        if (!mid) {
            // GeActivity may be an older build; log and degrade silently.
            env->ExceptionClear();
            SPDLOG_WARN("RefreshRateBoost: GeActivity.setFrameRateBoost not found");
        } else {
            env->CallVoidMethod(activity, mid, active ? JNI_TRUE : JNI_FALSE);
        }
        env->DeleteLocalRef(cls);
    }
#else
    void callJavaBoost(bool) {}
#endif
};

RefreshRateBoost::RefreshRateBoost() : m_(std::make_unique<M>()) {}

RefreshRateBoost::~RefreshRateBoost() {
    drainPresses();
}

void RefreshRateBoost::engagePress() {
    int prev = m_->count.fetch_add(1);
    if (prev == 0) {
        m_->callJavaBoost(true);
        SPDLOG_DEBUG("RefreshRateBoost: engaged");
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
        m_->callJavaBoost(false);
        SPDLOG_DEBUG("RefreshRateBoost: released");
    }
}

void RefreshRateBoost::drainPresses() {
    int old = m_->count.exchange(0);
    if (old > 0) {
        m_->callJavaBoost(false);
        SPDLOG_DEBUG("RefreshRateBoost: drain (was count={})", old);
    }
}

int RefreshRateBoost::pressCount() const noexcept {
    return m_->count.load();
}

} // namespace ge
