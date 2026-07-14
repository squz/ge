// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// AccelSynth — synthesize SDL_EVENT_SENSOR_UPDATE events from Shift-gated
// mouse motion when no real accelerometer is available (desktop, iOS
// simulator, Android emulator).
//
// iOS Simulator + GCMouse: motion deltas only arrive when relative mouse mode
// is on (SDL_uikitevents.m mouseMovedHandler gates on SDL_GCMouseRelativeMode).
// We enable relative mode on sim (and desktop); on *real* iOS devices synth is
// off (Core Motion) so relative mode is not needed.
// Absolute mouse / hover paths still work: if xrel/yrel are zero we fall back
// to position deltas while Shift is held.
//
// Belongs to the render subsystem. DirectRenderHost (in-process — desktop
// app, hidden-window server, sim/emu) is the sole owner: synthesis is an
// ENGINE concern, so it runs wherever the engine hosts the game. Players are
// dumb peripherals that forward raw Shift+drag over the wire; the server-side
// host's synth interprets it. Games only ever see SDL_EVENT_SENSOR_UPDATE
// events; whether they originate from real hardware or this synthesis is
// invisible to them.
//
// Tilt model: mouse displacement from its initial press-point is the
// tilt vector. Magnitude × a fixed radians-per-pixel scale gives the
// tilt angle; the axis of rotation is perpendicular to the displacement
// in the screen plane. One rotation about one axis — no gimballing.
// No cap on displacement: let the user "flip the device upside down"
// if they want, with the physics following sin(angle) naturally.
//
// GE_ACCELSYNTH_AUTODRIVE (iOS Simulator only):
// When this environment variable is set to "1", update() drives a
// synthetic 100-pixel X-axis tilt for the first 2 seconds, then
// releases into easing. This lets the matrix cell (ios-sim-tablet-dist)
// assert the AccelSynth path produces non-zero sensor output without
// requiring any mouse input — simctl cannot inject GCKeyboard Shift events.
// This path is compiled and active ONLY on TARGET_OS_SIMULATOR; it is
// unreachable on real devices.
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

// Radians of tilt per pixel of mouse displacement. Chosen so that ~300 px
// of displacement corresponds to ~45° tilt — a comfortable reach on a
// laptop trackpad / mouse.
constexpr float kTiltRadPerPixel = 0.0026f;  // ≈ π/4 / 300
constexpr float kG = 9.81f;

struct Tilt {
    float x = 0.f;  // mouse displacement from press-point, pixels
    float y = 0.f;
};

// Relative mouse for Shift-drag on hosts that synthesize tilt (desktop +
// iOS Simulator). Real iOS uses Core Motion and never needs this. Stream
// players call this when forwarding Shift so GCMouse deltas reach the wire.
inline void setRelativeMouseForShiftDrag(SDL_Window* window, bool shiftDown) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_SIMULATOR
    (void)window;
    (void)shiftDown;
#else
    SDL_Window* w = window ? window : SDL_GetMouseFocus();
    if (w) SDL_SetWindowRelativeMouseMode(w, shiftDown);
#endif
}

class AccelSynth {
public:
    AccelSynth() = default;

    void setWindow(SDL_Window* w) { window_ = w; }
    void setEmit(std::function<void(const SDL_Event&)> fn) { emit_ = std::move(fn); }

    // Current tilt vector (raw mouse displacement from the Shift-press
    // point, in pixels). Zero when Shift isn't held.
    Tilt current() const { return tilt_; }

