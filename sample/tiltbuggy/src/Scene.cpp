// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "Scene.h"

#include <cmath>
#include <memory>

namespace tiltbuggy {

// ----------------------------------------------------------------------------
// Impl
// ----------------------------------------------------------------------------

struct Scene::Impl {
    float halfExtent;

    b2WorldId worldId;

    b2BodyId groundId;
    b2BodyId chassisId;
    b2BodyId steeringId;

    // Wheel joints: rearLeft, rearRight, frontLeft, frontRight
    b2JointId wheelJoints[4];

    // Revolute joint connecting steering body to chassis
    b2JointId steeringJoint;

    // Sensor shapes for surface patches
    b2ShapeId iceShapeId;
    b2ShapeId dirtShapeId;

    // Remembered surface bounds for surfaces()
    // y-up world rects: x = left, y = bottom (smaller y), w/h positive.
    ge::Rect iceRect;
    ge::Rect dirtRect;

    explicit Impl(float halfExtent_) : halfExtent(halfExtent_) {
        // ------------------------------------------------------------------
        // World
        // ------------------------------------------------------------------
        b2WorldDef wdef = b2DefaultWorldDef();
        // Gravity is supplied per-step via b2World_SetGravity; start at zero.
        wdef.gravity = {0.0f, 0.0f};
        worldId = b2CreateWorld(&wdef);

        // ------------------------------------------------------------------
        // Ground (static body that owns walls and sensor patches)
        // ------------------------------------------------------------------
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_staticBody;
            bdef.name = "arena";  // 🎯T117 names ride through worldGeometry as ids
            groundId = b2CreateBody(worldId, &bdef);
        }

        // Walls — four edge segments at ±halfExtent
        {
            const float e = halfExtent;
            const b2Vec2 corners[4] = {
                {-e, -e}, { e, -e}, { e,  e}, {-e,  e}
            };
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.material.friction = 0.8f;
            sdef.material.restitution = 0.5f;
            for (int i = 0; i < 4; ++i) {
                b2Segment seg = { corners[i], corners[(i + 1) % 4] };
                b2CreateSegmentShape(groundId, &sdef, &seg);
            }
        }

        // ------------------------------------------------------------------
        // Chassis — 0.5 × 0.25 m rectangle, starts at origin
        // (Half real-world size + 2× viewport zoom = same on-screen size as
        // a 1.0 × 0.5 m chassis would have at the original scale, but
        // physics traverse the smaller arena 2× faster under the same
        // 9.81 m/s² gravity — see kWorldHalfExtent in main.cpp.)
        // ------------------------------------------------------------------
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_dynamicBody;
            bdef.name = "buggy";  // 🎯T117 → geometry slice id
            bdef.position = {0.0f, 0.0f};
            bdef.linearDamping = 0.5f;
            bdef.angularDamping = 2.0f;
            bdef.enableSleep = false;  // gravity changes must always take effect
            chassisId = b2CreateBody(worldId, &bdef);

            b2Polygon box = b2MakeBox(0.03125f, 0.015625f); // half-extents (1/16 of original)
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.density = 1.0f;
            sdef.material.friction = 0.8f;
            b2CreatePolygonShape(chassisId, &sdef, &box);
        }

        // Steering body and wheel joints removed for stage-2-minimum:
        // chassis slides freely under gravity to validate render+physics.
        // Re-add steering + wheels as separate bodies with proper vehicle
        // dynamics in a later pass.

        // ------------------------------------------------------------------
        // Surface sensor patches
        // ------------------------------------------------------------------

        // Ice patch: upper-centre — sized at 1/16 of the original layout.
        iceRect = ge::Rect{-0.1875f, 0.125f, 0.375f, 0.25f};
        {
            const float hw = iceRect.w * 0.5f;
            const float hh = iceRect.h * 0.5f;
            const auto centre = iceRect.center();
            b2Polygon box = b2MakeOffsetBox(hw, hh, b2Vec2{centre.x, centre.y}, b2Rot_identity);
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.isSensor = true;
            iceShapeId = b2CreatePolygonShape(groundId, &sdef, &box);
        }

        // Dirt patch: left strip, x ∈ [-halfExtent, -halfExtent/2], y ∈ [-halfExtent, halfExtent]
        dirtRect = ge::Rect{-halfExtent, -halfExtent, halfExtent * 0.5f, 2.f * halfExtent};
        {
            const float hw = dirtRect.w * 0.5f;
            const float hh = dirtRect.h * 0.5f;
            const auto centre = dirtRect.center();
            b2Polygon box = b2MakeOffsetBox(hw, hh, b2Vec2{centre.x, centre.y}, b2Rot_identity);
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.isSensor = true;
            dirtShapeId = b2CreatePolygonShape(groundId, &sdef, &box);
        }
    }

    ~Impl() {
        b2DestroyWorld(worldId);
    }
};

// ----------------------------------------------------------------------------
// Scene
// ----------------------------------------------------------------------------

Scene::Scene(float halfExtent) : i_(std::make_unique<Impl>(halfExtent)) {}
Scene::~Scene() = default;

void Scene::step(float dt, b2Vec2 gravity) {
    b2World_SetGravity(i_->worldId, gravity);
    b2World_Step(i_->worldId, dt, 4);
    // Arcade vehicle dynamics + surface effects temporarily disabled
    // so the buggy moves as a free-floating body — easier to diagnose
    // tilt-input behavior. Re-enable after viewport tilt is dialled in.
}

Pose Scene::buggyPose() const {
    b2Vec2 pos = b2Body_GetPosition(i_->chassisId);
    b2Rot  rot = b2Body_GetRotation(i_->chassisId);
    return { pos.x, pos.y, b2Rot_GetAngle(rot) };
}

b2WorldId Scene::worldId() const {
    return i_->worldId;
}

float Scene::halfExtent() const {
    return i_->halfExtent;
}

std::vector<Surface> Scene::surfaces() const {
    return {
        { i_->iceRect,  SurfaceType::Ice  },
        { i_->dirtRect, SurfaceType::Dirt },
    };
}

} // namespace tiltbuggy
