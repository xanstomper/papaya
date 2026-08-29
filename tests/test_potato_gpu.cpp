#include "papaya/common/logger.hpp"
#include "papaya/gpu/potato_interceptor.hpp"
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

    log::info("TEST", "Running unit test: test_potato_gpu");

    TextureOverrideConfig cfg{
        .mode = PerformanceMode::PotatoMode,
        .mip_lod_bias = 3.5f,
        .max_texture_dimension = 256,
        .enable_1x1_flat_replacement = true,
        .flat_color_rgba = 0x808080FF,
        .anisotropic_clamp = 0
    };

    PotatoInterceptor potato(cfg);

    // 1. Test Sampler LOD Bias Interception
    u32 aniso = 16;
    f32 biased_lod = potato.intercept_sampler_lod_bias(0.0f, aniso);
    TEST_CHECK(biased_lod >= 3.5f);
    TEST_CHECK(aniso == 0); // Anisotropic filtering clamped to 0x

    // 2. Test 4K Heavy Texture Stripping & 1x1 Replacement
    u64 raw_4k_size = 4096 * 4096 * 4; // 64 MB
    bool strip_4k = potato.should_strip_texture(4096, 4096, 0, raw_4k_size);
    TEST_CHECK(strip_4k == true);

    // Small UI icon should NOT be stripped
    bool strip_ui = potato.should_strip_texture(64, 64, 0, 64 * 64 * 4);
    TEST_CHECK(strip_ui == false);

    // 3. Verify 1x1 Flat Pixel Buffer
    auto flat_buf = potato.get_1x1_flat_buffer();
    TEST_CHECK(flat_buf.size() == 4);

    // 4. Verify VRAM Savings Stats
    const auto& stats = potato.get_stats();
    TEST_CHECK(stats.total_textures_created == 2);
    TEST_CHECK(stats.textures_stripped == 1);
    TEST_CHECK(stats.saved_vram_bytes > 60 * MiB);

    log::info("TEST", "Potato Mode stripped {} textures and saved {} MB VRAM",
              stats.textures_stripped.load(), stats.saved_vram_bytes.load() / MiB);

    log::info("TEST", ">>> test_potato_gpu PASSED ALL CHECKS! <<<");
    return 0;
}
