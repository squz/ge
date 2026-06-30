// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T107: sokol_gfx backend glue for Android, with runtime Vulkan-or-GLES3
// selection.
//
// This TU is pure backend glue — window / surface / swapchain lifecycle. The
// sokol_gfx *implementation* lives in a per-backend .so (libgesokol-vk.so or
// libgesokol-gles.so); its sg_* calls resolve to libge's dispatch shim, which
// forwards to whichever backend was bound (ge_sokol_set_api). No SOKOL_IMPL
// here.
//
// Selection: at construction, ge_vk_probe() decides whether the device clears
// the sokol-Vulkan capability bar (instance ≥1.1, device ≥1.3,
// VK_EXT_descriptor_buffer + descriptorBuffer / bufferDeviceAddress /
// dynamicRendering / synchronization2 / a graphics queue). ACCEPT → try the
// Vulkan backend; any failure in Vulkan window/instance/device/swapchain bring-up
// falls back to GLES3. REJECT → GLES3 directly. GLES3 is the universal path that
// every Android GPU runs.
//
// Backend split: an abstract SokolContext::M with two concrete subclasses
// (GlesM, VkM). SokolContext's per-frame methods delegate; the single
// backend-choice point is the constructor. This keeps the two glue paths in
// self-contained units rather than #ifdef-scattered through one body.
//
// libge references Vulkan 1.0 core + KHR_surface/KHR_swapchain directly (the
// VkM glue): every such symbol is exported by Android's libvulkan.so loader,
// which is mandatory since API 24, so linking it costs no minSdk (we ship
// minSdk 26). The 1.1/1.2/1.3 core functions sokol's IMPL needs are NOT linked
// here — they are resolved inside the .so via ge_sokol_vk_init_loader.
//
// T39 carry-over: keep the swapchain pixel format RGBA8 on Android so the
// Apple-Silicon AVD's host GL/VK translator doesn't byte-swap the channels;
// RGBA8 is universally supported on real Android GPUs too.

#if defined(__ANDROID__)

#include <ge/SokolContext.h>
#include <ge/Signal.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_vulkan.h>
#include <spdlog/spdlog.h>

#include <GLES3/gl3.h>            // 🎯T92.6 glReadPixels for GLES screenshot readback
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <vector>

// SOKOL_GLES3 only satisfies sokol_gfx.h's "select a backend" #error; the public
// structs (sg_environment / sg_swapchain) carry every backend's substruct
// unconditionally, so this TU fills sc.gl.* OR sc.vulkan.* freely. No SOKOL_IMPL.
#define SOKOL_GLES3
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "ge_sokol_dispatch.h"   // ge_sg_api, ge_sokol_set_api (libge shim)

#include "ge_vkprobe.h"          // ge_vk_probe, ge_vk_result, GE_VK_ACCEPT

// Each backend .so returns its sg_* table; we install it via ge_sokol_set_api
// before sg_setup. ge_sokol_vk_init_loader resolves the .so's internal Vulkan
// function pointers (see ge_vk_loader.h) — call once after the VkInstance exists.
extern "C" const ge_sg_api* ge_sokol_bind_gles(void);
extern "C" const ge_sg_api* ge_sokol_bind_vk(void);
extern "C" void             ge_sokol_vk_init_loader(VkInstance instance);

namespace ge {

namespace {

// sokol-gfx callbacks bridged to spdlog (T66 — logcat via Android log sink).
void sokolLog(const char* tag, uint32_t log_level, uint32_t log_item,
              const char* message, uint32_t line_nr, const char* filename,
              void* /*user*/) {
    SPDLOG_LOGGER_CALL(spdlog::default_logger().get(),
        log_level <= 1 ? spdlog::level::err : spdlog::level::warn,
        "sokol[{}] {}({},{}): {}",
        tag, filename ? filename : "?", line_nr, log_item,
        message ? message : "");
}

bool vkOk(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    SPDLOG_ERROR("{} failed: VkResult {}", what, int(r));
    return false;
}

const void* vkHandle(VkImage h)     { return (const void*)(uintptr_t)h; }
const void* vkHandle(VkImageView h) { return (const void*)(uintptr_t)h; }
const void* vkHandle(VkSemaphore h) { return (const void*)(uintptr_t)h; }

sg_pixel_format sgFormat(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM: return SG_PIXELFORMAT_RGBA8;
        case VK_FORMAT_B8G8R8A8_UNORM: return SG_PIXELFORMAT_BGRA8;
        default:                       return SG_PIXELFORMAT_RGBA8;
    }
}

} // namespace

