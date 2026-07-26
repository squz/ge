// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Android impl: sysinfo() totalram * mem_unit reports total physical RAM.

#include <cstdint>
#include <sys/sysinfo.h>

namespace ge {

uint64_t platformPhysRamBytes() {
    struct sysinfo info {};
    if (sysinfo(&info) != 0) return 0;
    return uint64_t(info.totalram) * uint64_t(info.mem_unit);
}

} // namespace ge
