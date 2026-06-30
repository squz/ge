// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ge/SessionHost.h>  // ge::Rect

#include <box2d/box2d.h>
#include <memory>
#include <vector>

namespace tiltbuggy {

// World pose — y-up, radians, metres.
struct Pose { float x, y, angle; };

enum class SurfaceType { Asphalt, Ice, Dirt };
struct Surface {
    // AABB in world coords (y-up): rect.x = left, rect.y = bottom (smaller y),
    // rect.w / rect.h positive.
    ge::Rect    rect;
    SurfaceType type;
};

// 🎯T137.1/.2 Live grip (lateral-traction cap, m/s²) of each axle. Asphalt =
// kBaseGrip; ice/dirt drop the axle currently over them. Exposed so a log /
// state slice can show the wheel grip changing as the buggy crosses a patch.
struct GripState { float front, rear; };

class Scene {
public:
    // World is [-halfExtent, +halfExtent] on both axes.
    //
    // 🎯T131.5 allowBuggySleep: by default the chassis has sleep DISABLED so a
    // gravity change always takes effect immediately (the buggy is the whole
    // game). For the render-on-demand demo it must be allowed to sleep when the
    // buggy settles (so the box2d-awake trigger lets the loop idle); the
    // consumer then calls wakeBuggy() on a real tilt so the new gravity moves it.
    explicit Scene(float halfExtent, bool allowBuggySleep = false);
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Step physics by `dt` seconds. `gravity` drives the buggy — the car
    // has no throttle; it accelerates in whatever direction gravity points.
    void step(float dt, b2Vec2 gravity);

    // Pose of the buggy chassis in world coords.
    Pose buggyPose() const;

    // 🎯T124 Set the buggy chassis to a saved pose (and zero its velocity) so a
    // serialized state can be reconstructed for a headless render. The static
    // arena is rebuilt deterministically by the ctor; only the dynamic body's
    // transform needs restoring.
    void applyPose(const Pose& pose);

    // 🎯T131.5 Wake the chassis. Needed when allowBuggySleep is on and a sleeping
    // buggy must respond to a new gravity vector: b2World_SetGravity does not
    // wake sleeping bodies, so a tilt has to wake it explicitly.
    void wakeBuggy();

    // The box2d world — exposed so the 🎯T117 geometry slice can read every body
    // out via ge::box2d::worldGeometry(scene->worldId()).
    b2WorldId worldId() const;

    // Extent of the world (walls at ±halfExtent).
    float halfExtent() const;

    // 🎯T137.1 Half-extents of the chassis box (metres). Single source of truth
    // shared with the Renderer so the drawn buggy matches the physics body.
    b2Vec2 chassisHalfExtents() const;

    // 🎯T137.1/.2 Current per-axle grip (m/s² lateral-traction cap).
    GripState gripState() const;

    // Static surface regions for rendering (includes ice, dirt).
    // Does NOT include the default asphalt background.
    std::vector<Surface> surfaces() const;

private:
    struct Impl;
    std::unique_ptr<Impl> i_;
};

} // namespace tiltbuggy
