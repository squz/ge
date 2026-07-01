// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace tiltbuggy {

namespace {

// ----------------------------------------------------------------------------
// Vehicle model — a faithful port of the 2013 Chipmunk car (🎯T16)
//
// The original is a CONSTRAINT-based top-down car, not a force heuristic. We
// replicate its exact structure and numbers so the dynamics (forward
// weathervane-stability AND reverse fishtailing) emerge from the geometry
// rather than being faked with an alignment torque:
//
//   • Chassis: a 2×1 box, mass 1, moment of inertia 1 (cpBodyNew(1,1)). The
//     moment is set by hand — ~2.4× a uniform box's I/m — which is what gives
//     the car its planted, deliberate rotation.
//   • Steering body: mass 0.1, moment 0.1, at chassis-local (0.7, 0), joined to
//     the chassis by a revolute with a ±0.3 rad limit and a PURE rotary damper
//     (cpDampedRotarySpring stiffness 0, damping 1 — no self-centring spring).
//   • Two "wheels" = groove joints (cpGrooveJoint + maxForce 150): hard
//     lateral-velocity constraints that null the sideways velocity at the wheel
//     point, capped at 150 N (ice/dirt lower the cap so the wheel slips). The
//     REAR grips on the chassis at (-0.65,0); the FRONT grips on the STEERING
//     body at (-0.1,0), so the front wheel resists slip along the *steered*
//     direction and the force feeds the chassis through the pivot.
//   • 4 substeps/frame (orig 4 × cpSpaceStep(0.25/60)), re-solving the wheels
//     each substep. No linear/angular damping (Chipmunk default), no rolling
//     resistance — the car coasts, controlled by tilt + walls + wheel grip.
// ----------------------------------------------------------------------------

// Chassis (orig cpBodyNew(1,1), box rectVects({1,0.5}) = 2×1).
constexpr float kChassisHalfLen = 1.0f;    // half-length along +x (forward)
constexpr float kChassisHalfWid = 0.5f;    // half-width along ±y
constexpr float kChassisMass    = 1.0f;
constexpr float kChassisInertia = 1.0f;    // set by hand (orig), not from the box
constexpr float kChassisRestitution = 0.5f;

// Steering body (orig cpBodyNew(0.1,0.1) at chassis-local (0.7,0)).
constexpr float kSteerOffsetX = 0.7f;
constexpr float kSteerHalf    = 0.1f;      // small box (visual/link only)
constexpr float kSteerMass    = 0.1f;
constexpr float kSteerInertia = 0.1f;
constexpr float kSteerLimit   = 0.3f;      // ± revolute limit (orig ±0.3)
constexpr float kSteerDamping = 1.0f;      // pure rotary damper (orig damping 1)

// Wheels — force-capped lateral constraints (orig cpGrooveJoint maxForce 150).
constexpr float kBaseGrip    = 150.0f;     // N lateral force cap on asphalt
constexpr float kFrontWheelX = -0.10f;     // steering-local front wheel (orig -0.1)
constexpr float kRearWheelX  = -0.65f;     // chassis-local rear wheel (orig -0.65)

constexpr int   kSubSteps = 4;             // physics substeps / frame (orig 4)

// ----------------------------------------------------------------------------
// Surface traps (🎯T137.2) — orig grip 150 → 75 on ice, 90 on dirt, per axle.
// ----------------------------------------------------------------------------
constexpr float kIceGrip     = 75.0f;      // N (orig 150 - 75)
constexpr float kDirtGrip    = 90.0f;      // N (orig 150 - 60)
constexpr float kFrontProbeX = 0.70f;      // chassis-local front tread (near steer)
constexpr float kProbeHalf   = 0.12f;      // small tread-probe box

// Collision-filter categories: tread probes are *detected by* the surface
// sensors but collide with nothing physical.
constexpr uint64_t kCatWall   = 0x1;
constexpr uint64_t kCatBuggy  = 0x2;
constexpr uint64_t kCatProbe  = 0x4;
constexpr uint64_t kCatSensor = 0x8;

// A groove-joint wheel: null the lateral (sideways) velocity at a wheel point,
// capped at maxForce. `lat` is the body's local lateral axis (so the front
// wheel, on the steering body, grips along the steered direction). Uses the
// constraint's effective mass 1/(1/m + (r⊥·n)²/I) so the impulse exactly
// removes the point's lateral velocity — the cpGrooveJoint behaviour.
void applyWheelGrip(b2BodyId body, b2Vec2 localPt, float maxForce, float dt) {
    const b2Vec2 lat = b2Body_GetWorldVector(body, b2Vec2{0.0f, 1.0f});
    const b2Vec2 p   = b2Body_GetWorldPoint(body, localPt);
    const b2Vec2 v   = b2Body_GetWorldPointVelocity(body, p);
    const float  vLat = b2Dot(v, lat);
    const b2Vec2 com = b2Body_GetWorldCenterOfMass(body);
    const b2Vec2 r   = b2Sub(p, com);
    const float  rn  = b2Cross(r, lat);
    const float  m   = b2Body_GetMass(body);
    const float  I   = b2Body_GetRotationalInertia(body);
    const float  invM = 1.0f / m + (I > 0.0f ? (rn * rn) / I : 0.0f);
    float j = (invM > 0.0f) ? (-vLat / invM) : 0.0f;
    const float cap = maxForce * dt;
    j = std::clamp(j, -cap, cap);
    b2Body_ApplyLinearImpulse(body, b2MulSV(j, lat), p, false);
}

// Pure rotary damper between chassis and steering (orig cpDampedRotarySpring
// stiffness 0, damping 1): resist the RELATIVE spin, no centring.
void applySteerDamping(b2BodyId chassis, b2BodyId steer, float damping) {
    const float relW = b2Body_GetAngularVelocity(steer)
                     - b2Body_GetAngularVelocity(chassis);
    const float torque = -damping * relW;
    b2Body_ApplyTorque(steer, torque, false);
    b2Body_ApplyTorque(chassis, -torque, false);
}

} // namespace

