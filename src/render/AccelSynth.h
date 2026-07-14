// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// AccelSynth — synthesize SDL_EVENT_SENSOR_UPDATE events from Shift-gated
// mouse motion when no real accelerometer is available (desktop, iOS
// simulator, Android emulator).
//
// iOS Simulator notes:
//  - Core Motion reports accelerometerAvailable=YES; we force
//    realSensorAvailable()=false so synth still arms.
//  - GCMouse only delivers relative deltas when relative mouse mode is on
//    (SDL_uikitevents.m). We enable it while tilt is armed.
//  - Hardware-keyboard Shift is unreliable on sim (Connect Hardware Keyboard
//    must be on). So on TARGET_OS_SIMULATOR we also arm while the primary
//    mouse button is held — click-drag alone tilts. Absolute position
//    deltas cover the UIKit button-drag path when xrel is zero.
//
// Desktop: Shift + drag (relative mode). Button alone does not tilt so the
// cursor stays free for UI.
//
// Belongs to the render subsystem. DirectRenderHost owns synthesis.
// Stream players forward raw Shift/drag; server-side synth interprets.
// Games only see SDL_EVENT_SENSOR_UPDATE.
//
// Tilt model: mouse displacement from the arm point is the tilt vector.
// Magnitude × kTiltRadPerPixel → angle; axis ⊥ displacement in screen plane.
//
// GE_ACCELSYNTH_AUTODRIVE=1 (iOS Simulator only): fixed 100 px X tilt for 2 s
// then ease — for matrix cells that cannot inject GCKeyboard Shift.
#pragma once

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cmath>
#include <cstdlib>
#include <functional>

namespace ge {

constexpr float kTiltRadPerPixel = 0.0026f;  // ≈ π/4 / 300
constexpr float kG = 9.81f;

struct Tilt {
    float x = 0.f;
    float y = 0.f;
};

// Relative mouse for Shift-drag on hosts that synthesize tilt (desktop +
// iOS Simulator). Real iOS uses Core Motion and never needs this.
inline void setRelativeMouseForShiftDrag(SDL_Window* window, bool armed) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_SIMULATOR
    (void)window;
    (void)armed;
#else
    SDL_Window* w = window ? window : SDL_GetMouseFocus();
    if (w) SDL_SetWindowRelativeMouseMode(w, armed);
#endif
}

class AccelSynth {
public:
    AccelSynth() = default;

    void setWindow(SDL_Window* w) { window_ = w; }
    void setEmit(std::function<void(const SDL_Event&)> fn) { emit_ = std::move(fn); }

    Tilt current() const { return tilt_; }

