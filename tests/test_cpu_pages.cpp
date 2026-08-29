#include "papaya/common/logger.hpp"
#include "papaya/cpu/cpu_translator.hpp"
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
    using namespace papaya::cpu;

    log::info("TEST", "Running unit test: test_cpu_pages");

    // 1. Test 16KB Page Size Alignment Math
    PageSizeManager page_mgr_16k(PAGE_SIZE_16K);
    TEST_CHECK(page_mgr_16k.is_4k_emulation_required());
    TEST_CHECK(page_mgr_16k.get_host_page_size() == 16384);

    u64 unaligned_addr = 0x10005;
    u64 aligned_addr = page_mgr_16k.align_to_host_page(unaligned_addr);
    TEST_CHECK(aligned_addr == 0x10000);

    u64 unaligned_size = 4096;
    u64 aligned_size = page_mgr_16k.align_size_to_host_page(unaligned_size);
    TEST_CHECK(aligned_size == 16384);

    // 2. Test Page Allocation & Memory Protection
    auto alloc_res = page_mgr_16k.allocate_page_aligned(4096, PageProtection::ReadWrite);
    TEST_CHECK(alloc_res.has_value());
    TEST_CHECK(*alloc_res != nullptr);
    TEST_CHECK(page_mgr_16k.protect_page_range(*alloc_res, 4096, PageProtection::Read).has_value());
    TEST_CHECK(page_mgr_16k.free_page_aligned(*alloc_res, 4096).has_value());

    // 3. Test CPU Translator JIT Environment Variables
    CpuTranslator translator(CpuTranslationEngine::Box64Jit);
    TEST_CHECK(translator.initialize().has_value());
    auto envs = translator.get_environment_overrides();
    TEST_CHECK(!envs.empty());

    bool found_dynarec = false;
    for (const auto& [k, v] : envs) {
        if (k == "BOX64_DYNAREC" && v == "1") found_dynarec = true;
    }
    TEST_CHECK(found_dynarec);

    log::info("TEST", ">>> test_cpu_pages PASSED ALL CHECKS! <<<");
    return 0;
}