// ----------------------------------------------------------------------------
// Impl
// ----------------------------------------------------------------------------

struct Scene::Impl {
    float halfExtent;   // y (shorter) axis
    float halfWidth;    // x axis = halfExtent * arenaAspect (🎯T137.3)

    b2WorldId worldId;

    b2BodyId groundId;
    b2BodyId chassisId;
    b2BodyId steeringId;

    b2JointId steeringJoint;  // revolute: chassis ↔ steering (limit only)

    // Sensor shapes for surface patches.
    b2ShapeId iceShapeId;
    b2ShapeId dirtShapeId;

    // 🎯T137.2 Non-colliding tread probes on the chassis, detected by the
    // surface sensors to drive per-axle grip.
    b2ShapeId frontProbeId;
    b2ShapeId rearProbeId;

    // Live per-axle wheel grip (lateral force cap, N). Asphalt = kBaseGrip;
    // 🎯T137.2 drops the axle over an ice / dirt patch and restores it on exit.
    float frontGrip = kBaseGrip;
    float rearGrip  = kBaseGrip;

    // Remembered surface bounds (y-up world rects) for surfaces().
    ge::Rect iceRect;
    ge::Rect dirtRect;

    Impl(float halfExtent_, float arenaAspect, bool allowBuggySleep)
        : halfExtent(halfExtent_),
          halfWidth(halfExtent_ * (arenaAspect > 0.0f ? arenaAspect : 1.0f)) {
        // World — gravity supplied per-step (device tilt); start at rest.
        b2WorldDef wdef = b2DefaultWorldDef();
        wdef.gravity = {0.0f, 0.0f};
        worldId = b2CreateWorld(&wdef);

        // Ground (static body: walls + sensor patches).
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_staticBody;
            bdef.name = "arena";  // 🎯T117 names ride through worldGeometry as ids
            groundId = b2CreateBody(worldId, &bdef);
        }

        // Walls — four edge segments at ±halfWidth (x) × ±halfExtent (y).
        // Elastic (orig restitution 1.0), frictionless (orig walls set no
        // friction) so the car slides along them.
        {
            const float w = halfWidth, e = halfExtent;
            const b2Vec2 corners[4] = {
                {-w, -e}, { w, -e}, { w,  e}, {-w,  e}
            };
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.material.friction = 0.0f;
            sdef.material.restitution = 1.0f;
            sdef.filter.categoryBits = kCatWall;
            sdef.filter.maskBits = kCatBuggy;  // walls collide with the chassis only
            for (int i = 0; i < 4; ++i) {
                b2Segment seg = { corners[i], corners[(i + 1) % 4] };
                b2CreateSegmentShape(groundId, &sdef, &seg);
            }
        }