// ── abstract backend ────────────────────────────────────────────────
struct SokolContext::M {
    int         width  = 0;
    int         height = 0;
    SDL_Window* window = nullptr;
    bool        paused = false;
    SokolContext::FrameCaptureSink captureSink;  // 🎯T92.6 one-shot

    virtual ~M() = default;
    virtual void beginFrame(const float clearColor[4]) = 0;
    virtual void endFrame() = 0;
    virtual void onBackground() = 0;
    virtual void onForeground() = 0;
};

// ── GLES3 backend ───────────────────────────────────────────────────
namespace {

struct GlesM final : SokolContext::M {
    SDL_GLContext gl = nullptr;

    bool init(const SokolConfig& config) {
        width  = config.width;
        height = config.height;

        // EGL context attributes must be set before SDL_CreateWindow.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);  // 🎯T133: swapchain depth buffer
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

        const char* title = (config.title && *config.title) ? config.title : "ge";
        window = SDL_CreateWindow(title, config.width, config.height,
            SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
        if (!window) { SPDLOG_ERROR("SDL_CreateWindow(GLES) failed: {}", SDL_GetError()); return false; }

        gl = SDL_GL_CreateContext(window);
        if (!gl) { SPDLOG_ERROR("SDL_GL_CreateContext failed: {}", SDL_GetError()); return false; }
        if (!SDL_GL_MakeCurrent(window, gl)) {
            SPDLOG_ERROR("SDL_GL_MakeCurrent failed: {}", SDL_GetError()); return false;
        }
        SDL_GL_SetSwapInterval(1);  // vsync

        int pxW = 0, pxH = 0;
        if (SDL_GetWindowSizeInPixels(window, &pxW, &pxH) && pxW > 0 && pxH > 0) {
            width = pxW; height = pxH;
        }

        sg_desc desc{};
        desc.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;  // T39
        desc.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH;   // 🎯T133
        desc.environment.defaults.sample_count = 1;
        desc.logger.func = sokolLog;
        ge_sokol_set_api(ge_sokol_bind_gles());
        sg_setup(&desc);
        if (!sg_isvalid()) { SPDLOG_ERROR("sg_setup (GLES) failed"); return false; }

        SPDLOG_INFO("SokolContext: {}x{} GLES3 (Android)", width, height);
        SDL_Log("ge: SokolContext %dx%d GLES3 (Android)", width, height);
        return true;
    }

    ~GlesM() override {
        if (gl) {
            SDL_GL_MakeCurrent(window, gl);  // shut down sokol against the right context
            sg_shutdown();
            SDL_GL_DestroyContext(gl);
        } else {
            sg_shutdown();
        }
        if (window) SDL_DestroyWindow(window);
        SPDLOG_INFO("SokolContext destroyed (GLES)");
    }

    void beginFrame(const float clearColor[4]) override {
        if (paused || !sg_isvalid()) return;

        int pxW = 0, pxH = 0;
        if (SDL_GetWindowSizeInPixels(window, &pxW, &pxH)
            && pxW > 0 && pxH > 0 && (pxW != width || pxH != height)) {
            width = pxW; height = pxH;
        }

        sg_pass pass{};
        auto& act = pass.action;
        act.colors[0].load_action  = SG_LOADACTION_CLEAR;
        act.colors[0].store_action = SG_STOREACTION_STORE;
        act.colors[0].clear_value  = clearColor
            ? sg_color{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}
            : sg_color{0.f, 0.f, 0.f, 1.f};
        act.depth.load_action  = SG_LOADACTION_CLEAR;   // 🎯T133: clear depth to far
        act.depth.store_action = SG_STOREACTION_DONTCARE;
        act.depth.clear_value  = 1.0f;

        auto& sc = pass.swapchain;
        sc.width        = width;
        sc.height       = height;
        sc.sample_count = 1;
        sc.color_format = SG_PIXELFORMAT_RGBA8;
        sc.depth_format = SG_PIXELFORMAT_DEPTH;   // 🎯T133: matches EGL 24-bit depth
        sc.gl.framebuffer = 0;  // FBO 0 == default EGL window framebuffer
        sg_begin_pass(&pass);
    }

    void endFrame() override {
        if (paused || !sg_isvalid()) return;
        sg_end_pass();
        sg_commit();

        // 🎯T92.6 readback before SDL_GL_SwapWindow discards the back buffer.
        // glReadPixels is bottom-up; flip rows to top-down RGBA8.
        if (captureSink) {
            auto sink = std::move(captureSink);
            captureSink = nullptr;
            const int w = width, h = height;
            if (w > 0 && h > 0) {
                const std::size_t row = static_cast<std::size_t>(w) * 4;
                std::vector<std::uint8_t> raw(row * h), rgba(row * h);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
                for (int y = 0; y < h; ++y)
                    std::memcpy(&rgba[static_cast<std::size_t>(h - 1 - y) * row],
                                &raw[static_cast<std::size_t>(y) * row], row);
                sink(rgba.data(), w, h);
            }
        }
        SDL_GL_SwapWindow(window);
    }

    void onBackground() override {
        paused = true;
        SPDLOG_INFO("SokolContext: backgrounded (GLES, paused)");
    }

    void onForeground() override {
        if (window && gl) {
            if (!SDL_GL_MakeCurrent(window, gl))
                SPDLOG_WARN("SDL_GL_MakeCurrent on foreground failed: {}", SDL_GetError());
            int w = 0, h = 0;
            if (SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0) {
                width = w; height = h;
            }
            SPDLOG_INFO("SokolContext: foregrounded (GLES), surface {}x{}", width, height);
        }
        paused = false;
    }
};

// ── Vulkan backend ──────────────────────────────────────────────────
struct VkM final : SokolContext::M {
    VkInstance       instance       = VK_NULL_HANDLE;
    VkSurfaceKHR     surface        = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice         device         = VK_NULL_HANDLE;
    VkQueue          queue          = VK_NULL_HANDLE;
    uint32_t         queueFamily    = 0;

