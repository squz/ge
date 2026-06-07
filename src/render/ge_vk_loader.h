// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge_vk_loader.h (🎯T107) — volk-style dynamic Vulkan loader for
// libgesokol-vk.so. sokol's Vulkan backend calls 1.1/1.2/1.3 core functions
// (vkGetPhysicalDeviceProperties2, vkGetBufferDeviceAddress, vkCmdBeginRendering,
// vkCmdPipelineBarrier2, vkCmdCopyBufferToImage2, …) as plain symbols. Android's
// on-device libvulkan.so loader only exports those core entry points on recent
// OS versions (1.3 core lands in the Android 13+ loader), so a .so that links
// them directly would fail to dlopen on capable-driver-but-older-OS devices.
//
// Fix: compile the backend .so with VK_NO_PROTOTYPES and resolve every vk*
// through this table. vkGetInstanceProcAddr is bootstrapped via
// dlopen("libvulkan.so") + dlsym (so the .so carries NO hard libvulkan link —
// it cannot fail to load for a missing Vulkan symbol), then every other command
// is resolved through vkGetInstanceProcAddr(instance, name). Per the Vulkan
// spec, vkGetInstanceProcAddr returns valid pointers for instance-, physical-
// device-, AND device-level commands (device commands dispatch through the
// loader trampoline), so no per-function instance/device classification is
// needed — one flat list, one resolve loop.
//
// GE_VK_FUNCS is the authoritative list, derived from `llvm-nm -u` over the
// compiled sokol-VK object (58 symbols; vkGetInstanceProcAddr handled
// separately as the bootstrap). If sokol starts calling a new vk* the .so link
// (-Wl,--no-undefined) fails loudly at build time — add the symbol here.

#ifndef GE_VK_LOADER_H
#define GE_VK_LOADER_H

#define GE_VK_FUNCS(X) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkCmdBeginRendering) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdCopyBuffer) \
    X(vkCmdCopyBufferToImage2) \
    X(vkCmdDispatch) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdEndRendering) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdSetScissor) \
    X(vkCmdSetViewport) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateComputePipelines) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateFence) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImage) \
    X(vkCreateImageView) \
    X(vkCreatePipelineLayout) \
    X(vkCreateSampler) \
    X(vkCreateShaderModule) \
    X(vkDestroyBuffer) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyFence) \
    X(vkDestroyImage) \
    X(vkDestroyImageView) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroySampler) \
    X(vkDestroyShaderModule) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkFreeMemory) \
    X(vkGetBufferDeviceAddress) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceProcAddr) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkMapMemory) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkResetCommandBuffer) \
    X(vkResetFences) \
    X(vkUnmapMemory) \
    X(vkWaitForFences) \

// Global function-pointer storage lives in the .so's single TU (defined via
// GE_VK_FUNCS before sokol's IMPL include, so sokol's vk* calls bind to them).
// init resolves them all; call once after the VkInstance exists, before
// sg_setup. Idempotent.
#ifdef __cplusplus
extern "C"
#endif
void ge_sokol_vk_init_loader(VkInstance instance);

#endif // GE_VK_LOADER_H
