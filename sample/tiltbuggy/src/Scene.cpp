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
// Surface traps (🎯T137.2). The original has FOUR tread shapes at the chassis
// corners (±0.85, ±0.45) — two per axle — and its collision handler subtracts
// the surface's grip delta PER TREAD from that axle's wheel force cap. So a
// fully-on axle reads 150 − 2·75 = 0 on ice / 150 − 2·60 = 30 on dirt, but a
// HALF-on axle (one tread over the edge) reads the intermediate 75 / 90 — a
// third grip level the differential harness proved is load-bearing (a half-on
// rear still grips → the car rotates). We replicate all four treads + the
// additive per-tread deltas so the grip states match {150, 75, 0} exactly.
// ----------------------------------------------------------------------------
constexpr int   kIceDelta   = -75;         // per-tread grip delta on ice (orig)
constexpr int   kDirtDelta  = -60;         // per-tread grip delta on dirt (orig)
constexpr float kTreadX     = 0.85f;       // tread x offset (orig ±0.85)
constexpr float kTreadY     = 0.45f;       // tread y offset (orig ±0.45)
constexpr float kTreadHalfX = 0.10f;       // tread box half-extents (orig rectVects({0.1,0.04}))
constexpr float kTreadHalfY = 0.04f;

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

// Pure rotary damper between chassis and steering — a verbatim port of Chipmunk's
// cpDampedRotarySpring (stiffness 0, damping 1). Per substep it removes
// w_coef = 1 − exp(−damping·dt·moment) of the RELATIVE angular velocity (moment =
// I⁻¹chassis + I⁻¹steer), distributing the impulse via the shared effective
// moment 1/moment. This is the exact velocity-level operation, not a linear
// ApplyTorque (which over-damps and couples through the integrator differently).
void applySteerDamping(b2BodyId chassis, b2BodyId steer, float damping, float dt) {
    const float iChas = b2Body_GetRotationalInertia(chassis);
    const float iSteer = b2Body_GetRotationalInertia(steer);
    if (iChas <= 0.0f || iSteer <= 0.0f) return;
    const float iaInv = 1.0f / iChas, ibInv = 1.0f / iSteer;
    const float moment = iaInv + ibInv;
    const float wCoef = 1.0f - std::exp(-damping * dt * moment);
    const float wA = b2Body_GetAngularVelocity(chassis);   // a = chassis
    const float wB = b2Body_GetAngularVelocity(steer);      // b = steering
    const float wrn = wA - wB;
    const float jDamp = (-wrn * wCoef) / moment;            // = w_damp · iSum
    b2Body_SetAngularVelocity(chassis, wA + jDamp * iaInv);
    b2Body_SetAngularVelocity(steer,   wB - jDamp * ibInv);
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

    // 🎯T137.2 Four non-colliding tread probes (2 front, 2 rear) at the chassis
    // corners, detected by the surface sensors. probes[0,1] are front, [2,3]
    // rear. probeDelta[i] is the grip delta of the surface probe i currently
    // sits on (0 = asphalt), so each axle's grip is derived, not accumulated —
    // robust against a teleport dropping a begin/end pair.
    b2ShapeId probes[4];
    int       probeDelta[4] = {0, 0, 0, 0};

    // Live per-axle wheel grip (lateral force cap, N), derived from the treads.
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

            // 🎯T137.2 Four tread probes at the chassis corners (orig ±0.85,
            // ±0.45): probes[0,1] front, [2,3] rear. Density 0, filtered to be
            // detected by the surface sensors but collide with nothing.
            b2ShapeDef pdef = b2DefaultShapeDef();
            pdef.density = 0.0f;
            pdef.filter.categoryBits = kCatProbe;
            pdef.filter.maskBits = kCatSensor;
            pdef.enableSensorEvents = true;
            const b2Vec2 treadPos[4] = {
                {+kTreadX, -kTreadY}, {+kTreadX, +kTreadY},   // front
                {-kTreadX, -kTreadY}, {-kTreadX, +kTreadY},   // rear
            };
            for (int i = 0; i < 4; ++i) {
                b2Polygon t = b2MakeOffsetBox(kTreadHalfX, kTreadHalfY,
                                              treadPos[i], b2Rot_identity);
                probes[i] = b2CreatePolygonShape(chassisId, &pdef, &t);
            }

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

    // 🎯T137.2 Drain sensor begin/end events, updating which surface each tread
    // sits on, then derive each axle's grip = 150 + Σ (its two treads' deltas),
    // clamped at 0 — exactly the original's per-tread additive collision handler.
    void updateSurfaceGrip() {
        b2SensorEvents ev = b2World_GetSensorEvents(worldId);
        for (int i = 0; i < ev.beginCount; ++i)
            setProbeDelta(ev.beginEvents[i].visitorShapeId,
                          surfaceDelta(ev.beginEvents[i].sensorShapeId));
        for (int i = 0; i < ev.endCount; ++i) {
            if (!b2Shape_IsValid(ev.endEvents[i].visitorShapeId)) continue;
            setProbeDelta(ev.endEvents[i].visitorShapeId, 0);  // left the patch
        }
        frontGrip = std::max(0.0f, kBaseGrip + float(probeDelta[0] + probeDelta[1]));
        rearGrip  = std::max(0.0f, kBaseGrip + float(probeDelta[2] + probeDelta[3]));
    }

    int surfaceDelta(b2ShapeId sensor) const {
        if (b2Shape_IsValid(sensor)) {
            if (B2_ID_EQUALS(sensor, iceShapeId))  return kIceDelta;
            if (B2_ID_EQUALS(sensor, dirtShapeId)) return kDirtDelta;
        }
        return 0;
    }
    void setProbeDelta(b2ShapeId visitor, int delta) {
        for (int i = 0; i < 4; ++i)
            if (B2_ID_EQUALS(visitor, probes[i])) { probeDelta[i] = delta; return; }
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
            applySteerDamping(i_->chassisId, i_->steeringId, kSteerDamping, h);
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
