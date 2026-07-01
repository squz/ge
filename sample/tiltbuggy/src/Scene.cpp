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
// maxForce). box2d v3 has no groove joint, so the wheels become a top-down
// tire-friction model (front/rear lateral-slide damping on the chassis) plus an
// explicit heading-alignment torque — see the constant blocks below. The
// steering body + revolute joint are retained as the cosmetic self-centring
// caster. Final feel is tuned in 🎯T16.
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

// Tire friction. Both axles act on the chassis at their wheel points: box2d v3
// has no groove joint, and the original front groove (on the light steering
// body) doesn't translate to a mass-scaled impulse, so the front grip is applied
// to the chassis at the front axle — the steering body stays as the cosmetic
// self-centring caster (🎯T16).
//
// Each axle removes a *fraction* (0..1) of its sideways velocity per frame — a
// proportional cornering force, not a hard cancel. This is what makes the buggy
// turn to track its travel: rear grip > front grip puts the net lateral force
// behind the centre of mass (like a dart's fin), so the heading weathervanes
// onto the velocity. A hard symmetric cancel produces zero net torque from a
// clean sideslip (both axles cancel equally) — the buggy would just slide
// sideways and never turn; equal-and-saturating grip oversteers and spins out on
// a fast release. Ice/dirt scale the axle's fraction down (🎯T137.2).
constexpr float kFrontGrip  = 0.12f;      // fraction of front sideways vel / frame
constexpr float kRearGrip   = 0.28f;      // fraction of rear sideways vel / frame
constexpr float kRollResist = 0.6f;       // forward rolling drag, per second
constexpr float kFrontAxleX  =  0.65f;    // chassis-local front axle
constexpr float kRearWheelX  = -0.65f;    // chassis-local rear axle (orig -0.65)
constexpr float kMaxSpin     = 6.0f;      // rad/s yaw clamp — kills wild spins
constexpr float kRefHz       = 60.0f;     // grip fractions are calibrated at 60fps

// 🎯T16 Heading-alignment assist. The grip imbalance alone gives only a weak
// weathervane (grip removes the sideways velocity that would drive it), so an
// explicit restoring torque noses the buggy onto its travel direction: snappy at
// speed, quiet at rest, and stable by construction (a damped restoring torque
// can't spin out). kAlign is the stiffness; the body's angularDamping damps it.
constexpr float kAlign         = 6.0f;    // heading-alignment stiffness (forward only)
constexpr float kMinAlignSpeed = 0.4f;    // m/s below which we don't align (anti-jitter)

// ----------------------------------------------------------------------------
// Surface traps (🎯T137.2)
//
// The original tripped per-axle grip changes with four corner "tread" shapes
// over ice / dirt sensor patches. Here, two non-colliding tread probes (front /
// rear) on the chassis are detected by the ground sensors via collision-filter
// categories; b2World_GetSensorEvents drives the per-axle grip. Ice/dirt
// multiply the affected axle's grip (orig deltas: 150→75 ice = ×0.5,
// 150→90 dirt = ×0.6; exaggerated here for a more legible slide, tuned in 🎯T16).
// ----------------------------------------------------------------------------
constexpr float kIceGripFactor  = 0.18f;  // ice: very slippery
constexpr float kDirtGripFactor = 0.45f;  // dirt: loose, moderate slip
constexpr float kFrontProbeX    = 0.70f;  // chassis-local front tread (near steer)
constexpr float kProbeHalf      = 0.12f;  // small tread-probe box

// Collision-filter categories: tread probes are *detected by* the surface
// sensors but collide with nothing physical. Sensor detection respects the
// same filter as collision, so each shape opts into exactly what it needs.
constexpr uint64_t kCatWall   = 0x1;  // arena walls
constexpr uint64_t kCatBuggy  = 0x2;  // chassis body
constexpr uint64_t kCatProbe  = 0x4;  // tread probes (detection only)
constexpr uint64_t kCatSensor = 0x8;  // ice / dirt sensor patches

