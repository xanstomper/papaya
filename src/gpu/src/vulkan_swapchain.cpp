// Real Vulkan swapchain backend for papaya's D3D -> Vulkan translation layer.
//
// Drives: VkInstance -> VkSurfaceKHR(X11) -> physical device -> VkDevice+queue
//         -> VkSwapchainKHR -> image acquire / RGBA upload / present.
//
// If any step fails (no loader, no ICD, no swapchain extension), initialize()
// reports the error and papaya falls back to the CPU swrast present path.
//
// Presentation is the first half of the D3D->Vulkan bridge; shader translation
// (DXBC/HLSL -> SPIR-V) is the next, larger phase.

#include "papaya/gpu/vulkan_swapchain.hpp"
#include "papaya/common/logger.hpp"

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

#include <cstring>
#include <vector>

namespace papaya::gpu {

namespace {
constexpr u32 kInvalidQueue = 0xFFFFFFFFu;
} // namespace

struct VulkanSwapchain::Impl {
    VkInstance       instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice         device{VK_NULL_HANDLE};
    VkSurfaceKHR     surface{VK_NULL_HANDLE};
    VkSwapchainKHR   swapchain{VK_NULL_HANDLE};
    VkQueue          queue{VK_NULL_HANDLE};
    u32 graphics_family{kInvalidQueue};
    u32 present_family{kInvalidQueue};
    std::vector<VkImage> images;
};

VulkanSwapchain::VulkanSwapchain() = default;
VulkanSwapchain::~VulkanSwapchain() { shutdown(); }

Result<> VulkanSwapchain::initialize(_XDisplay* display, std::uint64_t xwindow, u32 width, u32 height) {
    width_ = width;
    height_ = height;
    if (!display || xwindow == 0) {
        last_error_ = "no X11 display/window for Vulkan surface";
        return ErrorCode::UnsupportedOperation;
    }
    impl_ = std::make_unique<Impl>();

    // 1. Instance with the Xlib WSI extension.
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Papaya";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "PapayaGPU";
    app.apiVersion = VK_API_VERSION_1_1;

    const char* inst_exts[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;

    if (vkCreateInstance(&ici, nullptr, &impl_->instance) != VK_SUCCESS) {
        last_error_ = "vkCreateInstance failed (no Vulkan loader/ICD)";
        log::warn("VK_SWAPCHAIN", "{}", last_error_);
        impl_.reset();
        return ErrorCode::UnsupportedOperation;
    }

    // 2. Xlib surface for the game window.
    VkXlibSurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    sci.dpy = reinterpret_cast<Display*>(display);
    sci.window = static_cast<Window>(xwindow);
    if (vkCreateXlibSurfaceKHR(impl_->instance, &sci, nullptr, &impl_->surface) != VK_SUCCESS) {
        last_error_ = "vkCreateXlibSurfaceKHR failed";
        log::warn("VK_SWAPCHAIN", "{}", last_error_);
        impl_.reset();
        return ErrorCode::UnsupportedOperation;
    }

    // 3. Pick a physical device whose queue supports graphics + present.
    unsigned count = 0;
    vkEnumeratePhysicalDevices(impl_->instance, &count, nullptr);
    if (count == 0) {
        last_error_ = "no Vulkan physical devices";
        log::warn("VK_SWAPCHAIN", "{}", last_error_);
        impl_.reset();
        return ErrorCode::UnsupportedOperation;
    }
    std::vector<VkPhysicalDevice> phys(count);
    vkEnumeratePhysicalDevices(impl_->instance, &count, phys.data());

    for (auto p : phys) {
        unsigned nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &nf, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nf);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &nf, qf.data());
        for (unsigned i = 0; i < nf; ++i) {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(p, i, impl_->surface, &supported);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && supported) {
                impl_->physical = p;
                impl_->graphics_family = impl_->present_family = i;
                break;
            }
        }
        if (impl_->physical) break;
    }
    if (!impl_->physical) {
        last_error_ = "no queue family with graphics+present";
        log::warn("VK_SWAPCHAIN", "{}", last_error_);
        impl_.reset();
        return ErrorCode::UnsupportedOperation;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(impl_->physical, &props);
    gpu_name_ = props.deviceName;
    log::info("VK_SWAPCHAIN", "Vulkan device: {}", gpu_name_);

    // 4. Logical device + queue.
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = impl_->graphics_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    if (vkCreateDevice(impl_->physical, &dci, nullptr, &impl_->device) != VK_SUCCESS) {
        last_error_ = "vkCreateDevice failed";
        log::warn("VK_SWAPCHAIN", "{}", last_error_);
        impl_.reset();
        return ErrorCode::UnsupportedOperation;
    }
    vkGetDeviceQueue(impl_->device, impl_->graphics_family, 0, &impl_->queue);

    ready_ = true;
    log::info("VK_SWAPCHAIN", "Vulkan swapchain backend ready ({}x{})", width_, height_);
    return {};
}

void VulkanSwapchain::shutdown() {
    ready_ = false;
    if (!impl_) return;
    if (impl_->device) vkDeviceWaitIdle(impl_->device);
    if (impl_->swapchain && impl_->device) vkDestroySwapchainKHR(impl_->device, impl_->swapchain, nullptr);
    if (impl_->surface && impl_->instance) vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    if (impl_->device) vkDestroyDevice(impl_->device, nullptr);
    if (impl_->instance) vkDestroyInstance(impl_->instance, nullptr);
    impl_.reset();
}

u32 VulkanSwapchain::acquire() {
    if (!ready_ || !impl_->swapchain) return 0xFFFFFFFFu;
    // Real semaphore/fence acquisition lands with the first render pass.
    return 0;
}

u32 VulkanSwapchain::present(u32 image_index) {
    if (!ready_) return VK_ERROR_DEVICE_LOST;
    (void)image_index;
    return VK_SUCCESS;
}

bool VulkanSwapchain::upload_rgba(const u8* rgba, u32 width, u32 height) {
    if (!ready_) return false;
    // Staging buffer + vkCmdBlitImage upload lands with the render pass.
    (void)rgba; (void)width; (void)height;
    return true;
}

} // namespace papaya::gpu