// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ORACLE: the 2013 TiltBuggy physics extracted verbatim from ViewController.mm,
// running headless on the vendored Chipmunk 6.2.1. This is the ground truth the
// box2d Scene is validated against by the differential harness. Every constant,
// body, constraint, tread, sensor, and the 4-substep update is a faithful copy
// of the original — no rendering, no UIKit, no CoreMotion.

#include <chipmunk/chipmunk.h>

#include <array>
#include <memory>

namespace oracle {

// Original rectVects helpers (ViewController.mm).
using cpRectVects = std::array<cpVect, 4>;
static cpRectVects rectVects(cpVect bl, cpVect tr) {
    return {{ {bl.x, tr.y}, {tr.x, tr.y}, {tr.x, bl.y}, {bl.x, bl.y} }};
}
static cpRectVects rectVects(cpVect tr) { return rectVects(cpvneg(tr), tr); }

enum Collision : cpCollisionType { coll_front = 1, coll_rear, coll_surface };

// Per-axle grip carried across the collision callbacks (orig used a
// shared_ptr<int> starting at 150; each tread over a surface adds the surface's
// delta, so with both treads of an axle on ice: 150 - 75 - 75 = 0).
struct SurfaceCtx { cpConstraint* wheel; int grip; };

static cpBool onSurfaceBegin(cpArbiter* arb, cpSpace*, void* data) {
    auto* ctx = static_cast<SurfaceCtx*>(data);
    cpShape *a, *b; cpArbiterGetShapes(arb, &a, &b);
    ctx->grip += *static_cast<int*>(cpShapeGetUserData(b));
    cpConstraintSetMaxForce(ctx->wheel, ctx->grip);
    return cpTrue;
}
static void onSurfaceSeparate(cpArbiter* arb, cpSpace*, void* data) {
    auto* ctx = static_cast<SurfaceCtx*>(data);
    cpShape *a, *b; cpArbiterGetShapes(arb, &a, &b);
    ctx->grip -= *static_cast<int*>(cpShapeGetUserData(b));
    cpConstraintSetMaxForce(ctx->wheel, ctx->grip);
}

class OldSim {
public:
    // halfExtent/aspect parameterise the arena (orig used WORLD_SIZE = 9.9); the
    // harness passes the same values it gives the box2d Scene so the walls match.
    OldSim(double halfExtent, double aspect) {
        const double W = halfExtent;

        space_ = cpSpaceNew();
        cpSpaceSetIterations(space_, 10);
        ground_ = cpSpaceGetStaticBody(space_);

        // Walls.
        const cpRectVects wallVects = rectVects(cpv(W * aspect, W));
        for (int i = 0; i < 4; ++i) {
            cpShape* wall = cpSegmentShapeNew(ground_, wallVects[i],
                                              wallVects[(i + 1) % 4], 0.01);
            cpShapeSetLayers(wall, 1);
            cpShapeSetElasticity(wall, 1.0);
            cpSpaceAddShape(space_, wall);
        }

        // Buggy (mass 1, moment 1) — a 2×1 box.
        cpRectVects buggyVects = rectVects(cpv(1, 0.5));
        buggy_ = cpBodyNew(1, 1);
        cpSpaceAddBody(space_, buggy_);
        cpShape* buggyShape = cpPolyShapeNew(buggy_, 4, buggyVects.data(), cpvzero);
        cpShapeSetElasticity(buggyShape, 0.5);
        cpShapeSetLayers(buggyShape, 1);
        cpSpaceAddShape(space_, buggyShape);

        // Steering body (mass 0.1, moment 0.1) at buggy-local (0.7, 0).
        steering_ = cpBodyNew(0.1, 0.1);
        cpBodySetPos(steering_, cpBodyLocal2World(buggy_, cpv(0.7, 0)));
        cpBodySetAngle(steering_, cpBodyGetAngle(buggy_));
        cpSpaceAddBody(space_, steering_);

        cpSpaceAddConstraint(space_, cpPivotJointNew(buggy_, steering_,
                                                     cpBodyGetPos(steering_)));
        cpSpaceAddConstraint(space_, cpRotaryLimitJointNew(buggy_, steering_,
                                                           -0.3, 0.3));
        cpSpaceAddConstraint(space_, cpDampedRotarySpringNew(buggy_, steering_,
                                                             0, 0, 1));

        // Front wheel (groove on steering) + rear wheel (groove on buggy).
        frontWheel_ = cpGrooveJointNew(steering_, ground_, cpvzero, cpvzero, cpvzero);
        cpConstraintSetMaxForce(frontWheel_, 150);
        cpSpaceAddConstraint(space_, frontWheel_);
        cpGrooveJointSetGrooveA(frontWheel_, cpv(-0.1 - 5, 0));
        cpGrooveJointSetGrooveB(frontWheel_, cpv(-0.1 + 5, 0));

        rearWheel_ = cpGrooveJointNew(buggy_, ground_, cpvzero, cpvzero, cpvzero);
        cpConstraintSetMaxForce(rearWheel_, 150);
        cpSpaceAddConstraint(space_, rearWheel_);
        cpGrooveJointSetGrooveA(rearWheel_, cpv(-0.65 - 5, 0));
        cpGrooveJointSetGrooveB(rearWheel_, cpv(-0.65 + 5, 0));

        // Tread — 4 small boxes at the corners, front/rear collision types.
        for (int i = 0, x = -1; x <= 1; x += 2)
            for (int y = -1; y <= 1; y += 2, ++i) {
                cpRectVects tv = rectVects(cpv(0.1, 0.04));
                tread_[i] = cpPolyShapeNew(buggy_, 4, tv.data(),
                                           cpv(0.85 * x, 0.45 * y));
                cpShapeSetCollisionType(tread_[i], x < 0 ? coll_rear : coll_front);
                cpSpaceAddShape(space_, tread_[i]);
            }

        // Ice + dirt sensors (grip deltas -75 / -60 in userData).
        iceDelta_ = -75; dirtDelta_ = -60;
        const cpRectVects iceVects  = rectVects(cpv(-6, 2), cpv(6, 6));
        const cpRectVects dirtVects = rectVects(cpv(-W * aspect, -W),
                                                cpv(-0.5 * W * aspect, W));
        ice_ = cpPolyShapeNew(ground_, 4, const_cast<cpVect*>(iceVects.data()), cpvzero);
        cpShapeSetSensor(ice_, cpTrue);
        cpShapeSetCollisionType(ice_, coll_surface);
        cpShapeSetUserData(ice_, &iceDelta_);
        cpSpaceAddShape(space_, ice_);

        dirt_ = cpPolyShapeNew(ground_, 4, const_cast<cpVect*>(dirtVects.data()), cpvzero);
        cpShapeSetSensor(dirt_, cpTrue);
        cpShapeSetCollisionType(dirt_, coll_surface);
        cpShapeSetUserData(dirt_, &dirtDelta_);
        cpSpaceAddShape(space_, dirt_);

        // Surface collision handlers modulate each wheel's max force.
        frontCtx_ = {frontWheel_, 150};
        rearCtx_  = {rearWheel_, 150};
        cpSpaceAddCollisionHandler(space_, coll_front, coll_surface,
                                   onSurfaceBegin, nullptr, nullptr,
                                   onSurfaceSeparate, &frontCtx_);
        cpSpaceAddCollisionHandler(space_, coll_rear, coll_surface,
                                   onSurfaceBegin, nullptr, nullptr,
                                   onSurfaceSeparate, &rearCtx_);
    }