// Remove a fraction `gripFrac` (0..1) of a wheel's sideways velocity, plus a
// little forward rolling resistance. Applied at the wheel's world point so the
// impulse also yields the turning moment about the body's COM — the source of
// the weathervane alignment.
void applyTireFriction(b2BodyId body, b2Vec2 localPt, float gripFrac,
                       float rollResist, float dt) {
    const b2Vec2 fwd = b2Body_GetWorldVector(body, b2Vec2{1.0f, 0.0f});
    const b2Vec2 lat = b2Body_GetWorldVector(body, b2Vec2{0.0f, 1.0f});
    const b2Vec2 p   = b2Body_GetWorldPoint(body, localPt);
    const b2Vec2 v   = b2Body_GetWorldPointVelocity(body, p);
    const float  m   = b2Body_GetMass(body);

    // Lateral: remove gripFrac of the sideways velocity (calibrated at 60fps,
    // scaled to the actual dt, clamped so a long frame can't over-correct).
    const float frac = std::clamp(gripFrac * dt * kRefHz, 0.0f, 1.0f);
    const float jLat = -b2Dot(v, lat) * m * frac;
    b2Body_ApplyLinearImpulse(body, b2MulSV(jLat, lat), p, false);

    // Forward: gentle rolling resistance.
    const float jFwd = -rollResist * b2Dot(v, fwd) * m * dt;
    b2Body_ApplyLinearImpulse(body, b2MulSV(jFwd, fwd), p, false);
}

