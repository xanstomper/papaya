#include "papaya/common/logger.hpp"
#include "papaya/profile/auto_configurator.hpp"
#include <iostream>
#include <cstdlib>

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAILED: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

int main() {
    using namespace papaya;
    using namespace papaya::profile;

    log::info("TEST", "Running unit test: test_auto_configurator");

    // 1. Test UltraLowEnd (Raspberry Pi 5) profile
    auto rp5_cfg = AutoConfigurator::create_profile_for_tier(DeviceTier::UltraLowEnd);
    TEST_CHECK(rp5_cfg.perf_mode == PerformanceMode::PotatoMode);
    TEST_CHECK(rp5_cfg.texture_config.mip_lod_bias >= 4.0f);
    TEST_CHECK(rp5_cfg.texture_config.enable_1x1_flat_replacement == true);
    TEST_CHECK(rp5_cfg.swapchain_config.internal_width == 960);
    TEST_CHECK(rp5_cfg.swapchain_config.internal_height == 540);
    TEST_CHECK(rp5_cfg.spoof_profile.vendor_id == 0x8086); // Intel HD 4000

    // 2. Test MobileHighTier (Snapdragon 8 Gen 2 / Gen 3 / Odin 2)
    auto odin_cfg = AutoConfigurator::create_profile_for_tier(DeviceTier::MobileHighTier);
    TEST_CHECK(odin_cfg.perf_mode == PerformanceMode::Performance);
    TEST_CHECK(odin_cfg.texture_config.mip_lod_bias == 2.0f);
    TEST_CHECK(odin_cfg.swapchain_config.internal_width == 1280);

    // 3. Test Host Auto-detection
    auto host_cfg = AutoConfigurator::detect_and_configure();
    TEST_CHECK(host_cfg.texture_config.max_texture_dimension > 0);

    log::info("TEST", ">>> test_auto_configurator PASSED ALL CHECKS! <<<");
    return 0;
}
