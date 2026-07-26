// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Apple impl: hw.memsize via sysctlbyname reports total physical RAM
// (macOS desktop, iOS device, iOS simulator — the simulator reports the
// host Mac's RAM, which is fine since it only feeds a coarse tier decision).

#include <cstdint>
#include <sys/sysctl.h>

namespace ge {

uint64_t platformPhysRamBytes() {
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) return 0;
    return bytes;
}

} // namespace ge
