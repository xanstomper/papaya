#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/vcpu.hpp"
#include <functional>
#include <unordered_map>

namespace papaya::hle {

class Kernel;

using SyscallHandler = std::function<u64(Kernel& kernel, hv::IVcpu& vcpu, const hv::CpuRegisters& regs)>;

class SyscallDispatcher {
public:
    SyscallDispatcher();

    void register_syscall(u32 syscall_nr, std::string_view name, SyscallHandler handler);
    Result<u64> dispatch(Kernel& kernel, hv::IVcpu& vcpu, u32 syscall_nr, const hv::CpuRegisters& regs);

private:
    struct SyscallEntry {
        std::string name;
        SyscallHandler handler;
    };
    std::unordered_map<u32, SyscallEntry> table_;
};

} // namespace papaya::hle
