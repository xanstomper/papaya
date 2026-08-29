#include "papaya/hv/memory_map.hpp"
#include "papaya/common/memory_utils.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::hv {

MemoryMap::~MemoryMap() {
    for (auto& region : regions_) {
        if (region.host_ptr) {
            memory::unmap_anonymous(region.host_ptr, region.size);
            region.host_ptr = nullptr;
        }
    }
}

Result<> MemoryMap::initialize_xbox_one_layout() {
    log::info("MEM", "Initializing Xbox One Physical Memory Layout (8GB DDR3 + 32MB ESRAM)");
    total_ram_size_ = XBOX_ONE_RAM_SIZE;

    // 1. Map Main System RAM (8GB): GPA [0x0000_0000 - 0x1_FFFF_FFFF]
    auto ram_res = map_region("System_RAM_8GB", 0x00000000ULL, XBOX_ONE_RAM_SIZE, false);
    if (!ram_res) {
        log::error("MEM", "Failed to allocate 8GB System RAM");
        return ram_res.error();
    }
    ram_host_ptr_ = regions_.back().host_ptr;

    // 2. Map Embedded SRAM (32MB ESRAM): GPA [0x2_0000_0000 - 0x2_01FF_FFFF] (Top of 8GB space)
    constexpr GuestPhysAddr ESRAM_BASE_GPA = 0x200000000ULL;
    auto esram_res = map_region("ESRAM_32MB", ESRAM_BASE_GPA, XBOX_ONE_ESRAM_SIZE, false);
    if (!esram_res) {
        log::error("MEM", "Failed to allocate 32MB ESRAM");
        return esram_res.error();
    }
    esram_host_ptr_ = regions_.back().host_ptr;

    log::info("MEM", "Physical layout configured successfully (2 regions mapped)");
    return {};
}

Result<> MemoryMap::initialize_series_layout(bool is_series_x) {
    u64 ram_size = is_series_x ? XBOX_SERIES_X_RAM : XBOX_SERIES_S_RAM;
    log::info("MEM", "Initializing Xbox Series {} Layout ({} GB GDDR6)",
              is_series_x ? "X" : "S", ram_size / GiB);
    total_ram_size_ = ram_size;

    auto ram_res = map_region("Unified_GDDR6_RAM", 0x00000000ULL, ram_size, false);
    if (!ram_res) {
        return ram_res.error();
    }
    ram_host_ptr_ = regions_.back().host_ptr;
    return {};
}

Result<> MemoryMap::map_region(std::string_view name, GuestPhysAddr guest_base, u64 size, bool is_readonly) {
    void* host_ptr = memory::map_anonymous_huge(size);
    if (!host_ptr) {
        return ErrorCode::OutOfMemory;
    }

    MemoryRegion region{
        .name = std::string(name),
        .guest_base = guest_base,
        .size = size,
        .host_ptr = host_ptr,
        .slot_id = next_slot_id_++,
        .is_readonly = is_readonly,
        .is_mmio = false
    };

    regions_.push_back(region);
    log::debug("MEM", "Mapped '{}': GPA [0x{:X} - 0x{:X}], Size: {} MB (Slot #{})",
               name, guest_base, guest_base + size, size / MiB, region.slot_id);
    return {};
}

void* MemoryMap::get_host_pointer(GuestPhysAddr gpa) const {
    for (const auto& region : regions_) {
        if (gpa >= region.guest_base && gpa < (region.guest_base + region.size)) {
            u64 offset = gpa - region.guest_base;
            return static_cast<u8*>(region.host_ptr) + offset;
        }
    }
    return nullptr;
}

} // namespace papaya::hv
