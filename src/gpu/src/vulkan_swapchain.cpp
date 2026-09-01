// Real Vulkan swapchain backend for papaya's D3D -> Vulkan translation layer.
//
// Drives the full present chain: VkInstance -> X11 VkSurfaceKHR -> physical
// device (graphics+present queue) -> VkDevice -> VkSwapchainKHR -> per-frame
// image acquire, staging-buffer RGBA upload, and vkQueuePresentKHR. Papaya's
// D3D11 layer maps IDXGISwapChain::Present onto upload_rgba()+present().
//
// Honest scope: presentation (what a frame renders into) is real. D3D *draw*
// (command-list -> Vulkan command buffer, DXBC/HLSL -> SPIR-V shader
// translation) is the next, much larger phase — this module presents uploaded
// CPU RGBA, which is correct for the current D3D11 swrast path.

#include "papaya/gpu/vulkan_swapchain.hpp"
#include "papaya/gpu/vulkan_pipeline.hpp"
#include "papaya/common/logger.hpp"

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

#include <cstring>
#include <algorithm>
#include <vector>

namespace papaya::gpu {

namespace {
constexpr u32 kInvalidQueue = 0xFFFFFFFFu;
constexpr int kSwapchainImageCount = 2;
} // namespace

struct VulkanSwapchain::Impl {
    VkInstance       instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice         device{VK_NULL_HANDLE};
    VkSurfaceKHR     surface{VK_NULL_HANDLE};
    VkSwapchainKHR   swapchain{VK_NULL_HANDLE};
    VkQueue          queue{VK_NULL_HANDLE};
    bool             fence_pending{false};   // a submit that signals the fence
    VkFormat         format{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D       extent{0, 0};
    VkCommandPool    cmd_pool{VK_NULL_HANDLE};
    VkCommandBuffer  cmd{VK_NULL_HANDLE};
    VkSemaphore      image_ready{VK_NULL_HANDLE};
    VkSemaphore      render_finished{VK_NULL_HANDLE};
    VkFence          fence{VK_NULL_HANDLE};
    // Staging buffer used to upload the guest's RGBA frame.
    VkBuffer         staging{VK_NULL_HANDLE};
    VkDeviceMemory   staging_mem{VK_NULL_HANDLE};
    void*            staging_map{nullptr};
    u32              staging_size{0};
    u32              graphics_family{kInvalidQueue};
    u32              present_family{kInvalidQueue};
    std::vector<VkImage>     images;
    // GPU pipeline path (render_and_present): cached per-spec objects + a
    // framebuffer and image view per swapchain image.
    VkPipeline       gpu_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout gpu_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout gpu_dsl{VK_NULL_HANDLE};
    VkRenderPass     gpu_rp{VK_NULL_HANDLE};
    const void*      gpu_spec_key{nullptr};
    std::vector<VkImageView>   image_views;
    std::vector<VkFramebuffer> framebuffers;
    VkCommandPool    gpu_pool{VK_NULL_HANDLE};
    VkCommandBuffer  gpu_cmd{VK_NULL_HANDLE};
    VkQueue          gpu_queue{VK_NULL_HANDLE};
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

    // Surface format + swapchain.
    VkSurfaceFormatKHR fmt{};
    {
        unsigned nfmt = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physical, impl_->surface, &nfmt, nullptr);
        if (nfmt > 0) {
            std::vector<VkSurfaceFormatKHR> fmts(nfmt);
            vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physical, impl_->surface, &nfmt, fmts.data());
            for (auto& f : fmts)
                if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = f; break; }
            if (!fmt.format && nfmt) fmt = fmts[0];
        }
        if (!fmt.format) { last_error_ = "no surface formats"; impl_.reset(); return ErrorCode::UnsupportedOperation; }
    }
    impl_->format = fmt.format;

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(impl_->physical, impl_->surface, &caps);
    impl_->extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    impl_->extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    width_ = impl_->extent.width; height_ = impl_->extent.height;

    VkSwapchainCreateInfoKHR swci{};
    swci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swci.surface = impl_->surface;
    swci.minImageCount = std::max(2u, caps.minImageCount);
    swci.imageFormat = impl_->format;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = impl_->extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    swci.oldSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(impl_->device, &swci, nullptr, &impl_->swapchain) != VK_SUCCESS) {
        last_error_ = "vkCreateSwapchainKHR failed";
        log::warn("VK_SWAPCHAIN", "{}", last_error_);
        impl_.reset();
        return ErrorCode::UnsupportedOperation;
    }
    unsigned nimg = 0;
    vkGetSwapchainImagesKHR(impl_->device, impl_->swapchain, &nimg, nullptr);
    impl_->images.resize(nimg);
    vkGetSwapchainImagesKHR(impl_->device, impl_->swapchain, &nimg, impl_->images.data());

    // Command pool + one command buffer per swapchain image for the blit.
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = impl_->graphics_family;
    if (vkCreateCommandPool(impl_->device, &cpci, nullptr, &impl_->cmd_pool) != VK_SUCCESS) {
        last_error_ = "vkCreateCommandPool failed";
        impl_.reset();
        return ErrorCode::UnsupportedOperation;
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = impl_->cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(impl_->device, &cbai, &impl_->cmd);

    // Sync primitives.
    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(impl_->device, &semi, nullptr, &impl_->image_ready);
    vkCreateSemaphore(impl_->device, &semi, nullptr, &impl_->render_finished);
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(impl_->device, &fci, nullptr, &impl_->fence);

    ready_ = true;
    log::info("VK_SWAPCHAIN", "Vulkan swapchain backend ready ({}x{}, {} images)",
              impl_->extent.width, impl_->extent.height, nimg);
    return {};
}

