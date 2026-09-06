// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Shared adapter plumbing for the TiltBuggy differential-harness instance
// (see ~/think/differential-harness/SPEC.md). Both adapters read a JSONL tape
// on stdin — a `{"@init": {x, y, angle}}` preamble then one `{gx, gy}` gravity
// record per frame — and write one JSONL state record per frame: chassis pose,
// finite-difference speed/spin, per-axle grip, and a `spinning` flag (the
// semantic bit the old harness used to separate chaotic bifurcation from real
// disagreement).
#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <string>

namespace dh {

constexpr float  kHalfExtent = 10.0f;   // arena y half-extent (matches Renderer)
constexpr float  kAspect     = 1.5f;    // arena aspect (x = y·aspect)
constexpr double kFps        = 60.0;    // one tape tick = one 60 Hz frame
constexpr double kSpinThresh = 2.0;     // rad/s — "the car is fishtailing/spinning"

struct Pose { double x, y, angle; };
struct Grip { double front, rear; };

inline double wrapAngle(double a) { return std::atan2(std::sin(a), std::cos(a)); }

// Sim is the adapter's wrapper over one physics engine:
//   void applyPose(const Pose&);
//   void step(double gx, double gy);
//   Pose pose() const;
//   Grip grip() const;
template <class Sim>
int runAdapter(Sim& sim) {
    Pose prev{0, 0, 0};
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const auto rec = nlohmann::json::parse(line);
        if (rec.contains("@init")) {
            const auto& in = rec["@init"];
            prev = Pose{in.value("x", 0.0), in.value("y", 0.0), in.value("angle", 0.0)};
            sim.applyPose(prev);
            continue;
        }
        sim.step(rec.value("gx", 0.0), rec.value("gy", 0.0));
        const Pose p = sim.pose();
        const Grip g = sim.grip();
        const double vx = (p.x - prev.x) * kFps, vy = (p.y - prev.y) * kFps;
        const double spin = wrapAngle(p.angle - prev.angle) * kFps;
        nlohmann::json out = {
            {"x", p.x}, {"y", p.y}, {"angle", p.angle},
            {"speed", std::hypot(vx, vy)}, {"spin", spin},
            {"grip_front", g.front}, {"grip_rear", g.rear},
            {"spinning", std::fabs(spin) > kSpinThresh ? 1 : 0},
        };
        std::cout << out.dump() << '\n';
        prev = p;
    }
    return 0;
}

} // namespace dh
