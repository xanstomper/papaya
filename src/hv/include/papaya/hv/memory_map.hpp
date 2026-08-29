#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <string>

namespace papaya::hv {

struct MemoryRegion {
    std::string name;
    GuestPhysAddr guest_base;
    u64 size;
    void* host_ptr;
    u32 slot_id;
    bool is_readonly{false};
    bool is_mmio{false};
};

class MemoryMap {
public:
    MemoryMap() = default;
    ~MemoryMap();

    Result<> initialize_xbox_one_layout();
    Result<> initialize_series_layout(bool is_series_x);

    Result<> map_region(std::string_view name, GuestPhysAddr guest_base, u64 size, bool is_readonly = false);
    
    void* get_host_pointer(GuestPhysAddr gpa) const;
    const std::vector<MemoryRegion>& get_regions() const { return regions_; }

    void* get_ram_base() const { return ram_host_ptr_; }
    void* get_esram_base() const { return esram_host_ptr_; }

    u64 get_total_ram_size() const { return total_ram_size_; }

private:
    std::vector<MemoryRegion> regions_;
    void* ram_host_ptr_{nullptr};
    void* esram_host_ptr_{nullptr};
    u64 total_ram_size_{0};
    u32 next_slot_id_{0};
};

} // namespace papaya::hv
