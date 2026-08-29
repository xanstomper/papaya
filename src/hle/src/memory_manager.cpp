#include "papaya/hle/memory_manager.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <algorithm>

namespace papaya::hle {

MemoryManager::MemoryManager(ConsoleTarget target)
    : target_(target) {}

MemoryManager::~MemoryManager() = default;

Result<> MemoryManager::initialize(void* host_ram_base, u64 total_ram_size) {
    if (!host_ram_base || total_ram_size == 0) {
        return ErrorCode::InvalidParameter;
    }

    host_ram_base_ = host_ram_base;
    total_ram_size_ = total_ram_size;

    direct_memory_start_ = (total_ram_size >= 4 * GiB) ? 0x80000000ULL : (total_ram_size / 4);
    direct_memory_cur_   = direct_memory_start_;
    virtual_alloc_cur_   = (total_ram_size >= 4 * GiB) ? 0x100000000ULL : (total_ram_size / 2);

    log::info("MEM", "Initialized PlayStation Unified Memory Manager [RAM: {} MB, Mode: {}]",
              total_ram_size / MiB,
              target_ == ConsoleTarget::PlayStation5 ? "PS5 Prospero (16 GB GDDR6)" : "PS4 Orbis (8 GB GDDR5)");

    return {};
}

Result<GuestPhysAddr> MemoryManager::allocate_direct_memory(
    u64 search_start,
    u64 search_end,
    u64 length,
    u64 alignment,
    SceMemoryType mem_type
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (length == 0) return ErrorCode::InvalidParameter;
    if (alignment == 0) alignment = PAGE_SIZE_4K;

    // Align allocation
    u64 aligned_start = (direct_memory_cur_ + alignment - 1) & ~(alignment - 1);
    if (aligned_start + length > total_ram_size_) {
        log::error("MEM", "Direct memory exhausted! Requested {} MB, Available {} MB",
                   length / MiB, (total_ram_size_ > direct_memory_cur_ ? (total_ram_size_ - direct_memory_cur_) : 0) / MiB);
        return ErrorCode::OutOfMemory;
    }

    GuestPhysAddr phys_addr = aligned_start;
    direct_memory_cur_ = aligned_start + length;

    direct_blocks_.push_back({
        .physical_address = phys_addr,
        .size = length,
        .alignment = alignment,
        .type = mem_type,
        .is_mapped = false,
        .mapped_virtual_address = 0
    });

    log::debug("MEM", "sceKernelAllocateDirectMemory: GPA=0x{:X}, Size=0x{:X}, Type={}",
               phys_addr, length, mem_type == SceMemoryType::MainCoherent ? "WB_ONION" : "WC_GARLIC");

    return phys_addr;
}

Result<> MemoryManager::release_direct_memory(GuestPhysAddr phys_addr, u64 length) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = direct_blocks_.begin(); it != direct_blocks_.end(); ++it) {
        if (it->physical_address == phys_addr) {
            direct_blocks_.erase(it);
            log::debug("MEM", "sceKernelReleaseDirectMemory: GPA=0x{:X}", phys_addr);
            return {};
        }
    }
    return ErrorCode::NotFound;
}

Result<GuestVirtAddr> MemoryManager::map_direct_memory(
    GuestVirtAddr preferred_addr,
    u64 length,
    u32 protection,
    u32 flags,
    GuestPhysAddr phys_addr,
    u64 alignment
) {
    std::lock_guard<std::mutex> lock(mutex_);

    GuestVirtAddr vaddr = preferred_addr;
    if (vaddr == 0) {
        u64 align = (alignment > 0) ? alignment : PAGE_SIZE_2M;
        vaddr = (virtual_alloc_cur_ + align - 1) & ~(align - 1);
        virtual_alloc_cur_ = vaddr + length;
    }

    for (auto& block : direct_blocks_) {
        if (block.physical_address == phys_addr) {
            block.is_mapped = true;
            block.mapped_virtual_address = vaddr;
            break;
        }
    }

    log::debug("MEM", "sceKernelMapDirectMemory: GVA=0x{:X} -> GPA=0x{:X}, Size=0x{:X}, Prot=0x{:X}",
               vaddr, phys_addr, length, protection);

    return vaddr;
}

Result<> MemoryManager::unmap_direct_memory(GuestVirtAddr virt_addr, u64 length) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& block : direct_blocks_) {
        if (block.mapped_virtual_address == virt_addr) {
            block.is_mapped = false;
            block.mapped_virtual_address = 0;
            return {};
        }
    }
    return {};
}

Result<GuestVirtAddr> MemoryManager::sys_mmap(
    GuestVirtAddr addr,
    u64 length,
    u32 prot,
    u32 flags,
    s32 fd,
    s64 offset
) {
    std::lock_guard<std::mutex> lock(mutex_);

    GuestVirtAddr target = addr;
    if (target == 0) {
        target = (virtual_alloc_cur_ + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
        virtual_alloc_cur_ = target + length;
    }

    virtual_allocations_[target] = length;

    // Zero out memory
    if (host_ram_base_ && target + length <= total_ram_size_) {
        std::memset(static_cast<u8*>(host_ram_base_) + target, 0, length);
    }

    log::debug("MEM", "sys_mmap: GVA=0x{:X}, Size=0x{:X}, Prot=0x{:X}, Flags=0x{:X}", target, length, prot, flags);
    return target;
}

Result<> MemoryManager::sys_munmap(GuestVirtAddr addr, u64 length) {
    std::lock_guard<std::mutex> lock(mutex_);
    virtual_allocations_.erase(addr);
    return {};
}

Result<> MemoryManager::sys_mprotect(GuestVirtAddr addr, u64 length, u32 prot) {
    log::debug("MEM", "sys_mprotect: GVA=0x{:X}, Size=0x{:X}, Prot=0x{:X}", addr, length, prot);
    return {};
}

void* MemoryManager::get_host_pointer(GuestVirtAddr virt_addr) const {
    if (!host_ram_base_ || virt_addr >= total_ram_size_) return nullptr;
    return static_cast<u8*>(host_ram_base_) + virt_addr;
}

void* MemoryManager::get_host_physical_pointer(GuestPhysAddr phys_addr) const {
    if (!host_ram_base_ || phys_addr >= total_ram_size_) return nullptr;
    return static_cast<u8*>(host_ram_base_) + phys_addr;
}

u64 MemoryManager::get_available_direct_memory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (direct_memory_cur_ < total_ram_size_) ? (total_ram_size_ - direct_memory_cur_) : 0;
}

} // namespace papaya::hle
