// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Differential physics harness. Runs the 2013 Chipmunk oracle (oracle.cpp) and
// the box2d Scene (../src/Scene.h) through identical scenarios — same start
// pose, same per-frame gravity — and measures how far the box2d trajectory
// drifts from the oracle, as a function of horizon (so we can tell ULP-linear
// drift from chaotic/bifurcation blow-ups and pick an honest epsilon).
//
//   harness [numScenarios] [frames] [threads]   — batch stats
//   harness study <seedHex> [frames]            — replay one scenario, verbose

#include "../src/Scene.h"   // box2d Scene (new)
#include "oracle.cpp"       // Chipmunk OldSim (old)

#include <box2d/box2d.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kHalfExtent = 10.0f;
constexpr float kAspect     = 1.5f;
constexpr int   kSegFrames  = 30;   // gravity changes every 0.5 s

// Horizons (frames) at which we sample divergence.
constexpr int   kH[]   = {1, 3, 10, 30, 60, 90};
constexpr int   kNH    = int(sizeof(kH) / sizeof(kH[0]));

// Semantic-classification thresholds.
constexpr double kMoveThresh    = 0.5;   // m/s — "the car is travelling"
constexpr double kSpinThresh    = 2.0;   // rad/s — "the car is fishtailing/spinning"
constexpr double kDivergeThresh = 45.0;  // deg — angle divergence we call "wild"

// b2CreateWorld/b2DestroyWorld race on the shared world pool across threads;
// serialise sim construction/destruction (cheap vs the stepping).
std::mutex g_makeMtx;

inline double wrapAngle(double a) { return std::atan2(std::sin(a), std::cos(a)); }

struct Sample {
    double pos[kNH];      // |pos_old - pos_new| (m)
    double ang[kNH];      // |heading_old - heading_new| (rad)
    double velDir[kNH];   // angle between travel directions (rad), -1 if either ~stationary
    double spinOld[kNH];  // |old spin rate| (rad/s) at the horizon
    double spinNew[kNH];  // |new spin rate| (rad/s)
};

// Deterministic per-scenario setup from a seed.
struct Scenario {
    double x0, y0, a0;
    std::mt19937_64 rng;
    explicit Scenario(uint64_t seed) : rng(seed) {
        std::uniform_real_distribution<double> uPos(-7.0, 7.0);
        std::uniform_real_distribution<double> uAng(-M_PI, M_PI);
        x0 = uPos(rng); y0 = uPos(rng); a0 = uAng(rng);
    }
    void gravityFor(int frame, double& gx, double& gy) {
        if (frame % kSegFrames == 0) {
            std::uniform_real_distribution<double> uDir(-M_PI, M_PI);
            std::uniform_real_distribution<double> uMag(0.0, 100.0);
            const double dir = uDir(rng), mag = uMag(rng);
            gx = mag * std::cos(dir); gy = mag * std::sin(dir);
        }
    }
};

