// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include <ge/MemoryFootprint.h>
#include <ge/SessionHost.h>
#include <ge/metrics.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "appchannel_internal.h"

TEST_CASE("a fresh footprint sample clock publishes into the sole live session") {
    // Live desktop miss: perfTick's gate starts at steady_clock::time_point::min().
    // now - min() overflows duration to a large negative, the comparison with
    // 1s is false forever, publishMemoryFootprint never runs, and spyder's
    // samples lack mem_footprint_* while app_metrics_dump(instance=ge-memory)
    // reports "unknown instance" because the scope is constructed on publish.
    ge::appchannel::detail::resetAppChannelStateForTest();
    ge::resetMemoryFootprintSampleClockForTest();

    using clock = std::chrono::steady_clock;
    // A compile-time time_point::min() is folded at -O2 and hides the
    // signed-duration overflow. Force a runtime stamp, which is what the
    // static clock in sampleMemoryFootprintIfDue actually holds.
    clock::time_point unset = clock::time_point::min();
    volatile clock::rep stamp = unset.time_since_epoch().count();
    unset = clock::time_point(clock::duration(stamp));
    CHECK(ge::detail::memoryFootprintSampleDue(clock::now(), unset));

    ge::Context ctx(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    REQUIRE(ge::sampleMemoryFootprintIfDue());

    const auto snap = ge::appchannel::detail::perfCountersSnapshotForTest(ctx);
    REQUIRE(snap.contains("mem_footprint_bytes"));
    REQUIRE(snap.contains("mem_footprint_peak_bytes"));
    CHECK(snap["mem_footprint_bytes"].get<double>() > 1.0e6);
    CHECK(snap["mem_footprint_peak_bytes"].get<double>() >=
          snap["mem_footprint_bytes"].get<double>());

    REQUIRE(ge::metrics::Scope::find("ge-memory") != nullptr);

    // Inside the same second the gate stays closed and does not clear counters.
    CHECK_FALSE(ge::sampleMemoryFootprintIfDue());
    CHECK(ge::appchannel::detail::perfCountersSnapshotForTest(ctx).contains(
        "mem_footprint_bytes"));

    ge::resetMemoryFootprintSampleClockForTest();
    ge::appchannel::detail::resetAppChannelStateForTest();
}

TEST_CASE("physical footprint is positive and grows when a large buffer is touched") {
    const uint64_t before = ge::physicalFootprintBytes();
    REQUIRE(before > 0);

    constexpr std::size_t kBytes = 64u * 1024u * 1024u;
    std::vector<unsigned char> buf(kBytes);
    for (std::size_t i = 0; i < kBytes; i += 4096)
        buf[i] = 1;

    const uint64_t after = ge::physicalFootprintBytes();
    unsigned touched = 0;
    for (std::size_t i = 0; i < kBytes; i += 4096)
        touched += buf[i];
    CHECK(touched > 0);  // the pages stay live across the sample above
    CHECK(after >= before + 32u * 1024u * 1024u);

    const uint64_t peak = ge::physicalFootprintPeakBytes();
    CHECK(peak >= after);
    (void)buf[0];
}

TEST_CASE("publishMemoryFootprint feeds perf counters and the ge-memory metrics ring") {
    ge::appchannel::detail::resetAppChannelStateForTest();

    ge::publishMemoryFootprint();

    const auto snap = ge::appchannel::detail::perfCountersSnapshotForTest();
    REQUIRE(snap.contains("mem_footprint_bytes"));
    REQUIRE(snap.contains("mem_footprint_peak_bytes"));
    CHECK(snap["mem_footprint_bytes"].get<double>() > 0);
    CHECK(snap["mem_footprint_peak_bytes"].get<double>() >=
          snap["mem_footprint_bytes"].get<double>());

    auto* scope = ge::metrics::Scope::find("ge-memory");
    REQUIRE(scope != nullptr);
    const auto dumped = scope->dump();
    REQUIRE(dumped["series"].size() == 2);
    CHECK(dumped["series"][0] == "mem_footprint_bytes");
    CHECK(dumped["series"][1] == "mem_footprint_peak_bytes");
    REQUIRE(dumped["count"].get<int>() >= 1);
    CHECK(dumped["frames"].back()[0].get<double>() > 0);
    CHECK(dumped["frames"].back()[1].get<double>() >= dumped["frames"].back()[0].get<double>());

    ge::appchannel::detail::resetAppChannelStateForTest();
}
