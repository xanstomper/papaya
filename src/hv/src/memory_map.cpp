#include "papaya/hv/memory_map.hpp"
#include "papaya/common/logger.hpp"
#include <sys/mman.h>
#include <cstring>

namespace papaya::hv {

MemoryMap::~MemoryMap() {
    for (auto& region : regions_) {
        if (region.host_ptr && region.host_ptr != MAP_FAILED) {
            munmap(region.host_ptr, region.size);
        }
    }
    regions_.clear();
}

Result<> MemoryMap::map_region(std::string_view name, GuestPhysAddr guest_base, u64 size, bool is_readonly) {
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

    void* ptr = mmap(nullptr, size, prot, flags, -1, 0);
    if (ptr == MAP_FAILED) {
        log::error("MEM", "Failed to allocate {} MB for region '{}'", size / MiB, name);
        return ErrorCode::MemoryMappingFailed;
    }

    MemoryRegion region{
        .name = std::string(name),
        .guest_base = guest_base,
        .size = size,
        .host_ptr = ptr,
        .slot_id = next_slot_id_++,
        .is_readonly = is_readonly,
        .is_mmio = false
    };

    regions_.push_back(region);
    log::info("MEM", "Mapped region '{}' [GPA: 0x{:X}..0x{:X}, Size: {} MB, Host: {}]",
              name, guest_base, guest_base + size, size / MiB, ptr);

    return {};
}

Result<> MemoryMap::initialize_playstation_layout(ConsoleTarget target) {
    log::info("MEM", "Configuring PlayStation Unified Memory Map...");

    u64 ram_size = (target == ConsoleTarget::PlayStation5 || target == ConsoleTarget::PlayStation5Pro)
                    ? PS5_UNIFIED_RAM_SIZE
                    : PS4_UNIFIED_RAM_SIZE;

    // Direct Unified Memory (8 GB for PS4 / 16 GB for PS5)
    auto res = map_region("PlayStation Unified GDDR Memory", 0x00000000ULL, ram_size);
    if (!res) return res;

    ram_host_ptr_ = regions_.back().host_ptr;
    total_ram_size_ = ram_size;

    return {};
}

void* MemoryMap::get_host_pointer(GuestPhysAddr gpa) const {
    for (const auto& r : regions_) {
        if (gpa >= r.guest_base && gpa < r.guest_base + r.size) {
            u64 offset = gpa - r.guest_base;
            return static_cast<u8*>(r.host_ptr) + offset;
        }
    }
    return nullptr;
}

} // namespace papaya::hv
