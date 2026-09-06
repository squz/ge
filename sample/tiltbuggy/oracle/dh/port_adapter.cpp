// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// PORT adapter: the box2d tiltbuggy::Scene driven by a dh tape.
//
// DH_PLANT selects a deliberately broken variant so `dh selftest` can measure
// the oracle's catch rate (oracle-first rule 12 — test the oracle, not just the
// code). Each plant is a *structural* port error of a kind that has actually
// happened in engine ports, never a nudged constant:
//   axis-swap   gravity axes transposed (coordinate-convention bug)
//   substep-8   two half-frame steps per frame (integration structure changed)
//   pose-lag    state reported one frame late (off-by-one in the sampling path)

#include "dh_common.h"
#include "Scene.h"

#include <box2d/box2d.h>

#include <cstdlib>
#include <string>

namespace {

constexpr float kFrameDt = 1.0f / float(dh::kFps);

struct PortSim {
    tiltbuggy::Scene scene{dh::kHalfExtent, dh::kAspect};
    std::string plant = std::getenv("DH_PLANT") ? std::getenv("DH_PLANT") : "";
    dh::Pose lagged{};   // pose-lag: the previous frame's pose
    dh::Grip laggedGrip{};

    void applyPose(const dh::Pose& p) {
        scene.applyPose({float(p.x), float(p.y), float(p.angle)});
        lagged = p;
        laggedGrip = current().second;
    }
    void step(double gx, double gy) {
        if (plant == "axis-swap") std::swap(gx, gy);
        const b2Vec2 g{float(gx), float(gy)};
        if (plant == "substep-8") {
            scene.step(kFrameDt / 2, g);
            scene.step(kFrameDt / 2, g);
        } else {
            scene.step(kFrameDt, g);
        }
    }
    std::pair<dh::Pose, dh::Grip> current() const {
        const tiltbuggy::Pose p = scene.buggyPose();
        const tiltbuggy::GripState g = scene.gripState();
        return {dh::Pose{p.x, p.y, p.angle}, dh::Grip{g.front, g.rear}};
    }
    dh::Pose pose() {
        auto [p, g] = current();
        if (plant != "pose-lag") return p;
        std::swap(p, lagged);
        laggedGrip = g;
        return p;
    }
    dh::Grip grip() const {
        return plant == "pose-lag" ? laggedGrip : current().second;
    }
};

} // namespace

int main() {
    PortSim sim;
    return dh::runAdapter(sim);
}
