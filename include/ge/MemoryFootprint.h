// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Process physical-memory footprint. The number itself is available in
// every build, including release. Publishing it where spyder can read it
// (app-channel perf counters and the metrics ring) happens from
// ge::publishMemoryFootprint, which the debug app-channel calls about
// once a second. perfEmit is a no-op in release; the queries below are not.

#pragma once

#include <cstdint>

namespace ge {

// Current physical footprint of this process, in bytes.
//
// Apple (iOS and macOS): task_info(TASK_VM_INFO) phys_footprint, the
// figure jetsam uses. Not RSS.
// Android: proportional set size (Pss) from /proc/self/smaps_rollup.
// If that file cannot be read, resident set size from /proc/self/statm.
// Any other platform: 0. Zero means "not measured", not an empty process.
uint64_t physicalFootprintBytes();

// Highest footprint seen since this process started, in bytes.
// On Apple this includes ledger_phys_footprint_peak from the same
// task_info call, so a peak between samples is kept. Elsewhere it is the
// max of physicalFootprintBytes() results in this process. 0 means no
// measurement has succeeded yet.
uint64_t physicalFootprintPeakBytes();

// Sample the footprint, update the peak, and publish both gauges:
//   perf counters mem_footprint_bytes and mem_footprint_peak_bytes
//     (ge::appchannel::perfEmit; the debug perf push sends them about
//     once a second as samples alongside frame_ms)
//   metrics scope id "ge-memory", series of the same two names, one
//     sample per call (app_metrics_dump with {"instance":"ge-memory"})
// The SessionHost debug loop calls this at most once a second from
// perfTick. Release builds still update the peak; perfEmit is a no-op
// there. Safe to call from the game thread.
void publishMemoryFootprint();

}  // namespace ge