    VkSwapchainKHR             swapchain       = VK_NULL_HANDLE;
    VkFormat                   swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR            colorSpace      = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D                 extent{};
    std::vector<VkImage>       images;
    std::vector<VkImageView>   views;

    // 🎯T133: per-image depth attachment. sokol's swapchain pass needs a
    // depth-stencil image/view when depth_format != NONE; without it the
    // consumer's depth.compare/write are silently no-ops (sokol-metal is
    // lenient and tests anyway, but GLES/Vulkan honour NONE). VK_FORMAT_D32_SFLOAT
    // is what sokol maps SG_PIXELFORMAT_DEPTH to, and is a guaranteed-supported
    // depth attachment format. One per swapchain image so frames in flight don't
    // share a depth buffer.
    static constexpr VkFormat  kDepthFormat = VK_FORMAT_D32_SFLOAT;
    std::vector<VkImage>        depthImages;
    std::vector<VkDeviceMemory> depthMemories;
    std::vector<VkImageView>    depthViews;

    struct Sync {
        VkSemaphore presentComplete = VK_NULL_HANDLE;
        VkSemaphore renderFinished  = VK_NULL_HANDLE;
    };
    std::vector<Sync> sync;
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    bool     frameOpen  = false;

    bool init(const SokolConfig& config) {
        width  = config.width;
        height = config.height;

        const char* title = (config.title && *config.title) ? config.title : "ge";
        window = SDL_CreateWindow(title, config.width, config.height,
            SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN);
        if (!window) { SPDLOG_ERROR("SDL_CreateWindow(Vulkan) failed: {}", SDL_GetError()); return false; }

        if (!createInstance()) return false;
        // Resolve the backend .so's internal Vulkan function pointers now that an
        // instance exists (vkGetInstanceProcAddr resolves device-level commands too).
        ge_sokol_vk_init_loader(instance);

        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            SPDLOG_ERROR("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()); return false;
        }
        if (!chooseDevice())    return false;
        if (!createDevice())    return false;
        if (!createSwapchain()) return false;

        sg_desc desc{};
        desc.environment.defaults.color_format = sgFormat(swapchainFormat);
        desc.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH;   // 🎯T133
        desc.environment.defaults.sample_count = 1;
        desc.environment.vulkan.instance           = instance;
        desc.environment.vulkan.physical_device    = physicalDevice;
        desc.environment.vulkan.device             = device;
        desc.environment.vulkan.queue              = queue;
        desc.environment.vulkan.queue_family_index = queueFamily;
        desc.logger.func = sokolLog;
        ge_sokol_set_api(ge_sokol_bind_vk());
        sg_setup(&desc);
        if (!sg_isvalid()) { SPDLOG_ERROR("sg_setup (Vulkan) failed"); return false; }

        SPDLOG_INFO("SokolContext: {}x{} Vulkan (Android)", width, height);
        SDL_Log("ge: SokolContext %dx%d Vulkan (Android)", width, height);
        return true;
    }