    // Returns true if the event was consumed by the synthesis (caller
    // should NOT forward it). Returns false if the event passes through.
    bool handle(const SDL_Event& e) {
        if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP)
            && (e.key.scancode == SDL_SCANCODE_LSHIFT ||
                e.key.scancode == SDL_SCANCODE_RSHIFT)) {
            const bool newShift = (e.type == SDL_EVENT_KEY_DOWN);
            if (newShift != shiftDown_) {
                shiftDown_ = newShift;
                // Relative mouse: desktop (edge drag) + iOS Simulator (GCMouse
                // only reports deltas in relative mode). Real iOS uses Core
                // Motion and never constructs AccelSynth.
                setRelativeMouseForShiftDrag(window_, shiftDown_);
                if (shiftDown_) {
                    // Fresh capture — start from zero, no easing.
                    tilt_ = {};
                    easing_ = false;
                    haveLastAbs_ = false;
                } else {
                    // Released — start easing tilt back to zero.
                    easing_ = true;
                    lastTickNs_ = 0;  // first update() initialises clock
                    haveLastAbs_ = false;
                }
            }
            return true;  // consume Shift
        }

        if (e.type == SDL_EVENT_MOUSE_MOTION && shiftDown_) {
            float dx = e.motion.xrel;
            float dy = e.motion.yrel;
            // Absolute/hover paths (UIKit hover, button-drag) often report
            // zero xrel/yrel; derive deltas from window position.
            if (dx == 0.f && dy == 0.f) {
                if (haveLastAbs_) {
                    dx = e.motion.x - lastAbsX_;
                    dy = e.motion.y - lastAbsY_;
                }
                lastAbsX_ = e.motion.x;
                lastAbsY_ = e.motion.y;
                haveLastAbs_ = true;
            } else {
                haveLastAbs_ = false;  // relative stream is authoritative
            }
            if (dx != 0.f || dy != 0.f) {
                tilt_.x += dx;
                tilt_.y += dy;
                emitSensorFromTilt();
            }
            return true;  // consume motion-while-tilting
        }

        return false;
    }

    // Called once per frame by the host. Drives tilt easing back to
    // zero after Shift is released. No-op while Shift is held or after the
    // tilt has fully decayed.
    //
    // On TARGET_OS_SIMULATOR with GE_ACCELSYNTH_AUTODRIVE=1: drives a
    // synthetic 100-pixel X tilt for 2 s then triggers easing. The autodrive
    // state is initialized on the first update() call (checked once via
    // autodriveChecked_).
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
            const float elapsed = float(SDL_GetPerformanceCounter() - autodriveStartNs_)
                                  / float(freq);
            if (elapsed < 2.0f) {
                // Hold a 100 px X-axis tilt while in the drive window.
                tilt_.x = 100.f;
                tilt_.y = 0.f;
                shiftDown_ = true;
                emitSensorFromTilt();
                return;
            }
            // Drive window expired — release shift and start easing.
            autodriveActive_ = false;
            shiftDown_ = false;
            easing_ = true;
            lastTickNs_ = 0;
            SPDLOG_INFO("AccelSynth: GE_ACCELSYNTH_AUTODRIVE drive complete, easing");
        }
#endif  // TARGET_OS_SIMULATOR
        if (!easing_) return;
        const uint64_t now = SDL_GetPerformanceCounter();
        if (lastTickNs_ == 0) {
            lastTickNs_ = now;
            return;
        }
        const uint64_t freq = SDL_GetPerformanceFrequency();
        const float dt = float(now - lastTickNs_) / float(freq);
        lastTickNs_ = now;

        constexpr float kTau = 0.08f;  // 80 ms — quick ease
        const float decay = std::exp(-dt / kTau);
        tilt_.x *= decay;
        tilt_.y *= decay;
        if (std::sqrt(tilt_.x * tilt_.x + tilt_.y * tilt_.y) < 0.5f) {
            tilt_ = {};
            easing_ = false;
        }
        emitSensorFromTilt();
    }

    // True if a *usable* real accelerometer is available (synthesis should
    // NOT be active in that case). Caller should check this once at startup
    // and decide whether to wire AccelSynth in at all.
    //
    // iOS Simulator: Core Motion reports accelerometerAvailable=YES, but the
    // samples are not a gameplay substitute — always treat as no real sensor
    // so Shift-drag AccelSynth activates.
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
    void emitSensorFromTilt() {
        if (!emit_) return;
        // Emit in iOS SDL_SENSOR_ACCEL convention: values are the
        // counter-gravity vector in device frame. When the device is
        // tilted in some direction, gravity rotates that way in device
        // frame, and the reported accel rotates the OPPOSITE way.
        // Hence the sign-flip on tilt_.{x,y} below. tilt_.y is not
        // further inverted: SDL mouse-y grows downward, matching
        // iOS's +Y-points-toward-top convention (drag down → virtual
        // device leans forward → counter-gravity +Y component).
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
    bool shiftDown_ = false;
    bool easing_  = false;
    bool haveLastAbs_ = false;
    float lastAbsX_ = 0.f;
    float lastAbsY_ = 0.f;
    uint64_t lastTickNs_ = 0;
    SDL_Window* window_ = nullptr;
    std::function<void(const SDL_Event&)> emit_;
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
    bool autodriveChecked_ = false;
    bool autodriveActive_  = false;
    uint64_t autodriveStartNs_ = 0;
#endif
};

} // namespace ge
