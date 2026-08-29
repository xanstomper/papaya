#include "papaya/profile/auto_configurator.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>
#include <unistd.h>

namespace papaya::profile {

DeviceAutoConfig AutoConfigurator::create_profile_for_tier(DeviceTier tier) {
    DeviceAutoConfig cfg{};
    cfg.tier = tier;

    switch (tier) {
        case DeviceTier::UltraLowEnd: { // Raspberry Pi 5 / BCM2712
            cfg.perf_mode = PerformanceMode::PotatoMode;
            cfg.spoof_profile = HardwareSpoofer::get_low_end_fallback_profile();

            cfg.texture_config = {
                .mode = PerformanceMode::PotatoMode,
                .mip_lod_bias = 4.0f,
                .max_texture_dimension = 128,
                .enable_1x1_flat_replacement = true,
                .flat_color_rgba = 0x808080FF,
                .anisotropic_clamp = 0
            };

            cfg.swapchain_config = {
                .enable_forced_resolution = true,
                .internal_width = 960,
                .internal_height = 540,
                .display_width = 1920,
                .display_height = 1080,
                .filter = gpu::UpscalingFilter::FastBilinear
            };
            break;
        }
        case DeviceTier::MobileMidTier:
        case DeviceTier::MobileHighTier: { // Snapdragon 8 Gen 2 / Gen 3 (AYN Odin 2)
            cfg.perf_mode = PerformanceMode::Performance;
            cfg.spoof_profile = HardwareSpoofer::get_adreno_turnip_profile();

            cfg.texture_config = {
                .mode = PerformanceMode::Performance,
                .mip_lod_bias = 2.0f,
                .max_texture_dimension = 512,
                .enable_1x1_flat_replacement = false,
                .flat_color_rgba = 0x808080FF,
                .anisotropic_clamp = 2
            };

            cfg.swapchain_config = {
                .enable_forced_resolution = true,
                .internal_width = 1280,
                .internal_height = 720,
                .display_width = 1920,
                .display_height = 1080,
                .filter = gpu::UpscalingFilter::FastBilinear
            };
            break;
        }
        case DeviceTier::HandheldPC: { // Steam Deck / ROG Ally
            cfg.perf_mode = PerformanceMode::Balanced;
            cfg.spoof_profile = HardwareSpoofer::get_steam_deck_profile();

            cfg.texture_config = {
                .mode = PerformanceMode::Balanced,
                .mip_lod_bias = 0.5f,
                .max_texture_dimension = 1024,
                .enable_1x1_flat_replacement = false,
                .flat_color_rgba = 0x808080FF,
                .anisotropic_clamp = 4
            };

            cfg.swapchain_config = {
                .enable_forced_resolution = false,
                .internal_width = 1280,
                .internal_height = 800,
                .display_width = 1280,
                .display_height = 800,
                .filter = gpu::UpscalingFilter::FastBilinear
            };
            break;
        }
        case DeviceTier::DesktopLinux:
        default: {
            cfg.perf_mode = PerformanceMode::Native;
            cfg.spoof_profile = HardwareSpoofer::get_geforce_fallback_profile();

            cfg.texture_config = {
                .mode = PerformanceMode::Native,
                .mip_lod_bias = 0.0f,
                .max_texture_dimension = 4096,
                .enable_1x1_flat_replacement = false,
                .flat_color_rgba = 0x808080FF,
                .anisotropic_clamp = 16
            };

            cfg.swapchain_config = {
                .enable_forced_resolution = false,
                .internal_width = 1920,
                .internal_height = 1080,
                .display_width = 1920,
                .display_height = 1080,
                .filter = gpu::UpscalingFilter::FastBilinear
            };
            break;
        }
    }

    return cfg;
}

DeviceAutoConfig AutoConfigurator::detect_and_configure() {
    DeviceTier tier = DeviceTier::DesktopLinux;

    // Check /proc/cpuinfo or device tree for Raspberry Pi / Snapdragon signatures
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string content((std::istreambuf_iterator<char>(cpuinfo)), std::istreambuf_iterator<char>());

    if (content.find("BCM2712") != std::string::npos || content.find("Raspberry Pi 5") != std::string::npos) {
        tier = DeviceTier::UltraLowEnd;
        log::info("AUTOCONFIG", "Detected Raspberry Pi 5 (BCM2712 SOC) -> Activating UltraLowEnd Potato Profile");
    } else if (content.find("Qualcomm") != std::string::npos || content.find("SM8550") != std::string::npos || content.find("SM8650") != std::string::npos) {
        tier = DeviceTier::MobileHighTier;
        log::info("AUTOCONFIG", "Detected Snapdragon 8 Gen 2/3 Mobile SOC -> Activating MobileHighTier Profile");
    } else if (content.find("VanGogh") != std::string::npos || content.find("AMD Custom APU 0405") != std::string::npos) {
        tier = DeviceTier::HandheldPC;
        log::info("AUTOCONFIG", "Detected Steam Deck Handheld -> Activating HandheldPC Profile");
    } else {
#if defined(__aarch64__)
        tier = DeviceTier::MobileMidTier;
        log::info("AUTOCONFIG", "Detected Generic ARM64 host -> Activating MobileMidTier Profile");
#else
        tier = DeviceTier::DesktopLinux;
        log::info("AUTOCONFIG", "Detected Desktop x86-64 host -> Activating DesktopLinux Native Profile");
#endif
    }

    return create_profile_for_tier(tier);
}

} // namespace papaya::profile
