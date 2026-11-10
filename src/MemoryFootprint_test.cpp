// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include <ge/MemoryFootprint.h>
#include <ge/metrics.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "appchannel_internal.h"

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
