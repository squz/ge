// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::android Vulkan capability probe (🎯T107) — public surface.
//
// ge_vk_probe() is the device-distinguishing decision: pure C against
// libvulkan (resolved through vkGetInstanceProcAddr, so it links only that one
// 1.0 symbol and runs at low minSdk), no sokol/SDL deps. It compiles into both
// the standalone CLI (ge_vkprobe.c's main, gated by GE_VKPROBE_CLI) and libge,
// where SokolContext_android calls it before selecting a renderer backend.
//
// See ge_vkprobe.c for the contract rationale.

#ifndef GE_VKPROBE_H
#define GE_VKPROBE_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { GE_VK_ACCEPT = 0, GE_VK_REJECT = 1 } ge_vk_verdict;

// Per-requirement result, accumulated into a human + machine readable log.
typedef struct {
    ge_vk_verdict verdict;
    char          device_name[256];
    uint32_t      instance_version;
    uint32_t      device_api_version;
    int           has_descriptor_buffer_ext;
    int           feat_descriptor_buffer;
    int           feat_buffer_device_address;
    int           feat_dynamic_rendering;
    int           feat_synchronization2;
    int           has_graphics_queue;
    int           has_debug_utils_ext;   // informational: sokol labels objects
    char          reject_reasons[512];   // empty when ACCEPT
} ge_vk_result;

// Pure decision. Fills *out; returns its verdict. No surface checks (those run
// in-app via ge_vk_probe_surface). Safe to call on any device.
ge_vk_verdict ge_vk_probe(ge_vk_result* out);

// Surface half — runs in-app once an ANativeWindow → VkSurfaceKHR exists.
typedef struct {
    int  has_present_queue;
    int  has_usable_format;     // R8G8B8A8 or B8G8R8A8 UNORM/SRGB
    int  has_opaque_composite;
    int  has_color_attachment_usage;
    char reasons[256];
} ge_vk_surface_result;

int ge_vk_probe_surface(VkInstance inst, VkPhysicalDevice pd,
                        VkSurfaceKHR surface, ge_vk_surface_result* out);

#ifdef __cplusplus
}
#endif

#endif // GE_VKPROBE_H
