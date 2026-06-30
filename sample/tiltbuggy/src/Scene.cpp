// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tiltbuggy {

namespace {

// ----------------------------------------------------------------------------
// Vehicle constants (🎯T137.1)
//
// Scale: restored to the 2013 reference's ~10-unit world (walls at ±halfExtent,
// a 2×1 m chassis). box2d v3's solver is tuned for human-scale bodies, so the
// earlier 1/16 shrink (halfExtent 0.625) is dropped now that the follow-camera
// (🎯T137.3) keeps the buggy on screen without shrinking the world. The
// Renderer is scale-invariant (everything derives from scene.halfExtent()).
//
// The original (ViewController.mm) built a real top-down car: a chassis box, a
// separate steering body pivoted at the front (pivot + rotary-limit ±0.3 +
// self-centring damped spring → one box2d revolute joint here), and front/rear
// "wheels" that resist lateral slide up to a capped force (cpGrooveJoint with
// maxForce). box2d v3 has no groove joint, so the wheels are a top-down
// tire-friction model: each axle cancels its sideways velocity, capped by a
// grip *acceleration* (the mass-independent form of the old maxForce).
// ----------------------------------------------------------------------------

// Chassis — matches the original rectVects({1, 0.5}) → a 2×1 box.
constexpr float kChassisHalfLen = 1.0f;   // half-length along +x (forward)
constexpr float kChassisHalfWid = 0.5f;   // half-width along ±y
constexpr float kChassisDensity = 1.0f;

// Steering body — a tiny body ahead of the chassis centre, free to pivot
// ±kSteerClamp and self-centred by a soft rotary spring.
constexpr float kSteerOffsetX  = 0.7f;    // chassis-local x of the steer pivot
constexpr float kSteerHalf     = 0.1f;
constexpr float kSteerDensity   = 0.25f;
constexpr float kSteerClampRad  = 0.3f;   // ± steer-angle limit (orig -0.3..0.3)
constexpr float kSteerSpringHz  = 2.5f;   // self-centring stiffness (cycles/s)
constexpr float kSteerSpringZ   = 0.7f;   // self-centring damping ratio

// Tire friction. kBaseGrip is the asphalt lateral-traction cap (m/s²): high
// enough that the buggy tracks its heading. Ice/dirt drop the axle over them
// (🎯T137.2). kRollResist is mild forward drag so the buggy coasts to rest
// instead of gliding forever.
constexpr float kBaseGrip   = 400.0f;     // m/s² lateral grip on asphalt
constexpr float kRollResist = 0.6f;       // forward rolling drag, per second
constexpr float kRearWheelX  = -0.65f;    // chassis-local rear axle (orig -0.65)
constexpr float kFrontWheelX = -0.10f;    // steering-local front axle (orig -0.1)

// Cancel a wheel's lateral (sideways) velocity, capped by `gripAccel`, and
// apply a little forward rolling resistance. Applied at the wheel's world point
// so the impulse also yields the correct turning moment about the body's COM.
void applyTireFriction(b2BodyId body, b2Vec2 localPt, float gripAccel,
                       float rollResist, float dt) {
    const b2Vec2 fwd = b2Body_GetWorldVector(body, b2Vec2{1.0f, 0.0f});
    const b2Vec2 lat = b2Body_GetWorldVector(body, b2Vec2{0.0f, 1.0f});
    const b2Vec2 p   = b2Body_GetWorldPoint(body, localPt);
    const b2Vec2 v   = b2Body_GetWorldPointVelocity(body, p);
    const float  m   = b2Body_GetMass(body);

    // Lateral: cancel sideways velocity, capped at gripAccel·m·dt.
    float jLat = -b2Dot(v, lat) * m;
    const float cap = gripAccel * m * dt;
    jLat = std::clamp(jLat, -cap, cap);
    b2Body_ApplyLinearImpulse(body, b2MulSV(jLat, lat), p, true);

    // Forward: gentle rolling resistance.
    const float jFwd = -rollResist * b2Dot(v, fwd) * m * dt;
    b2Body_ApplyLinearImpulse(body, b2MulSV(jFwd, fwd), p, true);
}

} // namespace

// ----------------------------------------------------------------------------
// Impl
// ----------------------------------------------------------------------------

struct Scene::Impl {
    float halfExtent;

    b2WorldId worldId;

    b2BodyId groundId;
    b2BodyId chassisId;
    b2BodyId steeringId;

    b2JointId steeringJoint;  // revolute: chassis ↔ steering

