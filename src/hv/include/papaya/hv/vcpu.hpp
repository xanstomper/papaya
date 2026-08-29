#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <functional>

namespace papaya::hv {

enum class ExitReason {
    Unknown,
    IoIn,
    IoOut,
    MmioRead,
    MmioWrite,
    Halt,
    Shutdown,
    Hypercall,
    InternalError
};

struct VcpuExitInfo {
    ExitReason reason{ExitReason::Unknown};
    u64 address{0};
    u64 data{0};
    u32 size{0};
    bool is_write{false};
};

struct CpuRegisters {
    u64 rax{0}, rbx{0}, rcx{0}, rdx{0};
    u64 rsi{0}, rdi{0}, rsp{0}, rbp{0};
    u64 r8{0},  r9{0},  r10{0}, r11{0};
    u64 r12{0}, r13{0}, r14{0}, r15{0};
    u64 rip{0};
    u64 rflags{0x2}; // Bit 1 is always 1 in x86 EFLAGS
};

class IVcpu {
public:
    virtual ~IVcpu() = default;

    virtual u32 get_id() const = 0;
    virtual Result<> set_registers(const CpuRegisters& regs) = 0;
    virtual Result<CpuRegisters> get_registers() const = 0;
    virtual Result<> setup_initial_state(GuestPhysAddr entry_point, GuestPhysAddr stack_top) = 0;
    virtual Result<VcpuExitInfo> run_once() = 0;
    virtual void request_interrupt(u8 vector) = 0;
};

} // namespace papaya::hv
