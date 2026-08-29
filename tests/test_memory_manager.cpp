#include "papaya/common/logger.hpp"
#include "papaya/hle/memory_manager.hpp"
#include <cassert>
#include <vector>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::hle;

    log::info("TEST", "Running unit test: test_memory_manager");

    std::vector<u8> host_ram(128 * MiB, 0);

    // 1. Initialize Memory Manager with PS4 / PS5 unified layout
    MemoryManager mem_mgr(ConsoleTarget::PlayStation4);
    assert(mem_mgr.initialize(host_ram.data(), host_ram.size()).has_value());

    // 2. Allocate Direct Memory (Onion Coherent)
    auto gpa_res = mem_mgr.allocate_direct_memory(0, 0, 8 * MiB, PAGE_SIZE_2M, SceMemoryType::MainCoherent);
    assert(gpa_res.has_value());
    log::info("TEST", "Allocated Direct Memory: GPA=0x{:X}", *gpa_res);

    // 3. Map Direct Memory to Virtual Address Space
    auto gva_res = mem_mgr.map_direct_memory(0, 8 * MiB, SCE_PROT_READ | SCE_PROT_WRITE, 0, *gpa_res, PAGE_SIZE_2M);
    assert(gva_res.has_value());
    log::info("TEST", "Mapped Direct Memory: GVA=0x{:X}", *gva_res);

    // 4. Test FreeBSD sys_mmap
    auto mmap_res = mem_mgr.sys_mmap(0, 4 * MiB, SCE_PROT_ALL, SCE_MAP_ANONYMOUS | SCE_MAP_PRIVATE, -1, 0);
    assert(mmap_res.has_value());
    log::info("TEST", "sys_mmap allocated: GVA=0x{:X}", *mmap_res);

    // 5. Test sys_munmap and release
    assert(mem_mgr.sys_munmap(*mmap_res, 4 * MiB).has_value());
    assert(mem_mgr.release_direct_memory(*gpa_res, 8 * MiB).has_value());

    log::info("TEST", ">>> test_memory_manager PASSED ALL CHECKS! <<<");
    return 0;
}