// Run one scenario, sampling divergence at the fixed horizons.
Sample runScenario(uint64_t seed, int frames) {
    Scenario sc(seed);
    oracle::OldSim*  oldSim;
    tiltbuggy::Scene* newSim;
    {
        std::lock_guard<std::mutex> lk(g_makeMtx);
        oldSim = new oracle::OldSim(kHalfExtent, kAspect);
        newSim = new tiltbuggy::Scene(kHalfExtent, kAspect);
    }
    oldSim->applyPose(sc.x0, sc.y0, sc.a0);
    newSim->applyPose({float(sc.x0), float(sc.y0), float(sc.a0)});

    Sample s{};

    double gx = 0, gy = 0;
    double pox = sc.x0, poy = sc.y0, poa = sc.a0;  // prev old pose
    double pnx = sc.x0, pny = sc.y0, pna = sc.a0;  // prev new pose
    for (int f = 0; f < frames; ++f) {
        sc.gravityFor(f, gx, gy);
        oldSim->step(gx, gy);
        newSim->step(1.0f / 60.0f, b2Vec2{float(gx), float(gy)});
        double ox, oy, oa; oldSim->pose(ox, oy, oa);
        tiltbuggy::Pose np = newSim->buggyPose();

        // Finite-difference velocities (no sim perturbation).
        const double ovx = (ox - pox) * 60, ovy = (oy - poy) * 60;
        const double nvx = (np.x - pnx) * 60, nvy = (np.y - pny) * 60;
        const double oSpin = wrapAngle(oa - poa) * 60, nSpin = wrapAngle(np.angle - pna) * 60;
        const double oSpeed = std::hypot(ovx, ovy), nSpeed = std::hypot(nvx, nvy);
        const double pe = std::hypot(ox - np.x, oy - np.y);
        const double ae = std::fabs(wrapAngle(oa - np.angle));
        const double vd = (oSpeed > kMoveThresh && nSpeed > kMoveThresh)
            ? std::fabs(wrapAngle(std::atan2(ovy, ovx) - std::atan2(nvy, nvx)))
            : -1.0;
        for (int k = 0; k < kNH; ++k)
            if (f + 1 == kH[k]) {
                s.pos[k] = pe; s.ang[k] = ae; s.velDir[k] = vd;
                s.spinOld[k] = std::fabs(oSpin); s.spinNew[k] = std::fabs(nSpin);
            }
        pox = ox; poy = oy; poa = oa;
        pnx = np.x; pny = np.y; pna = np.angle;
    }
    { std::lock_guard<std::mutex> lk(g_makeMtx); delete newSim; delete oldSim; }
    return s;
}

uint64_t seedOf(uint64_t i) { return 0x9e3779b97f4a7c15ULL * (i + 1); }