void VulkanSwapchain::destroy_gpu_path() {
    if (!impl_ || !impl_->device) return;
    auto& im = *impl_;
    for (auto fb : im.framebuffers) if (fb) vkDestroyFramebuffer(im.device, fb, nullptr);
    for (auto v : im.image_views) if (v) vkDestroyImageView(im.device, v, nullptr);
    im.framebuffers.clear();
    im.image_views.clear();
    if (im.gpu_pipeline) vkDestroyPipeline(im.device, im.gpu_pipeline, nullptr);
    if (im.gpu_layout) vkDestroyPipelineLayout(im.device, im.gpu_layout, nullptr);
    if (im.gpu_dsl) vkDestroyDescriptorSetLayout(im.device, im.gpu_dsl, nullptr);
    if (im.gpu_rp) vkDestroyRenderPass(im.device, im.gpu_rp, nullptr);
    im.gpu_pipeline = VK_NULL_HANDLE;
    im.gpu_layout = VK_NULL_HANDLE;
    im.gpu_dsl = VK_NULL_HANDLE;
    im.gpu_rp = VK_NULL_HANDLE;
    im.gpu_spec_key = nullptr;
}

void VulkanSwapchain::shutdown() {
    ready_ = false;
    if (!impl_) return;
    if (impl_->device) vkDeviceWaitIdle(impl_->device);
    if (impl_->cmd_pool) vkDestroyCommandPool(impl_->device, impl_->cmd_pool, nullptr);
    if (impl_->image_ready) vkDestroySemaphore(impl_->device, impl_->image_ready, nullptr);
    if (impl_->render_finished) vkDestroySemaphore(impl_->device, impl_->render_finished, nullptr);
    if (impl_->fence) vkDestroyFence(impl_->device, impl_->fence, nullptr);
    if (impl_->staging_mem) vkFreeMemory(impl_->device, impl_->staging_mem, nullptr);
    if (impl_->staging) vkDestroyBuffer(impl_->device, impl_->staging, nullptr);
    if (impl_->swapchain) vkDestroySwapchainKHR(impl_->device, impl_->swapchain, nullptr);
    if (impl_->surface) vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    if (impl_->device) vkDestroyDevice(impl_->device, nullptr);
    if (impl_->instance) vkDestroyInstance(impl_->instance, nullptr);
    impl_.reset();
}

