#pragma once

#include <ge/Linalg.h>

#include <ge/DampedRotation.h>
#include <SDL3/SDL_events.h>
#include <chrono>
#include <cmath>
#include <functional>

namespace ge {

// Manages a spinnable globe: drag-to-rotate with inertia, and pinch-to-zoom.
// Handles mouse and single/two-finger touch input.
// All event coordinates are expected in pixels (the engine normalizes them before dispatch).
class GlobeController {
public:
    GlobeController(float damping = 0.90f, float sensitivity = 0.01f)
        : rotation_(damping), sensitivity_(sensitivity) {}

    // Feed an SDL event. Returns true if the event was handled.
    bool event(const SDL_Event& e) {
        switch (e.type) {
            case SDL_EVENT_FINGER_DOWN: {
                if (pinching_) return false;  // Ignore >2 fingers
                if (inputSource_ == InputSource::Finger) {
                    // Second finger: enter pinch mode, stop single-finger drag
                    pinching_ = true;
                    finger2Id_ = e.tfinger.fingerID;
                    finger2X_ = e.tfinger.x;
                    finger2Y_ = e.tfinger.y;
                    float dx = finger2X_ - finger1X_;
                    float dy = finger2Y_ - finger1Y_;
                    lastPinchDist_ = std::sqrt(dx*dx + dy*dy);
                    lastPinchAngle_ = std::atan2(dy, dx);
                    pinchDelta_ = 0.0f;
                    accumX_ = accumY_ = 0.0f;
                    return true;
                }
                if (inputSource_ != InputSource::None) return false;
                inputSource_ = InputSource::Finger;
                dragging_ = true;
                thresholdReached_ = false;
                finger1Id_ = e.tfinger.fingerID;
                finger1X_ = lastX_ = startX_ = e.tfinger.x;
                finger1Y_ = lastY_ = startY_ = e.tfinger.y;
                accumX_ = accumY_ = 0.0f;
                rotation_.setAngularVelocity({0, 0, 0});
                resetDragClock();
                return true;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                if (inputSource_ != InputSource::None) return false;
                inputSource_ = InputSource::Mouse;
                dragging_ = true;
                thresholdReached_ = false;
                lastX_ = startX_ = e.button.x;
                lastY_ = startY_ = e.button.y;
                accumX_ = accumY_ = 0.0f;
                rotation_.setAngularVelocity({0, 0, 0});
                resetDragClock();
                return true;
            }

            case SDL_EVENT_FINGER_MOTION: {
                if (pinching_) {
                    // Update whichever finger moved and accumulate log-scale pinch delta
                    if (e.tfinger.fingerID == finger1Id_)
                        finger1X_ = e.tfinger.x, finger1Y_ = e.tfinger.y;
                    else if (e.tfinger.fingerID == finger2Id_)
                        finger2X_ = e.tfinger.x, finger2Y_ = e.tfinger.y;
                    else
                        return false;
                    float dx = finger2X_ - finger1X_;
                    float dy = finger2Y_ - finger1Y_;
                    float newDist = std::sqrt(dx*dx + dy*dy);
                    float newAngle = std::atan2(dy, dx);
                    if (lastPinchDist_ > 0.1f)
                        pinchDelta_ += std::log(newDist / lastPinchDist_);
                    // Two-finger twist rotates about the camera view axis
                    // (🎯T122/T123), supplied via setCameraBasis; the default is
                    // the Z-up convention's view axis {0,1,0} (tiltbuggy).
                    float angleDiff = newAngle - lastPinchAngle_;
                    if (angleDiff > float(M_PI)) angleDiff -= 2.0f * float(M_PI);
                    if (angleDiff < -float(M_PI)) angleDiff += 2.0f * float(M_PI);
                    rotation_.rotate(viewAxis_, angleDiff);
                    lastPinchDist_ = newDist;
                    lastPinchAngle_ = newAngle;
                    return true;
                }
                if (inputSource_ != InputSource::Finger) return false;
                if (e.tfinger.fingerID != finger1Id_) return false;
                if (!thresholdReached_) {
                    float cdx = e.tfinger.x - startX_;
                    float cdy = e.tfinger.y - startY_;
                    if (cdx*cdx + cdy*cdy < kDragThreshold * kDragThreshold) {
                        lastX_ = e.tfinger.x;
                        lastY_ = e.tfinger.y;
                        return true;
                    }
                    thresholdReached_ = true;
                }
                float dx = e.tfinger.x - lastX_;
                float dy = e.tfinger.y - lastY_;
                rotation_.applyDrag(dx, dy, sensitivity_, screenUp_, screenRight_);
                accumX_ += dx;
                accumY_ += dy;
                lastX_ = e.tfinger.x;
                lastY_ = e.tfinger.y;
                return true;
            }

            case SDL_EVENT_MOUSE_MOTION: {
                if (inputSource_ != InputSource::Mouse) return false;
                if (!thresholdReached_) {
                    float cdx = e.motion.x - startX_;
                    float cdy = e.motion.y - startY_;
                    if (cdx*cdx + cdy*cdy < kDragThreshold * kDragThreshold) {
                        lastX_ = e.motion.x;
                        lastY_ = e.motion.y;
                        return true;
                    }
                    thresholdReached_ = true;
                }
                float dx = e.motion.x - lastX_;
                float dy = e.motion.y - lastY_;
                rotation_.applyDrag(dx, dy, sensitivity_, screenUp_, screenRight_);
                accumX_ += dx;
                accumY_ += dy;
                lastX_ = e.motion.x;
                lastY_ = e.motion.y;
                return true;
            }

            case SDL_EVENT_FINGER_UP: {
                if (pinching_) {
                    pinching_ = false;
                    if (e.tfinger.fingerID == finger1Id_) {
                        // Finger 1 lifted: promote finger 2 to single-finger drag
                        finger1Id_ = finger2Id_;
                        finger1X_ = lastX_ = startX_ = finger2X_;
                        finger1Y_ = lastY_ = startY_ = finger2Y_;
                    } else {
                        // Finger 2 lifted: stay with finger 1
                        lastX_ = startX_ = finger1X_;
                        lastY_ = startY_ = finger1Y_;
                    }
                    thresholdReached_ = true;  // Already past threshold from pinch
                    return true;
                }
                if (inputSource_ != InputSource::Finger) return false;
                if (e.tfinger.fingerID != finger1Id_) return false;
                closeDragSample();
                dragging_ = false;
                inputSource_ = InputSource::None;
                return true;
            }

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (inputSource_ != InputSource::Mouse) return false;
                closeDragSample();
                dragging_ = false;
                inputSource_ = InputSource::None;
                return true;

            default:
                return false;
        }
    }