// 🎯T16 Torque the chassis so its heading tracks its travel direction — but only
// when driving FORWARD. The torque is ∝ sin(slip)·max(cos(slip),0)·speed:
//   • sin(slip)  — the lateral slip; the aligning direction, 0 when straight.
//   • cos(slip)  — a forward gate: full when moving along the nose (slip≈0),
//                  fading to 0 by a sideways slide and staying 0 in reverse.
// So reverse gets NO explicit alignment (like the 2013 car, which had none) and
// stays controllable — its only turning tendency is the gentle tire-friction
// weathervane. Forward, sin·cos ≈ slip for small slip, so it noses in snappily.
// Speed-scaled; damped by the body's angularDamping.
void applyAlignment(b2BodyId body) {
    const b2Vec2 vel = b2Body_GetLinearVelocity(body);
    const float speed = b2Length(vel);
    if (speed < kMinAlignSpeed) return;
    const float velAngle = std::atan2(vel.y, vel.x);
    const float heading  = b2Rot_GetAngle(b2Body_GetRotation(body));
    const float dHeading = velAngle - heading;
    const float fwdGate  = std::cos(dHeading);
    if (fwdGate <= 0.0f) return;  // reverse / sideways-back: tire weathervane only
    const float torque = kAlign * std::sin(dHeading) * fwdGate * speed
                       * b2Body_GetMass(body);
    b2Body_ApplyTorque(body, torque, false);  // torque, not impulse
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

    b2JointId steeringJoint;  // revolute: chassis ↔ steering

    // Sensor shapes for surface patches.
    b2ShapeId iceShapeId;
    b2ShapeId dirtShapeId;

    // 🎯T137.2 Non-colliding tread probes on the chassis, detected by the
    // surface sensors to drive per-axle grip.
    b2ShapeId frontProbeId;
    b2ShapeId rearProbeId;

    // Live per-axle grip (m/s² lateral cap). Asphalt = base; 🎯T137.2 drops the
    // axle over an ice / dirt patch and restores it on exit.
    float frontGrip = kFrontGrip;
    float rearGrip  = kRearGrip;

    // Remembered surface bounds (y-up world rects) for surfaces().
    ge::Rect iceRect;
    ge::Rect dirtRect;

    Impl(float halfExtent_, float arenaAspect, bool allowBuggySleep)
        : halfExtent(halfExtent_),
          halfWidth(halfExtent_ * (arenaAspect > 0.0f ? arenaAspect : 1.0f)) {
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
            const float w = halfWidth, e = halfExtent;
            const b2Vec2 corners[4] = {
                {-w, -e}, { w, -e}, { w,  e}, {-w,  e}
            };
            b2ShapeDef sdef = b2DefaultShapeDef();
            sdef.material.friction = 0.4f;
            sdef.material.restitution = 0.6f;
            sdef.filter.categoryBits = kCatWall;
            sdef.filter.maskBits = kCatBuggy;  // walls collide with the chassis only
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
            sdef.filter.categoryBits = kCatBuggy;
            sdef.filter.maskBits = kCatWall;        // collide with walls only
            sdef.enableSensorEvents = false;        // only the treads trip sensors
            b2CreatePolygonShape(chassisId, &sdef, &box);

            // 🎯T137.2 Front + rear tread probes. Density 0 (no mass), filtered
            // to be detected by the surface sensors but collide with nothing.
            b2ShapeDef pdef = b2DefaultShapeDef();
            pdef.density = 0.0f;
            pdef.filter.categoryBits = kCatProbe;
            pdef.filter.maskBits = kCatSensor;      // "collide" with sensors only
            pdef.enableSensorEvents = true;
            b2Polygon frontBox =
                b2MakeOffsetBox(kProbeHalf, kProbeHalf,
                                b2Vec2{kFrontProbeX, 0.0f}, b2Rot_identity);
            b2Polygon rearBox =
                b2MakeOffsetBox(kProbeHalf, kProbeHalf,
                                b2Vec2{kRearWheelX, 0.0f}, b2Rot_identity);
            frontProbeId = b2CreatePolygonShape(chassisId, &pdef, &frontBox);
            rearProbeId  = b2CreatePolygonShape(chassisId, &pdef, &rearBox);
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
        dirtRect = ge::Rect{-halfWidth, -halfExtent,
                            halfWidth * 0.5f, 2.0f * halfExtent};
        dirtShapeId = makeSensorPatch(dirtRect);
    }

    // 🎯T137.2 Drain this step's sensor begin/end events and set each axle's
    // grip: a tread probe entering ice / dirt drops that axle; leaving restores
    // it. Front and rear are independent (separate probes → separate grips).
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
        float factor = 1.0f;  // leaving a patch restores asphalt grip
        if (begin) {
            if (b2Shape_IsValid(sensor) && B2_ID_EQUALS(sensor, iceShapeId))
                factor = kIceGripFactor;
            else if (b2Shape_IsValid(sensor) && B2_ID_EQUALS(sensor, dirtShapeId))
                factor = kDirtGripFactor;
        }
        if (B2_ID_EQUALS(visitor, frontProbeId))      frontGrip = kFrontGrip * factor;
        else if (B2_ID_EQUALS(visitor, rearProbeId))  rearGrip  = kRearGrip  * factor;
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

    // 🎯T137.1/T16 Tire friction — front + rear axles, both on the chassis —
    // applied before the solve, like the original's per-step wheel update. This
    // is what makes the buggy drive in arcs instead of sliding like a box; rear
    // grip > front grip keeps it from spinning out (understeer).
    // 🎯T137.2 folds sensor-driven grip changes into i_->front/rearGrip.
    // Skip a sleeping chassis (🎯T131.5 render-on-demand): a settled buggy must
    // stay asleep so the loop idles — applyTireFriction passes wake=false.
    if (dt > 0.0f && b2Body_IsAwake(i_->chassisId)) {
        applyTireFriction(i_->chassisId, {kFrontAxleX, 0.0f},
                          i_->frontGrip, kRollResist, dt);
        applyTireFriction(i_->chassisId, {kRearWheelX, 0.0f},
                          i_->rearGrip,  kRollResist, dt);
        applyAlignment(i_->chassisId);
    }

    b2World_Step(i_->worldId, dt, 4);

    // 🎯T16 Yaw clamp — a hard cap on chassis spin so a fast release / glancing
    // wall hit can't send the buggy into an unrecoverable pirouette. Normal
    // turning (~2–3 rad/s) is well under the cap, so it only catches blow-ups.
    const float w = b2Body_GetAngularVelocity(i_->chassisId);
    if (w >  kMaxSpin) b2Body_SetAngularVelocity(i_->chassisId,  kMaxSpin);
    else if (w < -kMaxSpin) b2Body_SetAngularVelocity(i_->chassisId, -kMaxSpin);

    // 🎯T137.2 Fold this step's surface overlaps into next step's grip.
    i_->updateSurfaceGrip();
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
