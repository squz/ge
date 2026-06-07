// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// libgesokol-vk.so (🎯T107) — the Vulkan sokol_gfx backend behind exported
// entrypoints. Mirror of SokolBackendGLES_android.c: all 150 sg_* and sokol's
// Vulkan impl compile here with hidden visibility. libge owns the
// VkInstance/device/swapchain glue and hands the handles to sg_setup() via the
// public sg_environment / sg_swapchain structs — this .so owns no window/surface,
// only the renderer.
//
// Two exported symbols:
//   ge_sokol_bind_vk()        → returns this .so's sg_* table (the dispatch shim
//                               in libge installs it via ge_sokol_set_api).
//   ge_sokol_vk_init_loader() → resolves sokol's Vulkan function pointers (see
//                               ge_vk_loader.h). libge calls it once after the
//                               VkInstance exists, before sg_setup.
//
// Loaded only on devices the capability probe accepts (ge_vk_probe). Because the
// loader dlopen()s libvulkan.so rather than linking it, this .so has no hard
// Vulkan link dependency — it cannot fail to load for a missing Vulkan symbol,
// and FALLBACK_GLES devices that never touch it pay nothing.

// VK_NO_PROTOTYPES must precede ANY vulkan.h include so every vk* becomes a
// function-pointer we own (resolved by ge_sokol_vk_init_loader), not a linked
// symbol the Android libvulkan.so stub may not export.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stddef.h>

#include "ge_vk_loader.h"   // GE_VK_FUNCS list + ge_sokol_vk_init_loader decl

// Global storage for sokol's Vulkan entry points. Defined BEFORE sokol's IMPL
// so its vk* calls bind to these symbols. The bootstrap pointer is separate
// because it is itself resolved via dlsym, not via the table.
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = NULL;
#define X(name) PFN_##name name = NULL;
GE_VK_FUNCS(X)
#undef X

__attribute__((visibility("default")))
void ge_sokol_vk_init_loader(VkInstance instance) {
    if (vkGetInstanceProcAddr == NULL) {
        void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (lib == NULL) return;  // no Vulkan loader — caller must fall back
        vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        if (vkGetInstanceProcAddr == NULL) return;
    }
    // vkGetInstanceProcAddr resolves instance-, physical-device-, AND
    // device-level commands, so one flat loop covers the whole table.
#define X(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
    GE_VK_FUNCS(X)
#undef X
}

#define SOKOL_VULKAN
#include "ge_sokol_dispatch.h"  // ge_sg_api, ge_sokol_set_api + sokol_gfx.h API

#define SOKOL_IMPL
#include "sokol_gfx.h"          // IMPL only (public section already guarded)

// Returns this .so's sg_* table; libge's SokolContext installs it via its own
// ge_sokol_set_api. Returning (rather than calling back) keeps the .so free of
// any libge symbol dependency — it exports its two symbols and imports none.
__attribute__((visibility("default")))
const ge_sg_api* ge_sokol_bind_vk(void) {
    static ge_sg_api a;
    ge_sg_api* _a = &a;
#include "ge_sokol_fill.inc"     // _a->sg_X = sg_X;  (this .so's hidden impl)
    return &a;
}