        // Chassis — 2×1 box at the origin, mass/moment (1,1) set by hand.
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_dynamicBody;
            bdef.name = "buggy";  // 🎯T117 → geometry slice id
            bdef.position = {0.0f, 0.0f};
            // No linear/angular damping — Chipmunk default (the car coasts).
            bdef.enableSleep = allowBuggySleep;  // 🎯T131.5 render-on-demand demo
            chassisId = b2CreateBody(worldId, &bdef);

            b2Polygon box = b2MakeBox(kChassisHalfLen, kChassisHalfWid);
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.density = 1.0f;  // overridden by SetMassData below
            sdef.material.friction = 0.0f;
            sdef.material.restitution = kChassisRestitution;
            sdef.filter.categoryBits = kCatBuggy;
            sdef.filter.maskBits = kCatWall;        // collide with walls only
            sdef.enableSensorEvents = false;        // only the treads trip sensors
            b2CreatePolygonShape(chassisId, &sdef, &box);

            // 🎯T137.2 Front + rear tread probes. Density 0, filtered to be
            // detected by the surface sensors but collide with nothing.
            b2ShapeDef pdef = b2DefaultShapeDef();
            pdef.density = 0.0f;
            pdef.filter.categoryBits = kCatProbe;
            pdef.filter.maskBits = kCatSensor;
            pdef.enableSensorEvents = true;
            b2Polygon frontBox = b2MakeOffsetBox(kProbeHalf, kProbeHalf,
                                     b2Vec2{kFrontProbeX, 0.0f}, b2Rot_identity);
            b2Polygon rearBox  = b2MakeOffsetBox(kProbeHalf, kProbeHalf,
                                     b2Vec2{kRearWheelX, 0.0f}, b2Rot_identity);
            frontProbeId = b2CreatePolygonShape(chassisId, &pdef, &frontBox);
            rearProbeId  = b2CreatePolygonShape(chassisId, &pdef, &rearBox);