    ~VkM() override {
        if (device) vkDeviceWaitIdle(device);
        if (sg_isvalid()) sg_shutdown();
        destroySwapchain();
        if (device)   vkDestroyDevice(device, nullptr);
        if (surface)  vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (window)   SDL_DestroyWindow(window);
        SPDLOG_INFO("SokolContext destroyed (Vulkan)");
    }

    bool createInstance() {
        uint32_t sdlExtCount = 0;
        char const* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
        if (!sdlExts || sdlExtCount == 0) {
            SPDLOG_ERROR("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError()); return false;
        }
        std::vector<const char*> exts(sdlExts, sdlExts + sdlExtCount);

        // 🎯T107: enable VK_EXT_debug_utils when present. sokol's debug build
        // labels every Vulkan object via vkSetDebugUtilsObjectNameEXT and asserts
        // the pointer is non-NULL. Strict loaders (Adreno) return NULL for that
        // function unless the extension is enabled here; lenient loaders (some
        // Samsung) happen to return it regardless — so enable it explicitly for
        // portability. (vkEnumerateInstanceExtensionProperties is 1.0 core, so
        // libge can link it directly.)
        uint32_t propCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &propCount, nullptr);
        std::vector<VkExtensionProperties> props(propCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &propCount, props.data());
        for (const auto& p : props) {
            if (std::strcmp(p.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                break;
            }
        }

        VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pApplicationName   = "ge";
        appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.pEngineName        = "ge";
        appInfo.engineVersion      = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.apiVersion         = VK_API_VERSION_1_3;

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo        = &appInfo;
        ci.enabledExtensionCount   = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        return vkOk(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
    }

    bool chooseDevice() {
        uint32_t devCount = 0;
        if (!vkOk(vkEnumeratePhysicalDevices(instance, &devCount, nullptr),
                  "vkEnumeratePhysicalDevices(count)") || devCount == 0) {
            SPDLOG_ERROR("No Vulkan physical devices"); return false;
        }
        std::vector<VkPhysicalDevice> devs(devCount);
        if (!vkOk(vkEnumeratePhysicalDevices(instance, &devCount, devs.data()),
                  "vkEnumeratePhysicalDevices")) return false;

        for (VkPhysicalDevice pd : devs) {
            uint32_t qCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, queues.data());
            for (uint32_t i = 0; i < qCount; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
                if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physicalDevice = pd; queueFamily = i; return true;
                }
            }
        }
        SPDLOG_ERROR("No Vulkan graphics+present queue family"); return false;
    }

    bool createDevice() {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &priority;

        const char* extensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        };

        VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        descriptorBuffer.descriptorBuffer = VK_TRUE;

        VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddress{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        bufferDeviceAddress.pNext               = &descriptorBuffer;
        bufferDeviceAddress.bufferDeviceAddress = VK_TRUE;

        VkPhysicalDeviceSynchronization2Features synchronization2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        synchronization2.pNext            = &bufferDeviceAddress;
        synchronization2.synchronization2 = VK_TRUE;

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        dynamicRendering.pNext           = &synchronization2;
        dynamicRendering.dynamicRendering = VK_TRUE;

        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.pNext                   = &dynamicRendering;
        ci.queueCreateInfoCount    = 1;
        ci.pQueueCreateInfos       = &qci;
        ci.enabledExtensionCount   = sizeof(extensions) / sizeof(extensions[0]);
        ci.ppEnabledExtensionNames = extensions;
        if (!vkOk(vkCreateDevice(physicalDevice, &ci, nullptr, &device), "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        return queue != VK_NULL_HANDLE;
    }

    void destroySwapchain() {
        for (auto& s : sync) {
            if (s.presentComplete) vkDestroySemaphore(device, s.presentComplete, nullptr);
            if (s.renderFinished)  vkDestroySemaphore(device, s.renderFinished, nullptr);
        }
        sync.clear();
        for (VkImageView view : views)
            if (view) vkDestroyImageView(device, view, nullptr);
        views.clear();
        images.clear();
        // 🎯T133: tear down the depth attachments alongside the color images.
        for (VkImageView v : depthViews)    if (v) vkDestroyImageView(device, v, nullptr);
        for (VkImage img : depthImages)     if (img) vkDestroyImage(device, img, nullptr);
        for (VkDeviceMemory m : depthMemories) if (m) vkFreeMemory(device, m, nullptr);
        depthViews.clear();
        depthImages.clear();
        depthMemories.clear();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    // 🎯T133: pick a DEVICE_LOCAL memory type for the depth image.
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    }

    // 🎯T133: one depth image + view per swapchain image, at the current extent.
    // Called from createSwapchain after the color views exist.
    bool createDepthResources(uint32_t count) {
        depthImages.resize(count, VK_NULL_HANDLE);
        depthMemories.resize(count, VK_NULL_HANDLE);
        depthViews.resize(count, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < count; ++i) {
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType     = VK_IMAGE_TYPE_2D;
            ici.format        = kDepthFormat;
            ici.extent        = {extent.width, extent.height, 1};
            ici.mipLevels     = 1;
            ici.arrayLayers   = 1;
            ici.samples       = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!vkOk(vkCreateImage(device, &ici, nullptr, &depthImages[i]), "vkCreateImage(depth)"))
                return false;

            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(device, depthImages[i], &mr);
            uint32_t mt = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mt == UINT32_MAX) { SPDLOG_ERROR("no DEVICE_LOCAL memory for depth image"); return false; }
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize  = mr.size;
            mai.memoryTypeIndex = mt;
            if (!vkOk(vkAllocateMemory(device, &mai, nullptr, &depthMemories[i]), "vkAllocateMemory(depth)"))
                return false;
            vkBindImageMemory(device, depthImages[i], depthMemories[i], 0);

            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image                       = depthImages[i];
            vi.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            vi.format                      = kDepthFormat;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (!vkOk(vkCreateImageView(device, &vi, nullptr, &depthViews[i]), "vkCreateImageView(depth)"))
                return false;
        }
        return true;
    }

    bool createSwapchain() {
        if (device) vkDeviceWaitIdle(device);
        destroySwapchain();

        int pxW = 0, pxH = 0;
        if (SDL_GetWindowSizeInPixels(window, &pxW, &pxH) && pxW > 0 && pxH > 0) {
            width = pxW; height = pxH;
        }

        VkSurfaceCapabilitiesKHR caps{};
        if (!vkOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps),
                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return false;

        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, formats.data());
        VkSurfaceFormatKHR chosen = formats.empty()
            ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
            : formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
        swapchainFormat = chosen.format;
        colorSpace      = chosen.colorSpace;

        if (caps.currentExtent.width != UINT32_MAX) {
            extent = caps.currentExtent;
        } else {
            extent.width  = std::clamp<uint32_t>(uint32_t(std::max(1, width)),
                                caps.minImageExtent.width,  caps.maxImageExtent.width);
            extent.height = std::clamp<uint32_t>(uint32_t(std::max(1, height)),
                                caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        width  = int(extent.width);
        height = int(extent.height);

        uint32_t imageCount = std::max(2u, caps.minImageCount + 1);
        if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface          = surface;
        ci.minImageCount    = imageCount;
        ci.imageFormat      = swapchainFormat;
        ci.imageColorSpace  = colorSpace;
        ci.imageExtent      = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = caps.currentTransform;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped          = VK_TRUE;
        if (!vkOk(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR"))
            return false;

        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr);
        images.resize(actualCount);
        vkGetSwapchainImagesKHR(device, swapchain, &actualCount, images.data());
        views.resize(actualCount);
        for (uint32_t i = 0; i < actualCount; ++i) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image                       = images[i];
            vi.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            vi.format                      = swapchainFormat;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (!vkOk(vkCreateImageView(device, &vi, nullptr, &views[i]), "vkCreateImageView"))
                return false;
        }

        if (!createDepthResources(actualCount)) return false;  // 🎯T133

        sync.resize(std::max<size_t>(2, actualCount));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : sync) {
            if (!vkOk(vkCreateSemaphore(device, &si, nullptr, &s.presentComplete),
                      "vkCreateSemaphore(presentComplete)") ||
                !vkOk(vkCreateSemaphore(device, &si, nullptr, &s.renderFinished),
                      "vkCreateSemaphore(renderFinished)")) return false;
        }
        frameIndex = 0;
        SPDLOG_INFO("SokolContext: {}x{} Vulkan, {} images, format {}",
                    width, height, actualCount, int(swapchainFormat));
        return true;
    }