    // Call once per frame. Flushes accumulated drag to velocity, applies inertia.
    void update(float dt) {
        if (dragging_ && dt > 0.001f) {
            // One sample per frame, including a stationary frame. The release
            // window in DampedRotation then sees the hold instead of the
            // pre-hold flick. See closeDragSample for a hold that never ticked.
            advanceDragClock(dt);
            const float invDt = 1.0f / dt;
            rotation_.updateVelocityFromDrag(accumX_ * invDt, accumY_ * invDt, sensitivity_,
                                              screenUp_, screenRight_, dragTime_);
            accumX_ = accumY_ = 0.0f;
        }
        if (!dragging_) {
            rotation_.update(dt);
        }
        // 🎯T131.3 Spinning the globe is a level activity (a drag, then inertia
        // decaying to rest). Under render-on-demand keep the loop awake while the
        // globe is dragging or coasting, then let it idle once velocity decays —
        // the consumer wires onRedraw once instead of forcing continuous mode.
        if (onRedraw && (dragging_ || rotation_.isMoving())) onRedraw();
    }

    // 🎯T131.3 Redraw sink for render-on-demand — see update(). Wire once,
    // capturing the Context by value: globe.onRedraw = [ctx]{ ctx.requestRedraw(); };
    std::function<void()> onRedraw;

    DampedRotation& rotation() { return rotation_; }
    const DampedRotation& rotation() const { return rotation_; }
    bool dragging() const { return dragging_; }
    bool pinching() const { return pinching_; }
    void setSensitivity(float s) { sensitivity_ = s; }
    void setDamping(float d) { rotation_.setDamping(d); }

    // Set the consumer's camera (view) basis as world-space unit vectors
    // (🎯T122/T123). Horizontal drag rotates about screenUp, vertical about
    // screenRight, two-finger twist about viewAxis. Defaults to the Z-up
    // convention (tiltbuggy: right={1,0,0}, up={0,0,1}, view={0,1,0}); a Y-up
    // consumer (esfera: eye (0,0,+d), up +Y, looking -Z) sets
    // right={1,0,0}, up={0,1,0}, view={0,0,-1}.
    void setCameraBasis(const float3& screenRight, const float3& screenUp, const float3& viewAxis) {
        screenRight_ = screenRight;
        screenUp_ = screenUp;
        viewAxis_ = viewAxis;
    }

