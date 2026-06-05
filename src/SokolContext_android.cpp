// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Experimental Android Vulkan backend for sokol_gfx via SDL3.

#if defined(__ANDROID__)

#include <ge/SokolContext.h>
#include <ge/Signal.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#define SOKOL_IMPL
#define SOKOL_VULKAN
#include "sokol_gfx.h"
#include "sokol_log.h"

namespace ge {

namespace {

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

const void* vkHandle(VkImage h) { return (const void*)(uintptr_t)h; }
const void* vkHandle(VkImageView h) { return (const void*)(uintptr_t)h; }
const void* vkHandle(VkSemaphore h) { return (const void*)(uintptr_t)h; }

sg_pixel_format sgFormat(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM: return SG_PIXELFORMAT_RGBA8;
        case VK_FORMAT_B8G8R8A8_UNORM: return SG_PIXELFORMAT_BGRA8;
        default: return SG_PIXELFORMAT_RGBA8;
    }
}

} // namespace

struct SokolContext::M {
    int width = 0;
    int height = 0;
    SDL_Window* window = nullptr;
    bool paused = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;

    struct Sync {
        VkSemaphore presentComplete = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE;
    };
    std::vector<Sync> sync;
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    bool frameOpen = false;

    SokolContext::FrameCaptureSink captureSink;

    ~M() {
        if (device) vkDeviceWaitIdle(device);
        if (sg_isvalid()) sg_shutdown();
        destroySwapchain();
        if (device) vkDestroyDevice(device, nullptr);
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (window) SDL_DestroyWindow(window);
        SPDLOG_INFO("SokolContext destroyed");
    }

