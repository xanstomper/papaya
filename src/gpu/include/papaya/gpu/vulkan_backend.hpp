#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/gpu/gcn_pm4.hpp"

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace papaya::gpu {

class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();

    Result<> initialize();
    void shutdown();

    Result<> execute_pm4_packets(const std::vector<DecodedPm4Packet>& packets);

    bool is_initialized() const { return initialized_; }

private:
    bool initialized_{false};
#ifdef PAPAYA_HAS_VULKAN
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
#endif
};

} // namespace papaya::gpu
