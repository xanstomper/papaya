#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <mutex>
#include <vector>
#include <map>

namespace papaya::hle {

// Protection flags (matching FreeBSD mmap/mprotect)
constexpr u32 SCE_PROT_NONE  = 0x00;
constexpr u32 SCE_PROT_READ  = 0x01;
constexpr u32 SCE_PROT_WRITE = 0x02;
constexpr u32 SCE_PROT_EXEC  = 0x04;
constexpr u32 SCE_PROT_ALL   = (SCE_PROT_READ | SCE_PROT_WRITE | SCE_PROT_EXEC);

// Map flags
constexpr u32 SCE_MAP_SHARED    = 0x0001;
constexpr u32 SCE_MAP_PRIVATE   = 0x0002;
constexpr u32 SCE_MAP_FIXED     = 0x0010;
constexpr u32 SCE_MAP_ANONYMOUS = 0x1000;

struct DirectMemoryBlock {
    GuestPhysAddr physical_address{0};
    u64 size{0};
    u64 alignment{PAGE_SIZE_2M};
    SceMemoryType type{SceMemoryType::MainCoherent};
    bool is_mapped{false};
    GuestVirtAddr mapped_virtual_address{0};
};

class MemoryManager {
public:
    explicit MemoryManager(ConsoleTarget target = ConsoleTarget::PlayStation4);
    ~MemoryManager();

    Result<> initialize(void* host_ram_base, u64 total_ram_size);

    // Direct Physical Memory Allocator
    Result<GuestPhysAddr> allocate_direct_memory(
        u64 search_start,
        u64 search_end,
        u64 length,
        u64 alignment,
        SceMemoryType mem_type
    );

    Result<> release_direct_memory(GuestPhysAddr phys_addr, u64 length);

    Result<GuestVirtAddr> map_direct_memory(
        GuestVirtAddr preferred_addr,
        u64 length,
        u32 protection,
        u32 flags,
        GuestPhysAddr phys_addr,
        u64 alignment
    );

    Result<> unmap_direct_memory(GuestVirtAddr virt_addr, u64 length);

    // FreeBSD Virtual Memory (sys_mmap, sys_munmap, sys_mprotect)
    Result<GuestVirtAddr> sys_mmap(
        GuestVirtAddr addr,
        u64 length,
        u32 prot,
        u32 flags,
        s32 fd,
        s64 offset
    );

    Result<> sys_munmap(GuestVirtAddr addr, u64 length);
    Result<> sys_mprotect(GuestVirtAddr addr, u64 length, u32 prot);

    void* get_host_pointer(GuestVirtAddr virt_addr) const;
    void* get_host_physical_pointer(GuestPhysAddr phys_addr) const;
    u64 get_total_ram_size() const { return total_ram_size_; }
    u64 get_available_direct_memory() const;

private:
    ConsoleTarget target_;
    void* host_ram_base_{nullptr};
    u64 total_ram_size_{0};

    GuestPhysAddr direct_memory_start_{0x80000000ULL}; // Physical Direct Memory starts at 2GB
    GuestPhysAddr direct_memory_cur_{0x80000000ULL};
    GuestVirtAddr virtual_alloc_cur_{0x100000000ULL};  // Virtual allocations start at 4GB

    mutable std::mutex mutex_;
    std::vector<DirectMemoryBlock> direct_blocks_;
    std::map<GuestVirtAddr, u64> virtual_allocations_;
};

} // namespace papaya::hle
