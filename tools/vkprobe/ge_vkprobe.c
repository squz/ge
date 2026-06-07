// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::android Vulkan capability probe (🎯T107).
//
// Decides — BEFORE any renderer backend is loaded — whether a device can run
// ge's sokol_gfx Vulkan backend, or must fall back to GLES3. The contract
// mirrors what current vendored sokol_gfx actually demands at sg_setup():
//
//   • Vulkan instance >= 1.1            (needed to even call Features2)
//   • physical-device API   >= 1.3      (sokol uses 1.3-era entry points)
//   • VK_EXT_descriptor_buffer extension AND descriptorBuffer feature
//   • bufferDeviceAddress feature       (Vulkan 1.2)
//   • dynamicRendering    feature       (Vulkan 1.3)
//   • synchronization2    feature       (Vulkan 1.3)
//   • a graphics-capable queue family
//
// Surface-dependent checks (present queue, surface format, composite alpha,
// swapchain usage + minimal swapchain viability) require a live ANativeWindow
// and so run IN-APP just before sg_setup; this standalone probe covers the
// device-distinguishing capability half — which is exactly what separates the
// multimaze2 experiment devices (Pixel Tablet / ASUS accept; S21 is Vulkan 1.1,
// Tab A9 lacks descriptor_buffer → both GLES).
//
// This file is the single source of the decision: `ge_vk_probe()` is pure C
// against libvulkan with no sokol/SDL deps, so it compiles into both the
// standalone CLI (this file's main) and, later, libgevkprobe.so for GeActivity
// to dlopen before getLibraries().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

// Extension name macros may be absent on very old headers; pin literals.
#ifndef VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
#define VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME "VK_EXT_descriptor_buffer"
#endif
#ifndef VK_EXT_DEBUG_UTILS_EXTENSION_NAME
#define VK_EXT_DEBUG_UTILS_EXTENSION_NAME "VK_EXT_debug_utils"
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

static void ver_str(uint32_t v, char* out, size_t n) {
    snprintf(out, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v),
             VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

static void add_reason(ge_vk_result* r, const char* why) {
    if (r->reject_reasons[0]) strncat(r->reject_reasons, "; ",
                                      sizeof r->reject_reasons - strlen(r->reject_reasons) - 1);
    strncat(r->reject_reasons, why, sizeof r->reject_reasons - strlen(r->reject_reasons) - 1);
}

static int has_ext(const VkExtensionProperties* exts, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; ++i)
        if (strcmp(exts[i].extensionName, name) == 0) return 1;
    return 0;
}

// Vulkan 1.1+ entry points are NOT exported by the Android `libvulkan.so` stub
// at low API levels (the platform only stubs 1.0). Directly linking them forces
// minSdk up to the platform that first stubbed them — the "forced API 35"
// problem the experiment hit. We instead resolve everything past vkCreateInstance
// through vkGetInstanceProcAddr (the one symbol we link, 1.0), so a single
// low-minSdk probe binary runs everywhere and simply rejects devices whose
// runtime returns NULL for a 1.1 entry point.
static PFN_vkEnumerateInstanceVersion          p_enumInstanceVersion;
static PFN_vkCreateInstance                     p_createInstance;
static PFN_vkDestroyInstance                    p_destroyInstance;
static PFN_vkEnumeratePhysicalDevices           p_enumPhysical;
static PFN_vkGetPhysicalDeviceProperties        p_getProps;
static PFN_vkEnumerateDeviceExtensionProperties p_enumDevExt;
static PFN_vkGetPhysicalDeviceFeatures2         p_getFeatures2;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_getQueueFams;

#define LOAD(inst, var, name) (var = (PFN_##name)vkGetInstanceProcAddr((inst), #name))