            // Match the original mass + moment exactly (after all shapes so the
            // auto-computed mass doesn't override this).
            b2MassData md{};
            md.mass = kChassisMass;
            md.center = {0.0f, 0.0f};
            md.rotationalInertia = kChassisInertia;
            b2Body_SetMassData(chassisId, md);
        }

        // Steering body + revolute joint: ±0.3 rad limit, NO spring (orig
        // stiffness 0). The rotary damping is applied manually each substep.
        {
            b2BodyDef bdef = b2DefaultBodyDef();
            bdef.type = b2_dynamicBody;
            bdef.name = "steering";
            bdef.position = {kSteerOffsetX, 0.0f};
            bdef.enableSleep = allowBuggySleep;
            steeringId = b2CreateBody(worldId, &bdef);

            b2Polygon box = b2MakeBox(kSteerHalf, kSteerHalf);
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.density = 1.0f;  // overridden below
            sdef.enableSensorEvents = false;
            sdef.filter.categoryBits = 0;  // pure control linkage; no collisions
            sdef.filter.maskBits = 0;
            b2CreatePolygonShape(steeringId, &sdef, &box);

            b2MassData md{};
            md.mass = kSteerMass;
            md.center = {0.0f, 0.0f};
            md.rotationalInertia = kSteerInertia;
            b2Body_SetMassData(steeringId, md);

            b2RevoluteJointDef jd = b2DefaultRevoluteJointDef();
            jd.bodyIdA = chassisId;
            jd.bodyIdB = steeringId;
            jd.localAnchorA = {kSteerOffsetX, 0.0f};
            jd.localAnchorB = {0.0f, 0.0f};
            jd.referenceAngle = 0.0f;
            jd.enableLimit = true;
            jd.lowerAngle = -kSteerLimit;
            jd.upperAngle =  kSteerLimit;
            jd.enableSpring = false;   // no self-centring (orig stiffness 0)
            jd.enableMotor  = false;   // damping done manually (applySteerDamping)
            jd.collideConnected = false;
            steeringJoint = b2CreateRevoluteJoint(worldId, &jd);
        }

        // Surface sensor patches (orig layout): ice upper-middle, dirt left edge.
        iceRect = ge::Rect{-6.0f, 2.0f, 12.0f, 4.0f};
        iceShapeId = makeSensorPatch(iceRect);
        dirtRect = ge::Rect{-halfWidth, -halfExtent,
                            halfWidth * 0.5f, 2.0f * halfExtent};
        dirtShapeId = makeSensorPatch(dirtRect);
    }

    // 🎯T137.2 Drain sensor begin/end events and set each axle's grip cap: a
    // tread probe entering ice / dirt drops that axle; leaving restores it.
    void updateSurfaceGrip() {
        b2SensorEvents ev = b2World_GetSensorEvents(worldId);
        for (int i = 0; i < ev.beginCount; ++i)
            applyTouch(ev.beginEvents[i].visitorShapeId,
                       ev.beginEvents[i].sensorShapeId, true);
        for (int i = 0; i < ev.endCount; ++i) {
            const auto& e = ev.endEvents[i];
            if (!b2Shape_IsValid(e.visitorShapeId)) continue;
            applyTouch(e.visitorShapeId, e.sensorShapeId, false);
        }
    }

    void applyTouch(b2ShapeId visitor, b2ShapeId sensor, bool begin) {
        float grip = kBaseGrip;  // leaving a patch restores asphalt grip
        if (begin) {
            if (b2Shape_IsValid(sensor) && B2_ID_EQUALS(sensor, iceShapeId))
                grip = kIceGrip;
            else if (b2Shape_IsValid(sensor) && B2_ID_EQUALS(sensor, dirtShapeId))
                grip = kDirtGrip;
        }
        if (B2_ID_EQUALS(visitor, frontProbeId))      frontGrip = grip;
        else if (B2_ID_EQUALS(visitor, rearProbeId))  rearGrip  = grip;
    }

    b2ShapeId makeSensorPatch(const ge::Rect& r) {
        const float hw = r.w * 0.5f;
        const float hh = r.h * 0.5f;
        const auto c = r.center();
        b2Polygon box = b2MakeOffsetBox(hw, hh, b2Vec2{c.x, c.y}, b2Rot_identity);
        b2ShapeDef sdef = b2DefaultShapeDef();
        sdef.isSensor = true;
        sdef.enableSensorEvents = true;
        sdef.filter.categoryBits = kCatSensor;
        sdef.filter.maskBits = kCatProbe;  // detect tread probes only
        return b2CreatePolygonShape(groundId, &sdef, &box);
    }

    ~Impl() {
        b2DestroyWorld(worldId);
    }
};

// ----------------------------------------------------------------------------
// Scene
// ----------------------------------------------------------------------------

Scene::Scene(float halfExtent, float arenaAspect, bool allowBuggySleep)
    : i_(std::make_unique<Impl>(halfExtent, arenaAspect, allowBuggySleep)) {}
Scene::~Scene() = default;

void Scene::step(float dt, b2Vec2 gravity) {
    b2World_SetGravity(i_->worldId, gravity);
    if (dt <= 0.0f) { b2World_Step(i_->worldId, 0.0f, 1); return; }

    // 🎯T16 Substep like the original (4 × the frame), re-solving the two
    // groove-joint wheels each substep: rear on the chassis, front on the
    // steering body (so it grips along the steered direction). The pivot joint
    // (solved inside b2World_Step) feeds the front grip into the chassis. No
    // heuristic alignment torque — the weathervane (forward) and the fishtail
    // (reverse) fall out of the geometry.
    const float h = dt / kSubSteps;
    for (int s = 0; s < kSubSteps; ++s) {
        // Skip a sleeping chassis (🎯T131.5 render-on-demand idle).
        if (b2Body_IsAwake(i_->chassisId)) {
            applyWheelGrip(i_->chassisId,  {kRearWheelX,  0.0f}, i_->rearGrip,  h);
            applyWheelGrip(i_->steeringId, {kFrontWheelX, 0.0f}, i_->frontGrip, h);
            applySteerDamping(i_->chassisId, i_->steeringId, kSteerDamping);
        }
        b2World_Step(i_->worldId, h, 4);
        i_->updateSurfaceGrip();
    }
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

float Scene::halfWidth() const {
    return i_->halfWidth;
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
