#include "papaya/common/logger.hpp"
#include "papaya/profile/hardware_spoofer.hpp"
#include <cstring>
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

    log::info("TEST", "Running unit test: test_hardware_spoofer");

    auto low_end = HardwareSpoofer::get_low_end_fallback_profile();
    HardwareSpoofer spoofer(low_end);

    char desc[128] = {0};
    u32 vendor = 0, dev = 0;
    u64 vram = 0, shared = 0;

    spoofer.spoof_dxgi_adapter_desc(desc, sizeof(desc), vendor, dev, vram, shared);
    TEST_CHECK(std::strcmp(desc, "Intel(R) HD Graphics 4000") == 0);
    TEST_CHECK(vendor == 0x8086);
    TEST_CHECK(dev == 0x0166);
    TEST_CHECK(vram == 1024 * MiB);

    // Test Adreno Turnip profile
    auto adreno = HardwareSpoofer::get_adreno_turnip_profile();
    spoofer.set_profile(adreno);
    spoofer.spoof_dxgi_adapter_desc(desc, sizeof(desc), vendor, dev, vram, shared);
    TEST_CHECK(std::string_view(desc).find("Adreno") != std::string_view::npos);
    TEST_CHECK(vendor == 0x5143);

    log::info("TEST", ">>> test_hardware_spoofer PASSED ALL CHECKS! <<<");
    return 0;
}
