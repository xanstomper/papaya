#include "papaya/hle/syscalls.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::hle {

SyscallDispatcher::SyscallDispatcher() = default;

void SyscallDispatcher::register_syscall(u32 syscall_nr, std::string_view name, SyscallHandler handler) {
    table_[syscall_nr] = SyscallEntry{
        .name = std::string(name),
        .handler = std::move(handler)
    };
    log::debug("SYSCALL", "Registered syscall 0x{:04X} -> '{}'", syscall_nr, name);
}

Result<u64> SyscallDispatcher::dispatch(Kernel& kernel, hv::IVcpu& vcpu, u32 syscall_nr, const hv::CpuRegisters& regs) {
    auto it = table_.find(syscall_nr);
    if (it == table_.end()) {
        log::warn("SYSCALL", "Unknown/Unimplemented Syscall 0x{:04X} called from RIP 0x{:X}",
                  syscall_nr, regs.rip);
        return 0; // Return generic error/status code
    }

    log::debug("SYSCALL", "Invoking '{}' (0x{:04X}) from RIP 0x{:X}", it->second.name, syscall_nr, regs.rip);
    return it->second.handler(kernel, vcpu, regs);
}

} // namespace papaya::hle