// Evaluate one physical device against the contract. Returns 1 if it satisfies
// every Vulkan-accept requirement.
static int eval_device(VkPhysicalDevice pd, ge_vk_result* r) {
    VkPhysicalDeviceProperties props;
    p_getProps(pd, &props);
    snprintf(r->device_name, sizeof r->device_name, "%s", props.deviceName);
    r->device_api_version = props.apiVersion;

    int ok = 1;

    // 1) physical-device API >= 1.3
    if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
        (VK_API_VERSION_MAJOR(props.apiVersion) == 1 &&
         VK_API_VERSION_MINOR(props.apiVersion) < 3)) {
        ok = 0; add_reason(r, "device Vulkan < 1.3");
    }

    // 2) device extensions: descriptor_buffer (required), debug_utils (info)
    uint32_t ext_n = 0;
    p_enumDevExt(pd, NULL, &ext_n, NULL);
    VkExtensionProperties* exts = calloc(ext_n ? ext_n : 1, sizeof *exts);
    p_enumDevExt(pd, NULL, &ext_n, exts);
    r->has_descriptor_buffer_ext = has_ext(exts, ext_n, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    r->has_debug_utils_ext       = has_ext(exts, ext_n, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    free(exts);
    if (!r->has_descriptor_buffer_ext) { ok = 0; add_reason(r, "no VK_EXT_descriptor_buffer"); }

    // 3) feature chain: descriptorBuffer + bufferDeviceAddress + dynamicRendering + synchronization2
    VkPhysicalDeviceDescriptorBufferFeaturesEXT dbf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &dbf };
    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13 };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12 };
    p_getFeatures2(pd, &f2);
    r->feat_descriptor_buffer     = dbf.descriptorBuffer ? 1 : 0;
    r->feat_buffer_device_address = v12.bufferDeviceAddress ? 1 : 0;
    r->feat_dynamic_rendering     = v13.dynamicRendering ? 1 : 0;
    r->feat_synchronization2      = v13.synchronization2 ? 1 : 0;
    if (!r->feat_descriptor_buffer)     { ok = 0; add_reason(r, "no descriptorBuffer feature"); }
    if (!r->feat_buffer_device_address) { ok = 0; add_reason(r, "no bufferDeviceAddress feature"); }
    if (!r->feat_dynamic_rendering)     { ok = 0; add_reason(r, "no dynamicRendering feature"); }
    if (!r->feat_synchronization2)      { ok = 0; add_reason(r, "no synchronization2 feature"); }

    // 4) a graphics-capable queue family
    uint32_t q_n = 0;
    p_getQueueFams(pd, &q_n, NULL);
    VkQueueFamilyProperties* qs = calloc(q_n ? q_n : 1, sizeof *qs);
    p_getQueueFams(pd, &q_n, qs);
    for (uint32_t i = 0; i < q_n; ++i)
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { r->has_graphics_queue = 1; break; }
    free(qs);
    if (!r->has_graphics_queue) { ok = 0; add_reason(r, "no graphics queue family"); }

    return ok;
}