u32 VulkanSwapchain::acquire() {
    if (!ready_) return 0xFFFFFFFFu;
    u32 index = 0xFFFFFFFFu;
    // Only wait when a prior frame's submit armed the fence (the first
    // acquire must never block on a never-signaled fence).
    if (impl_->fence_pending) {
        if (vkWaitForFences(impl_->device, 1, &impl_->fence, VK_TRUE, 5'000'000'000u) != VK_SUCCESS)
            return 0xFFFFFFFFu;
        vkResetFences(impl_->device, 1, &impl_->fence);
        impl_->fence_pending = false;
    }
    VkResult r = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, 2'000'000'000u,
                                        impl_->image_ready, impl_->fence, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 0xFFFFFFFFu;
    last_image_ = index;
    return index;
}

u32 VulkanSwapchain::present(u32 image_index, bool wait_on_render) {
    if (!ready_) return VK_ERROR_DEVICE_LOST;
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    if (wait_on_render && impl_->render_finished) {
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &impl_->render_finished;
    }
    pi.swapchainCount = 1;
    pi.pSwapchains = &impl_->swapchain;
    pi.pImageIndices = &image_index;
    VkResult r = vkQueuePresentKHR(impl_->queue, &pi);
    return static_cast<u32>(r);
}

bool VulkanSwapchain::upload_rgba(const u8* rgba, u32 width, u32 height) {
    if (!ready_ || !rgba || last_image_ >= impl_->images.size()) return false;
    // Ensure the staging buffer is large enough.
    u32 need = width * height * 4;
    if (impl_->staging_size < need) {
        if (impl_->staging_mem) { vkFreeMemory(impl_->device, impl_->staging_mem, nullptr); impl_->staging_mem = VK_NULL_HANDLE; }
        if (impl_->staging) { vkDestroyBuffer(impl_->device, impl_->staging, nullptr); impl_->staging = VK_NULL_HANDLE; }
        impl_->staging_size = need;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = need;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(impl_->device, &bci, nullptr, &impl_->staging) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(impl_->device, impl_->staging, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(impl_->physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((mr.memoryTypeBits >> i) & 1u && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mai.memoryTypeIndex = i; break; }
        if (vkAllocateMemory(impl_->device, &mai, nullptr, &impl_->staging_mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(impl_->device, impl_->staging, impl_->staging_mem, 0);
        vkMapMemory(impl_->device, impl_->staging_mem, 0, need, 0, &impl_->staging_map);
    }
    std::memcpy(impl_->staging_map, rgba, need);
    {
        VkMappedMemoryRange mmr{};
        mmr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mmr.memory = impl_->staging_mem;
        mmr.size = need;
        vkFlushMappedMemoryRanges(impl_->device, 1, &mmr);
    }

    // Record a command buffer: TRANSFER image LAYOUT, COPY staging->swapchain image,
    // then present (image ownership handled by the layout transition).
    VkCommandBuffer cb = impl_->cmd;
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cb, &cbbi) != VK_SUCCESS) return false;

    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = impl_->images[last_image_];
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.layerCount = 1;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cb, impl_->staging, impl_->images[last_image_],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = impl_->images[last_image_];
    to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_present.subresourceRange.layerCount = 1;
    to_present.subresourceRange.levelCount = 1;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_present);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return false;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    // Signal render_finished so present waits on the copy to complete.
    si.signalSemaphoreCount = impl_->render_finished ? 1 : 0;
    si.pSignalSemaphores = impl_->render_finished ? &impl_->render_finished : nullptr;
    if (vkQueueSubmit(impl_->queue, 1, &si, impl_->fence) != VK_SUCCESS) return false;
    impl_->fence_pending = true;
    return true;
}


u64 VulkanSwapchain::device() const { return impl_ && impl_->device ? reinterpret_cast<u64>(impl_->device) : 0; }
u64 VulkanSwapchain::physical_device() const {
    return impl_ && impl_->physical ? reinterpret_cast<u64>(impl_->physical) : 0;
}
u32 VulkanSwapchain::surface_format() const {
    return impl_ ? static_cast<u32>(impl_->format) : 0;
}

bool VulkanSwapchain::render_and_present(const PipelineSpec& spec,
                                         const u8* vertex_data, u32 vertex_stride,
                                         u32 vertex_count,
                                         const u8* cbuffer_data, size_t cbuffer_size) {
    if (!ready_ || !impl_ || !impl_->device || impl_->images.empty()) return false;
    if (!vertex_data || vertex_count == 0) return false;   // nothing to draw yet

    Impl& im = *impl_;
    // Cache the pipeline objects per spec (identity = the SPIR-V pointers,
    // which are stable because the D3D11 shader objects own the vectors).
    const void* key = spec.vs_spirv.data();
    if (im.gpu_spec_key != key) {
        destroy_gpu_path();
        std::string err;
        papaya::gpu::GraphicsPipeline gp;
        if (!papaya::gpu::create_graphics_pipeline(reinterpret_cast<u64>(im.device),
                                                   static_cast<u32>(im.format),
                                                   width_, height_, spec, gp, err))
            return false;
        im.gpu_pipeline = reinterpret_cast<VkPipeline>(gp.pipeline);
        im.gpu_layout = reinterpret_cast<VkPipelineLayout>(gp.layout);
        im.gpu_dsl = reinterpret_cast<VkDescriptorSetLayout>(gp.descriptor_layout);
        im.gpu_rp = reinterpret_cast<VkRenderPass>(gp.render_pass);
        im.gpu_spec_key = key;
        gp = {};   // ownership transferred
    }

    const u32 idx = acquire();
    if (idx == 0xFFFFFFFFu) return false;
    while (im.image_views.size() <= idx) {
        VkImageViewCreateInfo ivc{};
        ivc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivc.image = im.images[im.image_views.size()];
        ivc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivc.format = im.format;
        ivc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(im.device, &ivc, nullptr, &view);
        im.image_views.push_back(view);
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = im.gpu_rp;
        fci.attachmentCount = 1;
        fci.pAttachments = &view;
        fci.width = width_;
        fci.height = height_;
        fci.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(im.device, &fci, nullptr, &fb);
        im.framebuffers.push_back(fb);
    }

    std::string err;
    if (!papaya::gpu::render_pipeline(reinterpret_cast<u64>(im.device),
                                      reinterpret_cast<u64>(im.physical),
                                      width_, height_, static_cast<u32>(im.format),
                                      spec, reinterpret_cast<u64>(im.images[idx]),
                                      vertex_data, vertex_stride, vertex_count,
                                      cbuffer_data, cbuffer_size, nullptr, err)) {
        last_error_ = "GPU render: " + err;
        return false;
    }
    return present(idx, false) == 0;
}

} // namespace papaya::gpu