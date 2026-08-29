#include "papaya/hle/kernel.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::hle {

Kernel::Kernel(std::shared_ptr<hv::IHypervisor> hypervisor, std::shared_ptr<storage::VirtualFileSystem> vfs)
    : hypervisor_(std::move(hypervisor)), vfs_(std::move(vfs)) {}

Kernel::~Kernel() = default;

Result<> Kernel::initialize() {
    log::info("KERNEL", "Initializing Xbox OS / Era Kernel Runtime");
    register_standard_syscalls();
    return {};
}

void Kernel::register_standard_syscalls() {
    // 0x0001: NtYieldExecution
    dispatcher_.register_syscall(0x0001, "NtYieldExecution", [](Kernel&, hv::IVcpu&, const hv::CpuRegisters&) -> u64 {
        return 0; // STATUS_SUCCESS
    });

    // 0x0002: NtTerminateProcess
    dispatcher_.register_syscall(0x0002, "NtTerminateProcess", [](Kernel&, hv::IVcpu&, const hv::CpuRegisters& regs) -> u64 {
        log::info("KERNEL", "NtTerminateProcess invoked with exit code: {}", regs.rcx);
        return 0;
    });

    // 0x0003: NtQuerySystemInformation
    dispatcher_.register_syscall(0x0003, "NtQuerySystemInformation", [](Kernel&, hv::IVcpu&, const hv::CpuRegisters&) -> u64 {
        return 0;
    });

    // 0x0010: XgSubmitCommandRing (Graphics Ring Buffer Submit)
    dispatcher_.register_syscall(0x0010, "XgSubmitCommandRing", [](Kernel&, hv::IVcpu&, const hv::CpuRegisters& regs) -> u64 {
        log::debug("KERNEL", "XgSubmitCommandRing: ring_buffer_gpa=0x{:X}, size={}", regs.rcx, regs.rdx);
        return 0;
    });
}

Result<> Kernel::load_title_executable(std::string_view exe_path) {
    log::info("KERNEL", "Loading title executable into VM: {}", exe_path);
    if (!vfs_ || !vfs_->exists(exe_path)) {
        log::error("KERNEL", "Executable not found in VFS: {}", exe_path);
        return ErrorCode::InvalidParameter;
    }

    auto node = vfs_->resolve(exe_path);
    auto data_res = node->read_all();
    if (!data_res) {
        return data_res.error();
    }

    log::info("KERNEL", "Loaded title payload ({} bytes) successfully", data_res->size());
    return {};
}

} // namespace papaya::hle