    // Sensor shapes for surface patches.
    b2ShapeId iceShapeId;
    b2ShapeId dirtShapeId;

    // Live per-axle grip (m/s² lateral cap). Asphalt = kBaseGrip; 🎯T137.2 will
    // drop the axle over an ice / dirt patch and restore it on exit.
    float frontGrip = kBaseGrip;
    float rearGrip  = kBaseGrip;

    // Remembered surface bounds (y-up world rects) for surfaces().
    ge::Rect iceRect;
    ge::Rect dirtRect;

    Impl(float halfExtent_, bool allowBuggySleep) : halfExtent(halfExtent_) {
        // ------------------------------------------------------------------
        // World — gravity supplied per-step (device tilt); start at rest.
        // ------------------------------------------------------------------
        b2WorldDef wdef = b2DefaultWorldDef();
        wdef.gravity = {0.0f, 0.0f};
        worldId = b2CreateWorld(&wdef);

        // ------------------------------------------------------------------
        // Ground (static body: walls + sensor patches)
        // ------------------------------------------------------------------
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_staticBody;
            bdef.name = "arena";  // 🎯T117 names ride through worldGeometry as ids
            groundId = b2CreateBody(worldId, &bdef);
        }

        // Walls — four edge segments at ±halfExtent, elastic (orig restitution
        // 1.0; 0.6 here keeps the buggy from pinballing forever).
        {
            const float e = halfExtent;
            const b2Vec2 corners[4] = {
                {-e, -e}, { e, -e}, { e,  e}, {-e,  e}
            };
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.material.friction = 0.4f;
            sdef.material.restitution = 0.6f;
            for (int i = 0; i < 4; ++i) {
                b2Segment seg = { corners[i], corners[(i + 1) % 4] };
                b2CreateSegmentShape(groundId, &sdef, &seg);
            }
        }

        // ------------------------------------------------------------------
        // Chassis — 2×1 box at the origin.
        // ------------------------------------------------------------------
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_dynamicBody;
            bdef.name = "buggy";  // 🎯T117 → geometry slice id
            bdef.position = {0.0f, 0.0f};
            bdef.angularDamping = 0.5f;  // damps spin; lateral grip does the rest
            bdef.enableSleep = allowBuggySleep;  // 🎯T131.5 render-on-demand demo
            chassisId = b2CreateBody(worldId, &bdef);

            b2Polygon box = b2MakeBox(kChassisHalfLen, kChassisHalfWid);
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.density = kChassisDensity;
            sdef.material.friction = 0.6f;
            sdef.material.restitution = 0.3f;
            b2CreatePolygonShape(chassisId, &sdef, &box);
        }

        // ------------------------------------------------------------------
        // Steering body + revolute joint (the self-centring front axle).
        // The original used cpPivotJoint + cpRotaryLimitJoint(-0.3..0.3) +
        // cpDampedRotarySpring; box2d v3's revolute joint carries the limit
        // AND the self-centring spring in one joint.
        // ------------------------------------------------------------------
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_dynamicBody;
            bdef.name = "steering";
            bdef.position = {kSteerOffsetX, 0.0f};
            bdef.enableSleep = allowBuggySleep;
            steeringId = b2CreateBody(worldId, &bdef);

            b2Polygon box = b2MakeBox(kSteerHalf, kSteerHalf);
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.density = kSteerDensity;
            sdef.enableSensorEvents = false;
            // The steering body must not collide with anything — it is a pure
            // control linkage. Filter it out of all collisions.
            sdef.filter.categoryBits = 0;
            sdef.filter.maskBits = 0;
            b2CreatePolygonShape(steeringId, &sdef, &box);

            b2RevoluteJointDef jd = b2DefaultRevoluteJointDef();
            jd.bodyIdA = chassisId;
            jd.bodyIdB = steeringId;
            jd.localAnchorA = {kSteerOffsetX, 0.0f};
            jd.localAnchorB = {0.0f, 0.0f};
            jd.referenceAngle = 0.0f;
            jd.enableLimit = true;
            jd.lowerAngle = -kSteerClampRad;
            jd.upperAngle =  kSteerClampRad;
            jd.enableSpring = true;
            jd.targetAngle = 0.0f;          // self-centre to straight-ahead
            jd.hertz = kSteerSpringHz;
            jd.dampingRatio = kSteerSpringZ;
            jd.collideConnected = false;
            steeringJoint = b2CreateRevoluteJoint(worldId, &jd);
        }

        // ------------------------------------------------------------------
        // Surface sensor patches — restored to the 2013 layout (in world
        // units): ice across the upper-middle, dirt down the left edge.
        // ------------------------------------------------------------------

        // Ice: x ∈ [-6, 6], y ∈ [2, 6]   (orig rectVects({-6,2},{6,6}))
        iceRect = ge::Rect{-6.0f, 2.0f, 12.0f, 4.0f};
        iceShapeId = makeSensorPatch(iceRect);

        // Dirt: left strip x ∈ [-halfExtent, -halfExtent/2], full height
        // (orig dirt was the left ~quarter of the arena).
        dirtRect = ge::Rect{-halfExtent, -halfExtent,
                            halfExtent * 0.5f, 2.0f * halfExtent};
        dirtShapeId = makeSensorPatch(dirtRect);
    }

    b2ShapeId makeSensorPatch(const ge::Rect& r) {
        const float hw = r.w * 0.5f;
        const float hh = r.h * 0.5f;
        const auto c = r.center();
        b2Polygon box = b2MakeOffsetBox(hw, hh, b2Vec2{c.x, c.y}, b2Rot_identity);
        b2ShapeDef sdef = b2DefaultShapeDef();
        sdef.isSensor = true;
        sdef.enableSensorEvents = true;
        return b2CreatePolygonShape(groundId, &sdef, &box);
    }

    ~Impl() {
        b2DestroyWorld(worldId);
    }
};

