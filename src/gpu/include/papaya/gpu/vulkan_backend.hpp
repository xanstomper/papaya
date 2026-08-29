#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/gpu/gcn_pm4.hpp"
#include <vector>

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace papaya::gpu {

struct SwapchainInfo {
    u32 width{1920};
    u32 height{1080};
    u32 image_count{2};
};

class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();

    Result<> initialize();
    void shutdown();

    Result<> create_device();
    Result<> execute_pm4_packets(const std::vector<DecodedPm4Packet>& packets);
    Result<> render_frame_present(u32 width, u32 height);

    bool is_initialized() const { return initialized_; }
    bool has_device() const { return has_device_; }

private:
    bool initialized_{false};
    bool has_device_{false};

#ifdef PAPAYA_HAS_VULKAN
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    u32 graphics_queue_family_{0};
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer_{VK_NULL_HANDLE};
#endif
};

} // namespace papaya::gpu
