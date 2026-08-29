#include "papaya/common/logger.hpp"
#include "papaya/gpu/swapchain_upscaler.hpp"
#include <vector>
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
    using namespace papaya::gpu;

    log::info("TEST", "Running unit test: test_swapchain_upscaler");

    SwapchainScaleConfig cfg{
        .enable_forced_resolution = true,
        .internal_width = 960,
        .internal_height = 540,
        .display_width = 1920,
        .display_height = 1080,
        .filter = UpscalingFilter::FastBilinear
    };

    SwapchainUpscaler upscaler(cfg);

    u32 int_w = 0;
    u32 int_h = 0;
    upscaler.intercept_swapchain_dimensions(1920, 1080, int_w, int_h);
    TEST_CHECK(int_w == 960);
    TEST_CHECK(int_h == 540);

    TEST_CHECK(upscaler.get_scale_factor_x() == 2.0f);
    TEST_CHECK(upscaler.get_scale_factor_y() == 2.0f);

    // Test fast 540p -> 1080p spatial upscale
    std::vector<u8> src_frame(960 * 540 * 4, 0xAA);
    std::vector<u8> dst_frame(1920 * 1080 * 4, 0);

    upscaler.upscale_frame_rgba(src_frame.data(), dst_frame.data(), 960, 540, 1920, 1080);
    TEST_CHECK(dst_frame[0] == 0xAA);
    TEST_CHECK(dst_frame[dst_frame.size() - 1] == 0xAA);

    log::info("TEST", ">>> test_swapchain_upscaler PASSED ALL CHECKS! <<<");
    return 0;
}
