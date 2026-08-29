#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/profile/hardware_spoofer.hpp"
#include "papaya/gpu/potato_interceptor.hpp"
#include "papaya/gpu/swapchain_upscaler.hpp"

namespace papaya::profile {

struct DeviceAutoConfig {
    DeviceTier tier{DeviceTier::DesktopLinux};
    PerformanceMode perf_mode{PerformanceMode::Native};
    GpuSpoofProfile spoof_profile;
    gpu::TextureOverrideConfig texture_config;
    gpu::SwapchainScaleConfig swapchain_config;
    bool enable_ntsync{true};
    bool enable_io_uring{true};
};

class AutoConfigurator {
public:
    AutoConfigurator() = default;
    ~AutoConfigurator() = default;

    // Detects host specs and generates optimal configuration
    static DeviceAutoConfig detect_and_configure();

    // Generates profile for a specific device tier override
    static DeviceAutoConfig create_profile_for_tier(DeviceTier tier);
};

} // namespace papaya::profile