double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = size_t(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

void batch(uint64_t total, int frames, unsigned nthreads) {
    std::printf("Differential harness: %llu scenarios × %d frames on %u threads\n",
                (unsigned long long)total, frames, nthreads);
    std::vector<Sample> all(total);
    std::atomic<uint64_t> next{0};
    auto worker = [&]() {
        for (;;) {
            uint64_t i = next.fetch_add(1);
            if (i >= total) break;
            all[i] = runScenario(seedOf(i), frames);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    std::printf("\nhorizon | pos div (m): p50 p90 p99 max | heading div (deg): p50 p90 p99 max | travel-dir div (deg): p50 p90 p99\n");
    for (int k = 0; k < kNH; ++k) {
        std::vector<double> P(total), A(total), V;
        for (uint64_t i = 0; i < total; ++i) {
            P[i] = all[i].pos[k]; A[i] = all[i].ang[k];
            if (all[i].velDir[k] >= 0) V.push_back(all[i].velDir[k]);
        }
        std::vector<double> P2 = P, A2 = A, V2 = V, V3 = V;
        std::printf("%3df    | %5.3f %5.3f %5.3f %5.3f | %6.2f %6.2f %6.2f %6.2f | %6.2f %6.2f %6.2f\n",
                    kH[k], pct(P, .5), pct(P2, .9), pct(P, .99),
                    *std::max_element(P.begin(), P.end()),
                    pct(A, .5) * 57.2958, pct(A2, .9) * 57.2958, pct(A, .99) * 57.2958,
                    *std::max_element(A.begin(), A.end()) * 57.2958,
                    V.empty() ? 0 : pct(V, .5) * 57.2958,
                    V2.empty() ? 0 : pct(V2, .9) * 57.2958,
                    V3.empty() ? 0 : pct(V3, .99) * 57.2958);
    }

    // Semantic classification of "wild" heading divergence (>45°) at each
    // horizon: bifurcation (BOTH cars spinning — chaos picks the direction, but
    // both fishtail = same game semantics) vs disagreement (only one spins = a
    // real behavioural split we'd want to explain).
    std::printf("\nwild-heading (>%.0f deg) breakdown — bifurcation vs disagreement:\n",
                kDivergeThresh);
    std::printf("horizon | wild%%  | of wild: both-spin(bifurcation)  one-spin(disagree)  neither\n");
    for (int k = 0; k < kNH; ++k) {
        uint64_t wild = 0, bifurc = 0, oneSpin = 0, neither = 0;
        for (uint64_t i = 0; i < total; ++i) {
            if (all[i].ang[k] * 57.2958 <= kDivergeThresh) continue;
            ++wild;
            const bool os = all[i].spinOld[k] > kSpinThresh;
            const bool ns = all[i].spinNew[k] > kSpinThresh;
            if (os && ns) ++bifurc; else if (os || ns) ++oneSpin; else ++neither;
        }
        if (!wild) { std::printf("%3df    |  0.0%%  | (none)\n", kH[k]); continue; }
        std::printf("%3df    | %4.1f%%  | %6.1f%%                       %6.1f%%             %6.1f%%\n",
                    kH[k], 100.0 * wild / total, 100.0 * bifurc / wild,
                    100.0 * oneSpin / wild, 100.0 * neither / wild);
    }

    // Worst 6 "one-spin disagreement" cases @30f — these are the ones worth
    // studying (a genuine behavioural split, not a chaotic bifurcation).
    int h30 = 0; for (int k = 0; k < kNH; ++k) if (kH[k] == 30) h30 = k;
    std::vector<std::pair<double, uint64_t>> dis;
    for (uint64_t i = 0; i < total; ++i) {
        const bool os = all[i].spinOld[h30] > kSpinThresh;
        const bool ns = all[i].spinNew[h30] > kSpinThresh;
        if (all[i].ang[h30] * 57.2958 > kDivergeThresh && (os != ns))
            dis.push_back({all[i].ang[h30], seedOf(i)});
    }
    std::partial_sort(dis.begin(), dis.begin() + std::min<size_t>(6, dis.size()),
                      dis.end(), [](auto& a, auto& b){ return a.first > b.first; });
    std::printf("\nworst %zu one-spin disagreements @30f (harness study <seed>):\n",
                std::min<size_t>(6, dis.size()));
    for (size_t k = 0; k < std::min<size_t>(6, dis.size()); ++k)
        std::printf("  %016llx  %.1f deg\n", (unsigned long long)dis[k].second,
                    dis[k].first * 57.2958);
}

void study(uint64_t seed, int frames) {
    Scenario sc(seed);
    oracle::OldSim  oldSim(kHalfExtent, kAspect);
    tiltbuggy::Scene newSim(kHalfExtent, kAspect);
    oldSim.applyPose(sc.x0, sc.y0, sc.a0);
    newSim.applyPose({float(sc.x0), float(sc.y0), float(sc.a0)});
    std::printf("seed %016llx  start pose (%.2f,%.2f,%.1fdeg)\n",
                (unsigned long long)seed, sc.x0, sc.y0, sc.a0 * 57.2958);
    std::printf("frame  g=(gx,gy)      OLD(x,y,deg)          NEW(x,y,deg)        dPos  dAng\n");
    double gx = 0, gy = 0;
    for (int f = 0; f < frames; ++f) {
        sc.gravityFor(f, gx, gy);
        oldSim.step(gx, gy);
        newSim.step(1.0f / 60.0f, b2Vec2{float(gx), float(gy)});
        double ox, oy, oa; oldSim.pose(ox, oy, oa);
        tiltbuggy::Pose np = newSim.buggyPose();
        double of, orr; oldSim.grip(of, orr);
        tiltbuggy::GripState ng = newSim.gripState();
        const double pe = std::hypot(ox - np.x, oy - np.y);
        const double ae = std::fabs(wrapAngle(oa - np.angle)) * 57.2958;
        if (f < 12 || f % 10 == 0 || ae > 20)
            std::printf("%4d  (%+6.1f,%+6.1f)  (%+6.2f,%+6.2f,%+6.1f)  (%+6.2f,%+6.2f,%+6.1f)  %5.2f %6.1f  [grip o=%.0f/%.0f n=%.0f/%.0f]\n",
                        f, gx, gy, ox, oy, oa * 57.2958, np.x, np.y, np.angle * 57.2958,
                        pe, ae, of, orr, ng.front, ng.rear);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "study") == 0) {
        uint64_t seed = std::strtoull(argv[2], nullptr, 16);
        int frames = (argc > 3) ? std::atoi(argv[3]) : 90;
        study(seed, frames);
        return 0;
    }
    const uint64_t total = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 1000;
    const int frames     = (argc > 2) ? std::atoi(argv[2]) : 90;
    const unsigned nthreads = (argc > 3) ? std::atoi(argv[3])
                            : std::max(1u, std::thread::hardware_concurrency());
    batch(total, frames, nthreads);
    return 0;
}
