#pragma once

#include <ge/Linalg.h>

#include <linalg.h>
#include <chrono>
#include <cmath>
#include <limits>

using namespace linalg::aliases;

// Damped 3D rotation with inertia.
// Stores orientation as a quaternion and angular velocity as a 3D vector.
class DampedRotation {
public:
    constexpr DampedRotation(float damping = 0.90f) : damping_(damping) {}

    constexpr const float4& orientation() const { return orientation_; }
    constexpr const float3& angularVelocity() const { return angularVelocity_; }

    // EAGLView.mm touchesEnded: samples older than this are the start of the
    // release-velocity span, not part of the "still moving" tail.
    static constexpr double releaseWindowSec() { return kReleaseWindowSec; }

    constexpr void setOrientation(const float4& q) { orientation_ = q; }
    constexpr void setAngularVelocity(const float3& v) {
        angularVelocity_ = v;
        // A new velocity is not a continuation of the previous drag's samples.
        sampleCount_ = 0;
        sampleNext_ = 0;
        haveTime_ = false;
        lastTime_ = 0;
        dragPos_ = float3{0.0f, 0.0f, 0.0f};
    }
    constexpr void setDamping(float d) { damping_ = d; }

    // Apply incremental rotation (during drag)
    void rotate(const float3& axis, float angle) {
        if (angle == 0.0f) return;
        float4 delta = linalg::rotation_quat(axis, angle);
        orientation_ = linalg::qmul(delta, orientation_);
        orientation_ = linalg::normalize(orientation_);
    }

    // Apply rotation from a screen-space drag delta, in the consumer's camera
    // basis (🎯T122/T123). Horizontal drag rotates about the screen-up axis;
    // vertical drag about the screen-right axis. Both are world-space unit
    // vectors from the consumer's camera (view) basis. The defaults are the
    // Z-up convention (tiltbuggy: camera at (0,-3,0), up +Z) so existing callers
    // are unaffected; a Y-up consumer (esfera: up +Y) passes
    // screenUp = {0,1,0}, screenRight = {1,0,0}.
    void applyDrag(float dx, float dy, float sensitivity = 1.0f,
                   float3 screenUp = {0.0f, 0.0f, 1.0f},
                   float3 screenRight = {1.0f, 0.0f, 0.0f}) {
        if (dx != 0.0f) {
            rotate(screenUp, dx * sensitivity);
        }
        if (dy != 0.0f) {
            rotate(screenRight, dy * sensitivity);
        }
    }

    // Update angular velocity from drag speed.
    //
    // Same camera basis as applyDrag: horizontal speed spins about screenUp,
    // vertical about screenRight. Defaults to the Z-up convention.
    //
    // Release speed follows the iOS original, not an EMA. EAGLView.mm keeps a
    // 10-deep ring of touch samples (handleTouchMovement, the points/times
    // stores around line 1620) and on finger-up walks at most nine samples
    // back from the newest, stopping at the first sample more than 0.02 s
    // older (touchesEnded, the gs_dragging branch around line 1779). Velocity
    // is that displacement divided by the elapsed time. A stationary hold
    // therefore adds no displacement over a long span and cannot revive the
    // flick from before the hold.
    //
    // The original averages adjacent raw touch points (EAGLView.mm around
    // line 1624) before that walk. This API receives one speed per frame, not
    // raw touches, so that average is not applied. `timeSec` is the sample
    // clock in seconds (monotonic). Pass a negative value or NaN to stamp the
    // sample with steady_clock instead.
    void updateVelocityFromDrag(float dx, float dy, float sensitivity = 1.0f,
                                float3 screenUp = {0.0f, 0.0f, 1.0f},
                                float3 screenRight = {1.0f, 0.0f, 0.0f},
                                double timeSec = std::numeric_limits<double>::quiet_NaN()) {
        const float3 instant = screenUp * (dx * sensitivity) + screenRight * (dy * sensitivity);
        double t = timeSec;
        if (!(t >= 0.0)) {
            t = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
        }
        if (haveTime_) {
            const double gap = t - lastTime_;
            if (gap > 0.0)
                dragPos_ += instant * static_cast<float>(gap);
        }
        lastTime_ = t;
        haveTime_ = true;

        samples_[sampleNext_] = DragSample{t, dragPos_};
        sampleNext_ = (sampleNext_ + 1) % kDragSampleRing;
        if (sampleCount_ < kDragSampleRing) ++sampleCount_;

        const int newest = (sampleNext_ + kDragSampleRing - 1) % kDragSampleRing;
        int older = newest;
        int idx = newest;
        const int available = sampleCount_ - 1;
        const int limit = available < (kDragSampleRing - 1) ? available : (kDragSampleRing - 1);
        for (int n = 0; n < limit; ++n) {
            const int prev = (idx + kDragSampleRing - 1) % kDragSampleRing;
            older = prev;
            // Strictly greater than 0.02 s, matching EAGLView.mm.
            if (samples_[newest].t - samples_[prev].t > kReleaseWindowSec)
                break;
            idx = prev;
        }
        const double span = samples_[newest].t - samples_[older].t;
        if (!(span > 1e-8)) {
            // One sample, or a second stamp at the same instant. Do not
            // replace a flick with a zero-speed duplicate.
            if (sampleCount_ == 1)
                angularVelocity_ = instant;
            return;
        }
        angularVelocity_ = (samples_[newest].pos - samples_[older].pos) /
                           static_cast<float>(span);
    }

    // Advance simulation by dt seconds
    void update(float dt) {
        // Apply angular velocity as rotation
        float speed = linalg::length(angularVelocity_);
        if (speed > 1e-6f) {
            float3 axis = angularVelocity_ / speed;
            rotate(axis, speed * dt);

            // Exponential decay (framerate-independent)
            angularVelocity_ *= std::pow(damping_, 60.0f * dt);

            // Stop when velocity is negligible
            if (linalg::length(angularVelocity_) < 1e-5f) {
                angularVelocity_ = float3{0.0f, 0.0f, 0.0f};
            }
        }
    }

    bool isMoving() const {
        return linalg::length(angularVelocity_) > 1e-6f;
    }

    // Get rotation matrix for rendering
    float4x4 matrix() const {
        return linalg::rotation_matrix(orientation_);
    }

private:
    // EAGLView.mm ring depth and release window. See updateVelocityFromDrag.
    static constexpr int kDragSampleRing = 10;
    static constexpr double kReleaseWindowSec = 0.02;

    struct DragSample {
        double t = 0;
        float3 pos{};
    };

    float4 orientation_ = float4{0.0f, 0.0f, 0.0f, 1.0f};  // Identity quaternion
    float3 angularVelocity_ = float3{0.0f, 0.0f, 0.0f};     // Axis * angular speed (rad/s)
    float damping_;

    DragSample samples_[kDragSampleRing]{};
    int sampleCount_ = 0;
    int sampleNext_ = 0;
    bool haveTime_ = false;
    double lastTime_ = 0;
    float3 dragPos_{};  // integrated drag angle; release speed is Δpos/Δt
};