    void destroySwapchain() {
        for (auto& s : sync) {
            if (s.presentComplete) vkDestroySemaphore(device, s.presentComplete, nullptr);
            if (s.renderFinished) vkDestroySemaphore(device, s.renderFinished, nullptr);
        }
        sync.clear();
        for (VkImageView view : views) {
            if (view) vkDestroyImageView(device, view, nullptr);
        }
        views.clear();
        images.clear();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    bool createInstance() {
        uint32_t sdlExtCount = 0;
        char const* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
        if (!sdlExts || sdlExtCount == 0) {
            SPDLOG_ERROR("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
            return false;
        }
        std::vector<const char*> exts(sdlExts, sdlExts + sdlExtCount);

        uint32_t propCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &propCount, nullptr);
        std::vector<VkExtensionProperties> props(propCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &propCount, props.data());
        bool hasDebugUtils = false;
        for (const auto& p : props) {
            if (std::strcmp(p.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                hasDebugUtils = true;
                break;
            }
        }
        if (hasDebugUtils) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            SPDLOG_WARN("{} unavailable; sokol debug object labels may fail",
                        VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pApplicationName = "ge";
        appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.pEngineName = "ge";
        appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &appInfo;
        ci.enabledExtensionCount = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        return vkOk(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
    }

    bool chooseDevice() {
        uint32_t devCount = 0;
        if (!vkOk(vkEnumeratePhysicalDevices(instance, &devCount, nullptr),
                  "vkEnumeratePhysicalDevices(count)") ||
            devCount == 0) {
            SPDLOG_ERROR("No Vulkan physical devices");
            return false;
        }
        std::vector<VkPhysicalDevice> devs(devCount);
        if (!vkOk(vkEnumeratePhysicalDevices(instance, &devCount, devs.data()),
                  "vkEnumeratePhysicalDevices")) {
            return false;
        }

        for (VkPhysicalDevice pd : devs) {
            uint32_t qCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, queues.data());
            for (uint32_t i = 0; i < qCount; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
                if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physicalDevice = pd;
                    queueFamily = i;
                    return true;
                }
            }
        }
        SPDLOG_ERROR("No Vulkan graphics+present queue family");
        return false;
    }

    bool createDevice() {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
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
        bufferDeviceAddress.pNext = &descriptorBuffer;
        bufferDeviceAddress.bufferDeviceAddress = VK_TRUE;

        VkPhysicalDeviceSynchronization2Features synchronization2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        synchronization2.pNext = &bufferDeviceAddress;
        synchronization2.synchronization2 = VK_TRUE;

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        dynamicRendering.pNext = &synchronization2;
        dynamicRendering.dynamicRendering = VK_TRUE;

        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.pNext = &dynamicRendering;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qci;
        ci.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
        ci.ppEnabledExtensionNames = extensions;
        if (!vkOk(vkCreateDevice(physicalDevice, &ci, nullptr, &device),
                  "vkCreateDevice")) {
            return false;
        }
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        return queue != VK_NULL_HANDLE;
    }

    bool createSwapchain() {
        if (device) vkDeviceWaitIdle(device);
        destroySwapchain();

        int pxW = 0, pxH = 0;
        if (SDL_GetWindowSizeInPixels(window, &pxW, &pxH) && pxW > 0 && pxH > 0) {
            width = pxW;
            height = pxH;
        }

        VkSurfaceCapabilitiesKHR caps{};
        if (!vkOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps),
                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
            return false;
        }

        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, formats.data());
        VkSurfaceFormatKHR chosen = formats.empty()
            ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
            : formats[0];
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosen = f;
                break;
            }
        }
        swapchainFormat = chosen.format;
        colorSpace = chosen.colorSpace;

        if (caps.currentExtent.width != UINT32_MAX) {
            extent = caps.currentExtent;
        } else {
            extent.width = std::clamp<uint32_t>(uint32_t(std::max(1, width)),
                                                caps.minImageExtent.width,
                                                caps.maxImageExtent.width);
            extent.height = std::clamp<uint32_t>(uint32_t(std::max(1, height)),
                                                 caps.minImageExtent.height,
                                                 caps.maxImageExtent.height);
        }
        width = int(extent.width);
        height = int(extent.height);

        uint32_t imageCount = std::max(2u, caps.minImageCount + 1);
        if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = imageCount;
        ci.imageFormat = swapchainFormat;
        ci.imageColorSpace = colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        if (!vkOk(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain),
                  "vkCreateSwapchainKHR")) {
            return false;
        }

        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr);
        images.resize(actualCount);
        vkGetSwapchainImagesKHR(device, swapchain, &actualCount, images.data());
        views.resize(actualCount);
        for (uint32_t i = 0; i < actualCount; ++i) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = images[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swapchainFormat;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (!vkOk(vkCreateImageView(device, &vi, nullptr, &views[i]),
                      "vkCreateImageView")) {
                return false;
            }
        }

        sync.resize(std::max<size_t>(2, actualCount));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : sync) {
            if (!vkOk(vkCreateSemaphore(device, &si, nullptr, &s.presentComplete),
                      "vkCreateSemaphore(presentComplete)") ||
                !vkOk(vkCreateSemaphore(device, &si, nullptr, &s.renderFinished),
                      "vkCreateSemaphore(renderFinished)")) {
                return false;
            }
        }
        frameIndex = 0;
        SPDLOG_INFO("SokolContext: {}x{} Vulkan (Android), {} images, format {}",
                    width, height, actualCount, int(swapchainFormat));
        SDL_Log("ge: SokolContext %dx%d Vulkan (Android)", width, height);
        return true;
    }
};

SokolContext::SokolContext(const SokolConfig& config)
    : m(std::make_unique<M>()) {
    m->width = config.width;
    m->height = config.height;

    ge::installSignalHandlers();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_SENSOR)) {
        SPDLOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return;
    }

    const char* title = (config.title && *config.title) ? config.title : "ge";
    m->window = SDL_CreateWindow(title,
        config.width, config.height,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN);
    if (!m->window) {
        SPDLOG_ERROR("SDL_CreateWindow(Vulkan) failed: {}", SDL_GetError());
        return;
    }
    if (!m->createInstance()) return;
    if (!SDL_Vulkan_CreateSurface(m->window, m->instance, nullptr, &m->surface)) {
        SPDLOG_ERROR("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return;
    }
    if (!m->chooseDevice()) return;
    if (!m->createDevice()) return;
    if (!m->createSwapchain()) return;

    sg_desc desc{};
    desc.environment.defaults.color_format = sgFormat(m->swapchainFormat);
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_NONE;
    desc.environment.defaults.sample_count = 1;
    desc.environment.vulkan.instance = m->instance;
    desc.environment.vulkan.physical_device = m->physicalDevice;
    desc.environment.vulkan.device = m->device;
    desc.environment.vulkan.queue = m->queue;
    desc.environment.vulkan.queue_family_index = m->queueFamily;
    desc.logger.func = sokolLog;
    sg_setup(&desc);
    if (!sg_isvalid()) {
        SPDLOG_ERROR("sg_setup failed");
        return;
    }
}