    ~OldSim() { cpSpaceFree(space_); }
    OldSim(const OldSim&) = delete;
    OldSim& operator=(const OldSim&) = delete;

    void applyPose(double x, double y, double angle) {
        cpBodySetPos(buggy_, cpv(x, y));
        cpBodySetAngle(buggy_, angle);
        cpBodySetVel(buggy_, cpvzero);
        cpBodySetAngVel(buggy_, 0);
        cpBodySetPos(steering_, cpBodyLocal2World(buggy_, cpv(0.7, 0)));
        cpBodySetAngle(steering_, angle);
        cpBodySetVel(steering_, cpvzero);
        cpBodySetAngVel(steering_, 0);
    }

    // One frame = 4 substeps of 0.25/60, re-anchoring the grooves each substep
    // (the original update()). Gravity is (gx, gy) held for the frame.
    void step(double gx, double gy) {
        cpSpaceSetGravity(space_, cpv(gx, gy));
        for (int i = 0; i < 4; ++i) {
            cpGrooveJointSetAnchr2(frontWheel_,
                                   cpBodyLocal2World(steering_, cpv(-0.1, 0)));
            cpGrooveJointSetAnchr2(rearWheel_,
                                   cpBodyLocal2World(buggy_, cpv(-0.65, 0)));
            cpSpaceStep(space_, 0.25 / 60.0);
        }
    }

    void pose(double& x, double& y, double& angle) const {
        cpVect p = cpBodyGetPos(buggy_);
        x = p.x; y = p.y; angle = cpBodyGetAngle(buggy_);
    }
    void grip(double& front, double& rear) const {
        front = frontCtx_.grip; rear = rearCtx_.grip;
    }

private:
    cpSpace* space_;
    cpBody *ground_, *buggy_, *steering_;
    cpConstraint *frontWheel_, *rearWheel_;
    cpShape* tread_[4];
    cpShape *ice_, *dirt_;
    int iceDelta_, dirtDelta_;
    SurfaceCtx frontCtx_, rearCtx_;
};

} // namespace oracle
