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
        log::warn("GPU", "vkCreateInstance returned {} (Running without native display / headless)", (int)res);
        initialized_ = false;
        return {};
    }

    log::info("GPU", "Vulkan 1.3 Instance initialized successfully");
    initialized_ = true;

    create_device();
    return {};
#else
    log::warn("GPU", "Compiled without Vulkan support");
    return {};
#endif
}

Result<> VulkanBackend::create_device() {
#ifdef PAPAYA_HAS_VULKAN
    if (!initialized_ || instance_ == VK_NULL_HANDLE) {
        return {};
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance_, &gpu_count, nullptr);
    if (gpu_count == 0) {
        log::warn("GPU", "No physical Vulkan GPUs detected");
        return {};
    }

    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance_, &gpu_count, gpus.data());
    physical_device_ = gpus[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    log::info("GPU", "Selected GPU: '{}' (Vulkan API {}.{}.{})",
              props.deviceName,
              VK_VERSION_MAJOR(props.apiVersion),
              VK_VERSION_MINOR(props.apiVersion),
              VK_VERSION_PATCH(props.apiVersion));

    float queue_prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &queue_prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) == VK_SUCCESS) {
        vkGetDeviceQueue(device_, 0, 0, &graphics_queue_);
        has_device_ = true;
        log::info("GPU", "Vulkan logical device and graphics queue created successfully");
    }
#endif
    return {};
}

void VulkanBackend::shutdown() {
#ifdef PAPAYA_HAS_VULKAN
    if (command_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
#endif
    has_device_ = false;
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

Result<> VulkanBackend::render_frame_present(u32 width, u32 height) {
    log::trace("GPU", "Present frame {}x{}", width, height);
    return {};
}

} // namespace papaya::gpu