SokolContext::~SokolContext() = default;

int         SokolContext::width()      const { return m->width;  }
int         SokolContext::height()     const { return m->height; }
bool        SokolContext::shouldQuit() const { return ge::shouldQuit(); }
SDL_Window* SokolContext::window()     const { return m->window; }

void SokolContext::beginFrame(const float clearColor[4]) {
    if (m->paused || !sg_isvalid() || !m->swapchain || m->sync.empty()) return;

    auto& sync = m->sync[m->frameIndex % m->sync.size()];
    VkResult ar = vkAcquireNextImageKHR(m->device, m->swapchain, UINT64_MAX,
                                        sync.presentComplete, VK_NULL_HANDLE,
                                        &m->imageIndex);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        m->createSwapchain();
        return;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        SPDLOG_ERROR("vkAcquireNextImageKHR failed: VkResult {}", int(ar));
        return;
    }

    sg_pass pass{};
    auto& act = pass.action;
    act.colors[0].load_action = SG_LOADACTION_CLEAR;
    act.colors[0].store_action = SG_STOREACTION_STORE;
    act.colors[0].clear_value = clearColor
        ? sg_color{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}
        : sg_color{0.f, 0.f, 0.f, 1.f};

    auto& sc = pass.swapchain;
    sc.width = m->width;
    sc.height = m->height;
    sc.sample_count = 1;
    sc.color_format = sgFormat(m->swapchainFormat);
    sc.depth_format = SG_PIXELFORMAT_NONE;
    sc.vulkan.render_image = vkHandle(m->images[m->imageIndex]);
    sc.vulkan.render_view = vkHandle(m->views[m->imageIndex]);
    sc.vulkan.render_finished_semaphore = vkHandle(sync.renderFinished);
    sc.vulkan.present_complete_semaphore = vkHandle(sync.presentComplete);

    sg_begin_pass(&pass);
    m->frameOpen = true;
}

void SokolContext::captureNextFrame(FrameCaptureSink sink) {
    m->captureSink = std::move(sink);
}

void SokolContext::endFrame() {
    if (m->paused || !sg_isvalid() || !m->frameOpen) return;
    m->frameOpen = false;

    sg_end_pass();
    sg_commit();

    if (m->captureSink) {
        m->captureSink = nullptr;
        SPDLOG_WARN("SokolContext Vulkan screenshot capture is not implemented yet");
    }

    auto& sync = m->sync[m->frameIndex % m->sync.size()];
    VkSemaphore wait = sync.renderFinished;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &m->swapchain;
    pi.pImageIndices = &m->imageIndex;
    VkResult pr = vkQueuePresentKHR(m->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        m->createSwapchain();
    } else if (pr != VK_SUCCESS) {
        SPDLOG_ERROR("vkQueuePresentKHR failed: VkResult {}", int(pr));
    }
    m->frameIndex = (m->frameIndex + 1) % std::max<size_t>(1, m->sync.size());
}

void SokolContext::onBackground() {
    m->paused = true;
    if (m->device) vkDeviceWaitIdle(m->device);
    SPDLOG_INFO("SokolContext: backgrounded (paused)");
}

void SokolContext::onForeground() {
    int w = 0, h = 0;
    if (m->window && SDL_GetWindowSizeInPixels(m->window, &w, &h) && w > 0 && h > 0) {
        m->width = w;
        m->height = h;
    }
    if (m->device && m->surface) m->createSwapchain();
    m->paused = false;
    SPDLOG_INFO("SokolContext: foregrounded, surface {}x{}", m->width, m->height);
}

} // namespace ge

#endif // __ANDROID__
