#include "papaya/hv/kvm/kvm_long_mode.hpp"
#include "papaya/common/logger.hpp"
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <cstring>
#include <cstdlib>

namespace papaya::hv::kvm {

// Page table flags
constexpr u64 PAGE_PRESENT   = 1ULL << 0;
constexpr u64 PAGE_WRITABLE  = 1ULL << 1;
constexpr u64 PAGE_SIZE_2MB  = 1ULL << 7;

// EFER MSR bits
constexpr u64 EFER_SCE = 1ULL << 0;  // System Call Extensions
constexpr u64 EFER_LME = 1ULL << 8;  // Long Mode Enable
constexpr u64 EFER_LMA = 1ULL << 10; // Long Mode Active
constexpr u64 EFER_NXE = 1ULL << 11; // No-Execute Enable

// CR0 bits
constexpr u64 CR0_PE = 1ULL << 0;  // Protected Mode Enable
constexpr u64 CR0_MP = 1ULL << 1;  // Monitor Coprocessor
constexpr u64 CR0_ET = 1ULL << 4;  // Extension Type
constexpr u64 CR0_NE = 1ULL << 5;  // Numeric Error
constexpr u64 CR0_WP = 1ULL << 16; // Write Protect
constexpr u64 CR0_PG = 1ULL << 31; // Paging Enable

// CR4 bits
constexpr u64 CR4_PAE        = 1ULL << 5;  // Physical Address Extension
constexpr u64 CR4_PGE        = 1ULL << 7;  // Page Global Enable
constexpr u64 CR4_OSFXSR     = 1ULL << 9;  // OS support for FXSAVE and FXRSTOR
constexpr u64 CR4_OSXMMEXCPT = 1ULL << 10; // OS support for unmasked SIMD floating-point exceptions

// MSR registers
constexpr u32 MSR_EFER       = 0xC0000080;
constexpr u32 MSR_STAR       = 0xC0000081;
constexpr u32 MSR_LSTAR      = 0xC0000082;
constexpr u32 MSR_CSTAR      = 0xC0000083;
constexpr u32 MSR_FMASK      = 0xC0000084;
constexpr u32 MSR_FS_BASE    = 0xC0000100;
constexpr u32 MSR_GS_BASE    = 0xC0000101;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;

Result<> KvmLongMode::initialize_page_tables(void* host_ram_base, u64 ram_size) {
    if (!host_ram_base || ram_size < 0x20000000) { // Require at least 512MB
        return ErrorCode::InvalidParameter;
    }

    auto* ram = static_cast<u8*>(host_ram_base);

    // Clear page table memory space (64KB)
    std::memset(ram + PML4_BASE_GPA, 0, 0x10000);

    auto* pml4 = reinterpret_cast<u64*>(ram + PML4_BASE_GPA);
    auto* pdpt = reinterpret_cast<u64*>(ram + PDPT_BASE_GPA);

    // PML4 entry 0 -> PDPT
    pml4[0] = PDPT_BASE_GPA | PAGE_PRESENT | PAGE_WRITABLE;

    // We map up to 16 GB using 2MB large pages (16 PDPT entries, each pointing to a 512-entry PD)
    u64 num_gb = std::min<u64>(ram_size / GiB, 16);
    if (num_gb == 0) num_gb = 1;

    for (u64 gb = 0; gb < num_gb; ++gb) {
        GuestPhysAddr pd_gpa = PD_BASE_GPA + (gb * 0x1000);
        pdpt[gb] = pd_gpa | PAGE_PRESENT | PAGE_WRITABLE;

        auto* pd = reinterpret_cast<u64*>(ram + pd_gpa);
        for (u64 p = 0; p < 512; ++p) {
            u64 gpa = (gb * GiB) + (p * (2 * MiB));
            pd[p] = gpa | PAGE_PRESENT | PAGE_WRITABLE | PAGE_SIZE_2MB;
        }
    }

    log::info("LONG_MODE", "Identity mapped {} GB of guest physical memory (2MB huge pages)", num_gb);
    return {};
}

Result<> KvmLongMode::initialize_teb_peb(void* host_ram_base, u64 ram_size, u32 pid, u32 tid) {
    if (!host_ram_base || PEB_BASE_GPA + 0x1000 > ram_size) {
        return ErrorCode::InvalidParameter;
    }

    auto* ram = static_cast<u8*>(host_ram_base);

    // Zero TEB (4KB) and PEB (4KB)
    std::memset(ram + TEB_BASE_GPA, 0, 0x1000);
    std::memset(ram + PEB_BASE_GPA, 0, 0x1000);

    // Windows 64-bit TEB (Thread Environment Block):
    // Offset 0x30: LinearAddress (Self pointer)
    *reinterpret_cast<u64*>(ram + TEB_BASE_GPA + 0x30) = TEB_BASE_GPA;
    // Offset 0x60: ProcessEnvironmentBlock pointer
    *reinterpret_cast<u64*>(ram + TEB_BASE_GPA + 0x60) = PEB_BASE_GPA;
    // Offset 0x40: ClientId.UniqueProcess
    *reinterpret_cast<u64*>(ram + TEB_BASE_GPA + 0x40) = pid;
    // Offset 0x48: ClientId.UniqueThread
    *reinterpret_cast<u64*>(ram + TEB_BASE_GPA + 0x48) = tid;

    // Windows 64-bit PEB (Process Environment Block):
    // Offset 0x10: ImageBaseAddress
    *reinterpret_cast<u64*>(ram + PEB_BASE_GPA + 0x10) = 0x00400000ULL;
    // Offset 0x18: ProcessHeap
    *reinterpret_cast<u64*>(ram + PEB_BASE_GPA + 0x18) = 0x10000000ULL;

    log::info("LONG_MODE", "Initialized Windows x86-64 TEB (GPA 0x{:X}) and PEB (GPA 0x{:X}) [PID: 0x{:X}, TID: 0x{:X}]",
              TEB_BASE_GPA, PEB_BASE_GPA, pid, tid);
    return {};
}

Result<> KvmLongMode::switch_vcpu_to_long_mode(
    int vcpu_fd,
    GuestVirtAddr entry_rip,
    GuestVirtAddr stack_rsp
) {
    // 1. Configure Special Registers (SREGS)
    struct kvm_sregs sregs{};
    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
        log::error("LONG_MODE", "KVM_GET_SREGS failed");
        return ErrorCode::VcpuRunFailed;
    }