    // 🎯T107 / T92.6: read back the just-rendered swapchain image into the sink
    // (RGBA8, top-down — matching the GLES path's contract). Dev-only one-shot,
    // so it creates+destroys its staging buffer / command pool / fence per call
    // rather than caching across resizes. Uses only Vulkan 1.0 + KHR_swapchain
    // entry points (vkCmdCopyImageToBuffer, vkCmdPipelineBarrier — not the 1.3
    // barrier2), so libge links them directly; the caller has already
    // vkQueueWaitIdle'd so the image is rendered and in PRESENT_SRC layout.
    void captureSwapchain(const SokolContext::FrameCaptureSink& sink) {
        if (width <= 0 || height <= 0 || imageIndex >= images.size()) return;
        const VkDeviceSize size = VkDeviceSize(width) * height * 4;

        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vkOk(vkCreateBuffer(device, &bci, nullptr, &buf), "vkCreateBuffer(capture)")) return;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, buf, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mp);
        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t mt = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & want) == want) { mt = i; break; }
        if (mt == UINT32_MAX) { vkDestroyBuffer(device, buf, nullptr); return; }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        if (!vkOk(vkAllocateMemory(device, &mai, nullptr, &mem), "vkAllocateMemory(capture)")) {
            vkDestroyBuffer(device, buf, nullptr); return;
        }
        vkBindBufferMemory(device, buf, mem, 0);

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = queueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(device, &pci, nullptr, &pool);
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cbai, &cb);

        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &cbbi);

        VkImage img = images[imageIndex];
        auto layoutBarrier = [&](VkImageLayout from, VkImageLayout to,
                                 VkAccessFlags srcA, VkAccessFlags dstA,
                                 VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = srcA; b.dstAccessMask = dstA;
            vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        layoutBarrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {uint32_t(width), uint32_t(height), 1};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        layoutBarrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_READ_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cb);

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(device, &fci, nullptr, &fence);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        void* mapped = nullptr;
        if (vkMapMemory(device, mem, 0, size, 0, &mapped) == VK_SUCCESS && mapped) {
            // Swapchain images are top-left origin (Vulkan), so no row flip. The
            // sink wants RGBA8; swizzle if the swapchain happens to be BGRA8.
            if (swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM) {
                const size_t bytes = static_cast<size_t>(size);
                std::vector<std::uint8_t> rgba(bytes);
                const std::uint8_t* src = static_cast<const std::uint8_t*>(mapped);
                for (size_t i = 0; i + 3 < bytes; i += 4) {
                    rgba[i + 0] = src[i + 2];
                    rgba[i + 1] = src[i + 1];
                    rgba[i + 2] = src[i + 0];
                    rgba[i + 3] = src[i + 3];
                }
                sink(rgba.data(), width, height);
            } else {
                sink(static_cast<const std::uint8_t*>(mapped), width, height);
            }
            vkUnmapMemory(device, mem);
        }

        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);  // also frees cb
        vkFreeMemory(device, mem, nullptr);
        vkDestroyBuffer(device, buf, nullptr);
    }

    void beginFrame(const float clearColor[4]) override {
        if (paused || !sg_isvalid() || !swapchain || sync.empty()) return;

        auto& s = sync[frameIndex % sync.size()];
        VkResult ar = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                             s.presentComplete, VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { createSwapchain(); return; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            SPDLOG_ERROR("vkAcquireNextImageKHR failed: VkResult {}", int(ar)); return;
        }

        sg_pass pass{};
        auto& act = pass.action;
        act.colors[0].load_action  = SG_LOADACTION_CLEAR;
        act.colors[0].store_action = SG_STOREACTION_STORE;
        act.colors[0].clear_value  = clearColor
            ? sg_color{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}
            : sg_color{0.f, 0.f, 0.f, 1.f};
        act.depth.load_action  = SG_LOADACTION_CLEAR;   // 🎯T133: clear depth to far
        act.depth.store_action = SG_STOREACTION_DONTCARE;
        act.depth.clear_value  = 1.0f;

        auto& sc = pass.swapchain;
        sc.width        = width;
        sc.height       = height;
        sc.sample_count = 1;
        sc.color_format = sgFormat(swapchainFormat);
        sc.depth_format = SG_PIXELFORMAT_DEPTH;   // 🎯T133
        sc.vulkan.render_image               = vkHandle(images[imageIndex]);
        sc.vulkan.render_view                = vkHandle(views[imageIndex]);
        sc.vulkan.depth_stencil_image        = vkHandle(depthImages[imageIndex]);  // 🎯T133
        sc.vulkan.depth_stencil_view         = vkHandle(depthViews[imageIndex]);   // 🎯T133
        sc.vulkan.render_finished_semaphore  = vkHandle(s.renderFinished);
        sc.vulkan.present_complete_semaphore = vkHandle(s.presentComplete);
        sg_begin_pass(&pass);
        frameOpen = true;
    }

    void endFrame() override {
        if (paused || !sg_isvalid() || !frameOpen) return;
        frameOpen = false;

        sg_end_pass();
        sg_commit();

        // 🎯T107 / T92.6 screenshot readback. sg_commit submitted the render with
        // the render_finished semaphore; wait the queue idle so the swapchain
        // image is fully rendered (and in PRESENT_SRC) before copying it out.
        // vkQueueWaitIdle doesn't reset render_finished, so the present below
        // still waits on it correctly.
        if (captureSink) {
            auto sink = std::move(captureSink);
            captureSink = nullptr;
            vkQueueWaitIdle(queue);
            captureSwapchain(sink);
        }

        auto& s = sync[frameIndex % sync.size()];
        VkSemaphore wait = s.renderFinished;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &wait;
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &swapchain;
        pi.pImageIndices      = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            createSwapchain();
        } else if (pr != VK_SUCCESS) {
            SPDLOG_ERROR("vkQueuePresentKHR failed: VkResult {}", int(pr));
        }
        frameIndex = (frameIndex + 1) % std::max<size_t>(1, sync.size());
    }

    void onBackground() override {
        paused = true;
        if (device) vkDeviceWaitIdle(device);
        SPDLOG_INFO("SokolContext: backgrounded (Vulkan, paused)");
    }

    void onForeground() override {
        int w = 0, h = 0;
        if (window && SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0) {
            width = w; height = h;
        }
        if (device && surface) createSwapchain();
        paused = false;
        SPDLOG_INFO("SokolContext: foregrounded (Vulkan), surface {}x{}", width, height);
    }
};

} // namespace