    // Returns accumulated log-scale pinch delta since last call, then resets.
    // Positive = fingers spreading (zoom in), negative = fingers closing (zoom out).
    float consumePinchDelta() {
        float d = pinchDelta_;
        pinchDelta_ = 0.0f;
        return d;
    }

    // Host-supplied drag clock, in seconds. While installed, advanceDragClock
    // copies this value and does not read steady_clock, so a pause/step run
    // does not treat the wall gap between step commands as idle hold time.
    // The value is absolute simulation time and already includes the frame
    // dt the host is about to apply; the dt argument is ignored. Call
    // clearDragClock to restore the wall-clock idle-hold path used when the
    // frame loop is running in real time.
    void setDragClock(double simSeconds) {
        haveDragClock_ = true;
        dragClock_ = simSeconds;
    }
    void clearDragClock() { haveDragClock_ = false; }

    // Drop an in-progress drag, pinch, and the release-velocity sample ring
    // without changing angular velocity. A fixture reset calls this so the
    // next gesture does not inherit a half-finished flick.
    void releaseGesture() {
        dragging_ = false;
        pinching_ = false;
        inputSource_ = InputSource::None;
        thresholdReached_ = false;
        accumX_ = accumY_ = 0.0f;
        pinchDelta_ = 0.0f;
        resetDragClock();
        rotation_.setAngularVelocity(rotation_.angularVelocity());
    }


private:
    // Sample clock for DampedRotation's release window. Sim dt advances it
    // while the frame loop is ticking. A wall gap longer than the original's
    // 0.02 s release window (EAGLView.mm touchesEnded) is idle time the loop
    // slept through — render-on-demand does that on a stationary hold when
    // nothing requests a redraw — and has to count, or the pre-hold flick
    // is still the whole window at finger-up.
    void resetDragClock() {
        dragTime_ = 0;
        haveDragWall_ = false;
        // haveDragClock_ stays: the host clock is a mode, not a sample.
    }

    void advanceDragClock(float dt) {
        if (haveDragClock_) {
            // Absolute sim time from the host. Do not add dt again and do
            // not consult the wall clock.
            dragTime_ = dragClock_;
            return;
        }
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();
        double extra = 0;
        if (haveDragWall_) {
            const double wall = std::chrono::duration<double>(now - dragWall_).count();
            const double idle = wall - static_cast<double>(dt);
            if (idle > DampedRotation::releaseWindowSec())
                extra = idle;
        }
        dragWall_ = now;
        haveDragWall_ = true;
        dragTime_ += static_cast<double>(dt) + extra;
    }

    void closeDragSample() {
        advanceDragClock(0.0f);
        rotation_.updateVelocityFromDrag(0.0f, 0.0f, sensitivity_, screenUp_, screenRight_, dragTime_);
    }

    // Minimum displacement (pixels) before drag rotates the globe.
    // Prevents accidental micro-rotations from taps.
    static constexpr float kDragThreshold = 10.0f;

    enum class InputSource { None, Mouse, Finger };

    DampedRotation rotation_;
    float sensitivity_;

    // Camera (view) basis, world-space unit vectors. Default = Z-up convention
    // (🎯T122/T123); consumers with a different camera call setCameraBasis.
    float3 screenRight_ = {1.0f, 0.0f, 0.0f};
    float3 screenUp_    = {0.0f, 0.0f, 1.0f};
    float3 viewAxis_    = {0.0f, 1.0f, 0.0f};

    InputSource inputSource_ = InputSource::None;
    bool dragging_ = false;
    bool thresholdReached_ = false;
    float lastX_ = 0, lastY_ = 0;
    float startX_ = 0, startY_ = 0;
    float accumX_ = 0, accumY_ = 0;
    double dragTime_ = 0;
    bool haveDragWall_ = false;
    bool haveDragClock_ = false;
    double dragClock_ = 0;
    std::chrono::steady_clock::time_point dragWall_{};

    // Two-finger pinch state
    SDL_FingerID finger1Id_ = 0, finger2Id_ = 0;
    float finger1X_ = 0, finger1Y_ = 0;
    float finger2X_ = 0, finger2Y_ = 0;
    bool pinching_ = false;
    float lastPinchDist_ = 0;
    float lastPinchAngle_ = 0;
    float pinchDelta_ = 0;
};

} // namespace ge
