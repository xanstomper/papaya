#pragma once

#include "papaya/common/types.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

namespace papaya::memory {

inline void* allocate_aligned_pages(size_t size, size_t alignment = PAGE_SIZE_4K) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    std::memset(ptr, 0, size);
    return ptr;
}

inline void free_aligned_pages(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

inline void* map_anonymous_huge(size_t size) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (addr == MAP_FAILED) {
        return nullptr;
    }

#if defined(MADV_HUGEPAGE)
    // Advise kernel to back this mapping with Transparent Huge Pages (THP)
    madvise(addr, size, MADV_HUGEPAGE);
#endif

    return addr;
}

inline void unmap_anonymous(void* addr, size_t size) {
    if (addr && addr != MAP_FAILED) {
        munmap(addr, size);
    }
}

constexpr u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr u64 align_down(u64 value, u64 alignment) {
    return value & ~(alignment - 1);
}

} // namespace papaya::memory