// ----------------------------------------------------------------------------
// Scene
// ----------------------------------------------------------------------------

Scene::Scene(float halfExtent, bool allowBuggySleep)
    : i_(std::make_unique<Impl>(halfExtent, allowBuggySleep)) {}
Scene::~Scene() = default;

void Scene::step(float dt, b2Vec2 gravity) {
    b2World_SetGravity(i_->worldId, gravity);

    // 🎯T137.1 Tire friction — front (steering body) + rear (chassis) — applied
    // before the solve, like the original's per-step wheel update. This is what
    // makes the buggy drive in arcs instead of sliding like a frictionless box.
    // 🎯T137.2 will fold sensor-driven grip changes into i_->front/rearGrip.
    if (dt > 0.0f) {
        applyTireFriction(i_->chassisId,  {kRearWheelX, 0.0f},
                          i_->rearGrip,  kRollResist, dt);
        applyTireFriction(i_->steeringId, {kFrontWheelX, 0.0f},
                          i_->frontGrip, kRollResist, dt);
    }

    b2World_Step(i_->worldId, dt, 4);
}

Pose Scene::buggyPose() const {
    b2Vec2 pos = b2Body_GetPosition(i_->chassisId);
    b2Rot  rot = b2Body_GetRotation(i_->chassisId);
    return { pos.x, pos.y, b2Rot_GetAngle(rot) };
}

void Scene::applyPose(const Pose& pose) {
    const b2Rot rot = b2MakeRot(pose.angle);
    b2Body_SetTransform(i_->chassisId, {pose.x, pose.y}, rot);
    b2Body_SetLinearVelocity(i_->chassisId, {0.0f, 0.0f});
    b2Body_SetAngularVelocity(i_->chassisId, 0.0f);
    b2Body_SetAwake(i_->chassisId, true);

    // Keep the jointed steering body glued to the chassis front so the revolute
    // joint doesn't snap it back across the world after a teleport.
    const b2Vec2 steerPos =
        b2Body_GetWorldPoint(i_->chassisId, {kSteerOffsetX, 0.0f});
    b2Body_SetTransform(i_->steeringId, steerPos, rot);
    b2Body_SetLinearVelocity(i_->steeringId, {0.0f, 0.0f});
    b2Body_SetAngularVelocity(i_->steeringId, 0.0f);
    b2Body_SetAwake(i_->steeringId, true);
}

void Scene::wakeBuggy() {
    b2Body_SetAwake(i_->chassisId, true);
    b2Body_SetAwake(i_->steeringId, true);
}

b2WorldId Scene::worldId() const {
    return i_->worldId;
}

float Scene::halfExtent() const {
    return i_->halfExtent;
}

b2Vec2 Scene::chassisHalfExtents() const {
    return { kChassisHalfLen, kChassisHalfWid };
}

GripState Scene::gripState() const {
    return { i_->frontGrip, i_->rearGrip };
}

std::vector<Surface> Scene::surfaces() const {
    return {
        { i_->iceRect,  SurfaceType::Ice  },
        { i_->dirtRect, SurfaceType::Dirt },
    };
}

} // namespace tiltbuggy
