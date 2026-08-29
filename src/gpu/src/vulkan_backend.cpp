#include "papaya/gpu/vulkan_backend.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu {

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() {
    shutdown();
}

Result<> VulkanBackend::initialize() {
    log::info("GPU", "Initializing Vulkan 1.3 Graphics Backend");

#ifdef PAPAYA_HAS_VULKAN
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Project Papaya";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "PapayaGpu";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    VkResult res = vkCreateInstance(&create_info, nullptr, &instance_);
    if (res != VK_SUCCESS) {
        log::warn("GPU", "vkCreateInstance returned {} (Vulkan driver or display might not be active in headless mode)", (int)res);
        // Do not hard-fail if running headless / testing without display
        initialized_ = false;
        return {};
    }

    log::info("GPU", "Vulkan 1.3 Instance initialized successfully");
    initialized_ = true;
    return {};
#else
    log::warn("GPU", "Compiled without Vulkan support");
    return {};
#endif
}

void VulkanBackend::shutdown() {
#ifdef PAPAYA_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
#endif
    initialized_ = false;
}

Result<> VulkanBackend::execute_pm4_packets(const std::vector<DecodedPm4Packet>& packets) {
    for (const auto& pkt : packets) {
        switch (pkt.opcode) {
            case Pm4Type3OpCode::Nop:
                break;
            case Pm4Type3OpCode::SetContextReg:
                log::trace("GPU", "PM4 SetContextReg: count={}", pkt.payload.size());
                break;
            case Pm4Type3OpCode::DrawIndex2:
            case Pm4Type3OpCode::DrawIndexOffset2:
                log::debug("GPU", "PM4 Draw Command: opcode=0x{:02X}", static_cast<u8>(pkt.opcode));
                break;
            case Pm4Type3OpCode::DispatchDirect:
                log::debug("GPU", "PM4 Compute Dispatch: opcode=0x{:02X}", static_cast<u8>(pkt.opcode));
                break;
            default:
                log::trace("GPU", "PM4 Opcode 0x{:02X} unhandled", static_cast<u8>(pkt.opcode));
                break;
        }
    }
    return {};
}

} // namespace papaya::gpu
