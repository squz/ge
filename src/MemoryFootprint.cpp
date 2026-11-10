// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Peak tracking and the spyder-facing gauges. The bytes come from
// platformFootprintSample(), one definition per platform.

#include <ge/MemoryFootprint.h>

#include <ge/appchannel.h>
#include <ge/metrics.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include "MemoryFootprint_internal.h"

namespace ge {
namespace {

// Not a live session id (those start at 1 and count up). Keeps this scope
// out of Scope::all(sessionId), so a game's single scope stays the default
// metrics_dump target. Address it by id "ge-memory".
constexpr uint32_t kEngineMemorySession = 0xFFFFFFF0u;

std::atomic<uint64_t> g_peak{0};

uint64_t notePeak(const FootprintSample& s) {
    uint64_t obs = s.current;
    if (s.kernelPeak > obs) obs = s.kernelPeak;
    uint64_t prev = g_peak.load(std::memory_order_relaxed);
    while (obs > prev &&
           !g_peak.compare_exchange_weak(prev, obs, std::memory_order_relaxed)) {
    }
    return g_peak.load(std::memory_order_relaxed);
}

struct Gauges {
    metrics::Scope scope;
    metrics::metric<double> current;
    metrics::metric<double> peak;
    Gauges()
        : scope("ge-memory", kEngineMemorySession)
        , current(scope, "mem_footprint_bytes")
        , peak(scope, "mem_footprint_peak_bytes") {
        scope.arm({"mem_footprint_bytes", "mem_footprint_peak_bytes"}, 3600);
    }
};

Gauges& gauges() {
    static Gauges g;
    return g;
}

}  // namespace

uint64_t physicalFootprintBytes() {
    const FootprintSample s = platformFootprintSample();
    notePeak(s);
    return s.current;
}

uint64_t physicalFootprintPeakBytes() {
    const FootprintSample s = platformFootprintSample();
    return notePeak(s);
}

void publishMemoryFootprint() {
    const FootprintSample s = platformFootprintSample();
    const uint64_t peak = notePeak(s);
    appchannel::perfEmit("mem_footprint_bytes", static_cast<double>(s.current));
    appchannel::perfEmit("mem_footprint_peak_bytes", static_cast<double>(peak));
    auto& g = gauges();
    g.current = static_cast<double>(s.current);
    g.peak = static_cast<double>(peak);
    g.scope.endFrame();
}

namespace {

std::chrono::steady_clock::time_point g_lastFootprintSample =
    std::chrono::steady_clock::time_point::min();

}  // namespace

bool sampleMemoryFootprintIfDue() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    if (!detail::memoryFootprintSampleDue(now, g_lastFootprintSample)) return false;
    g_lastFootprintSample = now;
    publishMemoryFootprint();
    return true;
}

void resetMemoryFootprintSampleClockForTest() {
    g_lastFootprintSample = std::chrono::steady_clock::time_point::min();
}

}  // namespace ge
