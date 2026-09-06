// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// REFERENCE adapter: the 2013 Chipmunk TiltBuggy physics (../oracle.cpp,
// extracted verbatim from ViewController.mm) driven by a dh tape.

#include "dh_common.h"
#include "../oracle.cpp"

namespace {

struct RefSim {
    oracle::OldSim sim{dh::kHalfExtent, dh::kAspect};

    void applyPose(const dh::Pose& p) { sim.applyPose(p.x, p.y, p.angle); }
    void step(double gx, double gy) { sim.step(gx, gy); }
    dh::Pose pose() const {
        dh::Pose p{};
        sim.pose(p.x, p.y, p.angle);
        return p;
    }
    dh::Grip grip() const {
        dh::Grip g{};
        sim.grip(g.front, g.rear);
        return g;
    }
};

} // namespace

int main() {
    RefSim sim;
    return dh::runAdapter(sim);
}
