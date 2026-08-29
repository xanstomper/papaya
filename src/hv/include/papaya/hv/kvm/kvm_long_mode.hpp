#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/kvm/kvm_vcpu.hpp"

namespace papaya::hv::kvm {

// Page table and GDT memory layout constants
constexpr GuestPhysAddr PML4_BASE_GPA  = 0x00010000ULL;
constexpr GuestPhysAddr PDPT_BASE_GPA  = 0x00011000ULL;
constexpr GuestPhysAddr PD_BASE_GPA    = 0x00012000ULL;
constexpr GuestPhysAddr GDT_BASE_GPA   = 0x00018000ULL;
constexpr GuestPhysAddr TEB_BASE_GPA   = 0x00020000ULL;
constexpr GuestPhysAddr PEB_BASE_GPA   = 0x00021000ULL;

class KvmLongMode {
public:
    static Result<> initialize_page_tables(void* host_ram_base, u64 ram_size);
    static Result<> initialize_teb_peb(void* host_ram_base, u64 ram_size, u32 pid = 0x1337, u32 tid = 0x1000);
    static Result<> switch_vcpu_to_long_mode(
        int vcpu_fd,
        GuestVirtAddr entry_rip,
        GuestVirtAddr stack_rsp
    );
};

} // namespace papaya::hv::kvm