    // Set 64-bit Code Segment (CS)
    sregs.cs.base = 0;
    sregs.cs.limit = 0xFFFFFFFF;
    sregs.cs.selector = 0x08; // Selector 1 (GDT index 1)
    sregs.cs.type = 11;       // Code Execute/Read, accessed
    sregs.cs.present = 1;
    sregs.cs.dpl = 0;
    sregs.cs.db = 0;          // Must be 0 for 64-bit code segment
    sregs.cs.s = 1;
    sregs.cs.l = 1;          // 64-bit Long mode flag
    sregs.cs.g = 1;
    sregs.cs.unusable = 0;

    // Set Data Segments (DS, ES, SS, FS, GS)
    auto setup_data_seg = [](struct kvm_segment& seg, u16 selector) {
        seg.base = 0;
        seg.limit = 0xFFFFFFFF;
        seg.selector = selector;
        seg.type = 3;         // Data Read/Write, accessed
        seg.present = 1;
        seg.dpl = 0;
        seg.db = 1;
        seg.s = 1;
        seg.l = 0;
        seg.g = 1;
        seg.unusable = 0;
    };

    setup_data_seg(sregs.ds, 0x10);
    setup_data_seg(sregs.es, 0x10);
    setup_data_seg(sregs.ss, 0x10);
    setup_data_seg(sregs.fs, 0x10);
    setup_data_seg(sregs.gs, 0x10);

    // Setup TR (Task Register)
    sregs.tr.base = 0;
    sregs.tr.limit = 0xFFFFFFFF;
    sregs.tr.selector = 0x18;
    sregs.tr.type = 11;       // 64-bit TSS (busy)
    sregs.tr.present = 1;
    sregs.tr.dpl = 0;
    sregs.tr.db = 0;
    sregs.tr.s = 0;          // System segment
    sregs.tr.l = 0;
    sregs.tr.g = 1;
    sregs.tr.unusable = 0;

    // Setup CR3 (PML4 base)
    sregs.cr3 = PML4_BASE_GPA;

    // Setup CR4 (PAE, OSFXSR, OSXMMEXCPT)
    sregs.cr4 = CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT;

    // Setup CR0 (Paging + Protected Mode)
    sregs.cr0 = CR0_PG | CR0_PE;

    // Setup EFER (Long Mode Active + Long Mode Enable)
    sregs.efer = EFER_LME | EFER_LMA;

    if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        log::error("LONG_MODE", "KVM_SET_SREGS failed to switch to long mode: {}", strerror(errno));
        return ErrorCode::VcpuRunFailed;
    }

    // 2. Set GS_BASE and FS_BASE MSRs for Windows TEB
    size_t sz = sizeof(struct kvm_msrs) + (sizeof(struct kvm_msr_entry) * 3);
    auto* msrs = static_cast<struct kvm_msrs*>(std::malloc(sz));
    if (msrs) {
        std::memset(msrs, 0, sz);
        msrs->nmsrs = 3;
        msrs->entries[0].index = MSR_GS_BASE;
        msrs->entries[0].data  = TEB_BASE_GPA; // GS points to TEB in Win64
        msrs->entries[1].index = MSR_FS_BASE;
        msrs->entries[1].data  = 0;
        msrs->entries[2].index = MSR_KERNEL_GS_BASE;
        msrs->entries[2].data  = TEB_BASE_GPA;

        if (ioctl(vcpu_fd, KVM_SET_MSRS, msrs) < 0) {
            log::warn("LONG_MODE", "KVM_SET_MSRS failed: {}", strerror(errno));
        }
        std::free(msrs);
    }

    // 3. Configure General Purpose Registers (RIP, RSP, RFLAGS)
    struct kvm_regs regs{};
    regs.rip = entry_rip;
    regs.rsp = stack_rsp;
    regs.rflags = 0x2; // Reserved bit 1

    if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        log::error("LONG_MODE", "KVM_SET_REGS failed: {}", strerror(errno));
        return ErrorCode::VcpuRunFailed;
    }

    log::info("LONG_MODE", "Switched vCPU to 64-bit Long Mode: RIP=0x{:X}, RSP=0x{:X}, GS_BASE=0x{:X}",
              entry_rip, stack_rsp, TEB_BASE_GPA);
    return {};
}

} // namespace papaya::hv::kvm
