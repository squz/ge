// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Platform hook for MemoryFootprint.cpp. Not a public header.

#pragma once

#include <cstdint>

namespace ge {

struct FootprintSample {
    uint64_t current = 0;     // 0 = this platform cannot measure
    uint64_t kernelPeak = 0;  // 0 = no kernel-tracked peak
};

FootprintSample platformFootprintSample();

}  // namespace ge