// ── SokolContext: probe → select → delegate ─────────────────────────
SokolContext::SokolContext(const SokolConfig& config) {
    ge::installSignalHandlers();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_SENSOR)) {
        SPDLOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return;
    }

    ge_vk_result probe;
    bool accept = (ge_vk_probe(&probe) == GE_VK_ACCEPT);
    if (accept) {
        SPDLOG_INFO("ge_vk_probe: ACCEPT_VULKAN (device \"{}\")", probe.device_name);
        auto vk = std::make_unique<VkM>();
        if (vk->init(config)) { m = std::move(vk); return; }
        SPDLOG_WARN("Vulkan backend bring-up failed — falling back to GLES3");
        // VkM dtor cleans up partial Vulkan state as it unwinds here.
    } else {
        SPDLOG_INFO("ge_vk_probe: FALLBACK_GLES (reasons: {})",
                    probe.reject_reasons[0] ? probe.reject_reasons : "n/a");
    }

    auto gl = std::make_unique<GlesM>();
    gl->init(config);
    m = std::move(gl);
}

SokolContext::~SokolContext() = default;

int         SokolContext::width()      const { return m->width;  }
int         SokolContext::height()     const { return m->height; }
bool        SokolContext::shouldQuit() const { return ge::shouldQuit(); }
SDL_Window* SokolContext::window()     const { return m->window; }

void SokolContext::beginFrame(const float clearColor[4]) { m->beginFrame(clearColor); }
void SokolContext::endFrame()                            { m->endFrame(); }
void SokolContext::captureNextFrame(FrameCaptureSink sink) { m->captureSink = std::move(sink); }
void SokolContext::onBackground()                        { m->onBackground(); }
void SokolContext::onForeground()                        { m->onForeground(); }

} // namespace ge

#endif // __ANDROID__
