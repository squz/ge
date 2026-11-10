// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// iOS and macOS footprint: task_info(TASK_VM_INFO) phys_footprint, which
// is what jetsam compares against the process limit. ledger_phys_footprint_peak
// (task_vm_info rev3) is the kernel's high-water mark since launch.

#include <cstdint>

#include <mach/mach.h>

#include "MemoryFootprint_internal.h"

namespace ge {

FootprintSample platformFootprintSample() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                        reinterpret_cast<task_info_t>(&info), &count);
    if (kr != KERN_SUCCESS) return {};

    FootprintSample s;
    if (count >= TASK_VM_INFO_REV1_COUNT)
        s.current = info.phys_footprint;
    if (count >= TASK_VM_INFO_REV3_COUNT && info.ledger_phys_footprint_peak > 0)
        s.kernelPeak = static_cast<uint64_t>(info.ledger_phys_footprint_peak);
    return s;
}

}  // namespace ge
