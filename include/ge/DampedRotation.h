#pragma once

#include <ge/Linalg.h>

#include <linalg.h>
#include <cmath>

using namespace linalg::aliases;

// Damped 3D rotation with inertia.
// Stores orientation as a quaternion and angular velocity as a 3D vector.
class DampedRotation {
public:
    constexpr DampedRotation(float damping = 0.90f) : damping_(damping) {}

    constexpr const float4& orientation() const { return orientation_; }
    constexpr const float3& angularVelocity() const { return angularVelocity_; }

    constexpr void setOrientation(const float4& q) { orientation_ = q; }
    constexpr void setAngularVelocity(const float3& v) { angularVelocity_ = v; }
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

    // Update angular velocity from drag speed (smoothed for inertia on release).
    // Same camera basis as applyDrag: horizontal speed spins about screenUp,
    // vertical about screenRight. Defaults to the Z-up convention.
    void updateVelocityFromDrag(float dx, float dy, float sensitivity = 1.0f,
                                float3 screenUp = {0.0f, 0.0f, 1.0f},
                                float3 screenRight = {1.0f, 0.0f, 0.0f}) {
        float3 instantVel = screenUp * (dx * sensitivity) + screenRight * (dy * sensitivity);
        // Exponential moving average to smooth out jitter on release
        constexpr float smoothing = 0.3f;
        angularVelocity_ = linalg::lerp(angularVelocity_, instantVel, smoothing);
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
    float4 orientation_ = float4{0.0f, 0.0f, 0.0f, 1.0f};  // Identity quaternion
    float3 angularVelocity_ = float3{0.0f, 0.0f, 0.0f};     // Axis * angular speed (rad/s)
    float damping_;
};
