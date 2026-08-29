#include "papaya/common/logger.hpp"
#include "papaya/gpu/shader_stripper.hpp"
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

    log::info("TEST", "Running unit test: test_shader_stripping");

    ShaderStripperConfig cfg{
        .strip_compute_postprocess = true,
        .strip_motion_blur = true,
        .strip_volumetric_fog = true,
        .strip_ssao = true
    };

    ShaderStripper stripper(cfg);

    // Mock SPIR-V compute shader (ExecutionModel = 5)
    std::vector<u32> compute_spirv = {
        0x07230203, // SPIR-V Magic
        0x00010300, // Version 1.3
        0x00000000,
        0x00000010,
        0x00000000,
        // OpEntryPoint GLCompute (5)
        (3 << 16) | 15, 5, 1
    };

    ShaderType type = stripper.classify_spirv(compute_spirv);
    TEST_CHECK(type == ShaderType::Compute);

    // Verify should_strip triggers
    TEST_CHECK(stripper.should_strip(compute_spirv) == true);
    TEST_CHECK(stripper.get_stripped_shader_count() == 1);

    // Generate valid no-op SPIR-V module
    auto noop = stripper.generate_noop_spirv(type);
    TEST_CHECK(!noop.empty());
    TEST_CHECK(noop[0] == 0x07230203);

    log::info("TEST", ">>> test_shader_stripping PASSED ALL CHECKS! <<<");
    return 0;
}
