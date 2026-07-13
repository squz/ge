// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Android orientation lock for the native player (and any GE_ANDROID
// consumer that links this TU). Mirrors tools/player_orientation_ios.mm
// / 🎯T36: SessionConfig.orientation must force the Activity orientation
// so a landscape game is not letterboxed into a portrait tablet.
//
// Regression (2026-07-11): Pixel Tablet streamed tiltbuggy (AnyLandscape)
// but stayed portrait — CMake linked player_orientation_stub.cpp, which
// no-ops playerForceOrientation. Fixed by this file + GeActivity.forceOrientation.

#include "player_orientation.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <jni.h>

// Sentinel matching ge::wire::kOrientationAnyLandscape (Protocol.h).
static constexpr uint8_t kLockAnyLandscape = 0xFE;

void playerForceOrientation(uint8_t orientation) {
    if (orientation == 0) return;

    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) {
        SPDLOG_WARN("playerForceOrientation: no JNI env");
        return;
    }

    jclass cls = env->FindClass("com/squz/player/GeActivity");
    if (!cls) {
        env->ExceptionClear();
        SPDLOG_WARN("playerForceOrientation: GeActivity class not found");
        return;
    }

    jmethodID mid = env->GetStaticMethodID(cls, "forceOrientation", "(I)V");
    if (!mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        SPDLOG_WARN("playerForceOrientation: forceOrientation method missing");
        return;
    }

    env->CallStaticVoidMethod(cls, mid, static_cast<jint>(orientation));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SPDLOG_WARN("playerForceOrientation: JNI call failed");
    } else if (orientation == kLockAnyLandscape) {
        SPDLOG_INFO("playerForceOrientation: requested lock=AnyLandscape");
    } else {
        SPDLOG_INFO("playerForceOrientation: requested lock={}", static_cast<int>(orientation));
    }
    env->DeleteLocalRef(cls);
}

int playerGetPhysicalOrientation() {
    // SDL reports the current display orientation once the Activity has
    // rotated; good enough for hit-test remapping until we need sensor pose.
    const SDL_DisplayOrientation o =
        SDL_GetCurrentDisplayOrientation(SDL_GetPrimaryDisplay());
    return static_cast<int>(o);
}