    // true = consumed (do not forward to the game as raw mouse/key).
    bool handle(const SDL_Event& e) {
        // ── Modifier / button arm ─────────────────────────────────
        if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) &&
            isShiftKey(e)) {
            const bool down = (e.type == SDL_EVENT_KEY_DOWN);
            if (down != shiftKey_) {
                shiftKey_ = down;
                onArmChanged();
            }
            return true;
        }

        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                const bool down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                if (down != primaryDown_) {
                    primaryDown_ = down;
                    onArmChanged();
                }
            }
            // Do not consume button events — games may still want them.
            // On sim, button arms tilt but still reaches the game as a no-op.
            return false;
        }

        if (e.type == SDL_EVENT_MOUSE_MOTION && armed()) {
            float dx = e.motion.xrel;
            float dy = e.motion.yrel;
            if (dx == 0.f && dy == 0.f) {
                if (haveLastAbs_) {
                    dx = e.motion.x - lastAbsX_;
                    dy = e.motion.y - lastAbsY_;
                }
                lastAbsX_ = e.motion.x;
                lastAbsY_ = e.motion.y;
                haveLastAbs_ = true;
            } else {
                haveLastAbs_ = false;
            }
            if (dx != 0.f || dy != 0.f) {
                tilt_.x += dx;
                tilt_.y += dy;
                emitSensorFromTilt();
            }
            return true;
        }

        return false;
    }

    void update() {
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
        if (!autodriveChecked_) {
            autodriveChecked_ = true;
            const char* env = std::getenv("GE_ACCELSYNTH_AUTODRIVE");
            if (env && env[0] == '1') {
                autodriveActive_ = true;
                autodriveStartNs_ = SDL_GetPerformanceCounter();
                SPDLOG_INFO("AccelSynth: GE_ACCELSYNTH_AUTODRIVE active");
            }
        }
        if (autodriveActive_) {
            const uint64_t freq = SDL_GetPerformanceFrequency();
            const float elapsed =
                float(SDL_GetPerformanceCounter() - autodriveStartNs_) /
                float(freq);
            if (elapsed < 2.0f) {
                tilt_.x = 100.f;
                tilt_.y = 0.f;
                emitSensorFromTilt();
                return;
            }
            autodriveActive_ = false;
            easing_ = true;
            lastTickNs_ = 0;
            SPDLOG_INFO("AccelSynth: GE_ACCELSYNTH_AUTODRIVE drive complete, easing");
        }
#endif
        if (!easing_) return;
        const uint64_t now = SDL_GetPerformanceCounter();
        if (lastTickNs_ == 0) {
            lastTickNs_ = now;
            return;
        }
        const uint64_t freq = SDL_GetPerformanceFrequency();
        const float dt = float(now - lastTickNs_) / float(freq);
        lastTickNs_ = now;

        constexpr float kTau = 0.08f;
        const float decay = std::exp(-dt / kTau);
        tilt_.x *= decay;
        tilt_.y *= decay;
        if (std::sqrt(tilt_.x * tilt_.x + tilt_.y * tilt_.y) < 0.5f) {
            tilt_ = {};
            easing_ = false;
        }
        emitSensorFromTilt();
    }

    // Usable real accelerometer? iOS Simulator always false.
    static bool realSensorAvailable() {
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
        return false;
#else
        int count = 0;
        SDL_SensorID* ids = SDL_GetSensors(&count);
        if (ids) SDL_free(ids);
        return count > 0;
#endif
    }

private:
    static bool isShiftKey(const SDL_Event& e) {
        if (e.key.scancode == SDL_SCANCODE_LSHIFT ||
            e.key.scancode == SDL_SCANCODE_RSHIFT)
            return true;
        // Keycode path (some hosts fill key, not only scancode).
        if (e.key.key == SDLK_LSHIFT || e.key.key == SDLK_RSHIFT)
            return true;
        return false;
    }

    // Armed when Shift is held (key event or current mod state).
    // On iOS Simulator also when primary button is held (click-drag).
    bool armed() const {
        if (shiftKey_) return true;
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0) return true;
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
        if (primaryDown_) return true;
#endif
        return false;
    }

    void onArmChanged() {
        const bool now = armed();
        setRelativeMouseForShiftDrag(window_, now);
        if (now) {
            // Fresh capture each time we re-arm.
            if (!wasArmed_) {
                tilt_ = {};
                easing_ = false;
                haveLastAbs_ = false;
            }
        } else {
            easing_ = true;
            lastTickNs_ = 0;
            haveLastAbs_ = false;
        }
        wasArmed_ = now;
    }

    void emitSensorFromTilt() {
        if (!emit_) return;
        float mag = std::sqrt(tilt_.x * tilt_.x + tilt_.y * tilt_.y);
        float gx = 0.f, gy = 0.f;
        if (mag > 0.f) {
            float angle = mag * kTiltRadPerPixel;
            float s = std::sin(angle);
            gx = -kG * s * tilt_.x / mag;
            gy =  kG * s * tilt_.y / mag;
        }
        SDL_Event se{};
        se.type = SDL_EVENT_SENSOR_UPDATE;
        se.sensor.data[0] = gx;
        se.sensor.data[1] = gy;
        emit_(se);
    }

    Tilt tilt_;
    bool shiftKey_ = false;
    bool primaryDown_ = false;
    bool wasArmed_ = false;
    bool easing_ = false;
    bool haveLastAbs_ = false;
    float lastAbsX_ = 0.f;
    float lastAbsY_ = 0.f;
    uint64_t lastTickNs_ = 0;
    SDL_Window* window_ = nullptr;
    std::function<void(const SDL_Event&)> emit_;
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
    bool autodriveChecked_ = false;
    bool autodriveActive_ = false;
    uint64_t autodriveStartNs_ = 0;
#endif
};

} // namespace ge