// Pure decision. Fills *out; returns its verdict. No surface checks (those run
// in-app). Safe to call on any device — it only *queries* 1.3 features, so it
// runs fine on a Vulkan 1.1 device (which it then rejects).
ge_vk_verdict ge_vk_probe(ge_vk_result* out) {
    memset(out, 0, sizeof *out);
    out->verdict = GE_VK_REJECT;

    // Global commands resolve with a NULL instance. A NULL result for
    // vkEnumerateInstanceVersion means a 1.0 loader → no Features2 → reject.
    LOAD(VK_NULL_HANDLE, p_enumInstanceVersion, vkEnumerateInstanceVersion);
    LOAD(VK_NULL_HANDLE, p_createInstance,      vkCreateInstance);
    if (!p_createInstance) { add_reason(out, "no vkCreateInstance"); return GE_VK_REJECT; }
    if (p_enumInstanceVersion) p_enumInstanceVersion(&out->instance_version);
    else                       out->instance_version = VK_API_VERSION_1_0;
    if (VK_API_VERSION_MAJOR(out->instance_version) < 1 ||
        (VK_API_VERSION_MAJOR(out->instance_version) == 1 &&
         VK_API_VERSION_MINOR(out->instance_version) < 1)) {
        add_reason(out, "instance Vulkan < 1.1 (no Features2)");
        return GE_VK_REJECT;
    }

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ge-vkprobe",
                              .apiVersion = out->instance_version };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    VkInstance inst = VK_NULL_HANDLE;
    if (p_createInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        add_reason(out, "vkCreateInstance failed");
        return GE_VK_REJECT;
    }

    // Instance-level commands resolve against the live instance.
    LOAD(inst, p_destroyInstance, vkDestroyInstance);
    LOAD(inst, p_enumPhysical,    vkEnumeratePhysicalDevices);
    LOAD(inst, p_getProps,        vkGetPhysicalDeviceProperties);
    LOAD(inst, p_enumDevExt,      vkEnumerateDeviceExtensionProperties);
    LOAD(inst, p_getFeatures2,    vkGetPhysicalDeviceFeatures2);
    LOAD(inst, p_getQueueFams,    vkGetPhysicalDeviceQueueFamilyProperties);
    if (!p_enumPhysical || !p_getProps || !p_enumDevExt || !p_getFeatures2 ||
        !p_getQueueFams) {
        add_reason(out, "missing Vulkan 1.1 instance entry points");
        if (p_destroyInstance) p_destroyInstance(inst, NULL);
        return GE_VK_REJECT;
    }

    uint32_t pd_n = 0;
    p_enumPhysical(inst, &pd_n, NULL);
    if (pd_n == 0) { add_reason(out, "no physical devices"); if (p_destroyInstance) p_destroyInstance(inst, NULL); return GE_VK_REJECT; }
    VkPhysicalDevice* pds = calloc(pd_n, sizeof *pds);
    p_enumPhysical(inst, &pd_n, pds);

    // Accept iff any device satisfies the full contract; report that device,
    // else report the first device's failing reasons.
    ge_vk_verdict v = GE_VK_REJECT;
    for (uint32_t i = 0; i < pd_n; ++i) {
        ge_vk_result tmp; memset(&tmp, 0, sizeof tmp);
        tmp.instance_version = out->instance_version;
        if (eval_device(pds[i], &tmp)) { *out = tmp; out->verdict = GE_VK_ACCEPT; v = GE_VK_ACCEPT; break; }
        if (i == 0) { *out = tmp; }   // keep first device's reasons for the log
    }
    out->verdict = v;
    free(pds);
    p_destroyInstance(inst, NULL);
    return v;
}

int main(void) {
    ge_vk_result r;
    ge_vk_verdict v = ge_vk_probe(&r);

    char iv[32], dv[32];
    ver_str(r.instance_version, iv, sizeof iv);
    ver_str(r.device_api_version, dv, sizeof dv);

    printf("device=\"%s\"\n", r.device_name);
    printf("instance_version=%s device_api=%s\n", iv, dv);
    printf("ext.descriptor_buffer=%d ext.debug_utils=%d\n",
           r.has_descriptor_buffer_ext, r.has_debug_utils_ext);
    printf("feat.descriptorBuffer=%d feat.bufferDeviceAddress=%d "
           "feat.dynamicRendering=%d feat.synchronization2=%d\n",
           r.feat_descriptor_buffer, r.feat_buffer_device_address,
           r.feat_dynamic_rendering, r.feat_synchronization2);
    printf("queue.graphics=%d\n", r.has_graphics_queue);
    // The line the selector / test harness greps:
    if (v == GE_VK_ACCEPT)
        printf("GE_VK_VERDICT=ACCEPT_VULKAN\n");
    else
        printf("GE_VK_VERDICT=FALLBACK_GLES reasons=[%s]\n", r.reject_reasons);
    return (int)v;   // 0 = accept, 1 = fallback
}
