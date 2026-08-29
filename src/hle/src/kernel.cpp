#include "papaya/hle/kernel.hpp"
#include "papaya/common/logger.hpp"
#include <chrono>

namespace papaya::hle {

Kernel::Kernel(std::shared_ptr<hv::IHypervisor> hypervisor, std::shared_ptr<storage::VirtualFileSystem> vfs)
    : hypervisor_(std::move(hypervisor)), vfs_(std::move(vfs)) {}

Kernel::~Kernel() = default;

Result<> Kernel::initialize() {
    log::info("KERNEL", "Initializing Xbox OS / Era Kernel Runtime & HLE Library Dispatch");
    register_standard_syscalls();
    register_standard_hle_apis();
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

void Kernel::register_standard_hle_apis() {
    // === kernel32.dll ===
    thunk_mgr_.register_function("kernel32.dll", "GetModuleHandleA", [](HleCallContext& ctx) -> u64 {
        log::debug("HLE", "GetModuleHandleA(0x{:X})", ctx.rcx);
        return 0x00400000; // Base address of main executable
    });

    thunk_mgr_.register_function("kernel32.dll", "GetModuleHandleW", [](HleCallContext& ctx) -> u64 {
        log::debug("HLE", "GetModuleHandleW(0x{:X})", ctx.rcx);
        return 0x00400000;
    });

    thunk_mgr_.register_function("kernel32.dll", "GetProcAddress", [](HleCallContext& ctx) -> u64 {
        log::debug("HLE", "GetProcAddress(hModule=0x{:X}, lpProcName=0x{:X})", ctx.rcx, ctx.rdx);
        return 0;
    });

    thunk_mgr_.register_function("kernel32.dll", "GetCurrentProcessId", [](HleCallContext&) -> u64 {
        return 0x1337;
    });

    thunk_mgr_.register_function("kernel32.dll", "GetCurrentThreadId", [](HleCallContext&) -> u64 {
        return 0x1000;
    });

    thunk_mgr_.register_function("kernel32.dll", "QueryPerformanceFrequency", [](HleCallContext& ctx) -> u64 {
        if (ctx.host_ram_base && ctx.rcx + sizeof(u64) <= ctx.ram_size) {
            auto* freq_ptr = reinterpret_cast<u64*>(static_cast<u8*>(ctx.host_ram_base) + ctx.rcx);
            *freq_ptr = 10000000ULL; // 10 MHz timer
            return 1; // TRUE
        }
        return 0;
    });

    thunk_mgr_.register_function("kernel32.dll", "QueryPerformanceCounter", [](HleCallContext& ctx) -> u64 {
        if (ctx.host_ram_base && ctx.rcx + sizeof(u64) <= ctx.ram_size) {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            u64 counts = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100;
            auto* counter_ptr = reinterpret_cast<u64*>(static_cast<u8*>(ctx.host_ram_base) + ctx.rcx);
            *counter_ptr = counts;
            return 1; // TRUE
        }
        return 0;
    });

    thunk_mgr_.register_function("kernel32.dll", "VirtualAlloc", [](HleCallContext& ctx) -> u64 {
        log::info("HLE", "VirtualAlloc(lpAddress=0x{:X}, dwSize=0x{:X}, flAllocationType=0x{:X}, flProtect=0x{:X})",
                  ctx.rcx, ctx.rdx, ctx.r8, ctx.r9);
        // Simple bump/identity allocation for guest Title address space
        static u64 next_guest_alloc = 0x20000000ULL; // 512MB mark
        u64 allocated = (ctx.rcx != 0) ? ctx.rcx : next_guest_alloc;
        if (ctx.rcx == 0) {
            next_guest_alloc = (next_guest_alloc + ctx.rdx + 0xFFFULL) & ~0xFFFULL;
        }
        return allocated;
    });

    thunk_mgr_.register_function("kernel32.dll", "OutputDebugStringA", [](HleCallContext& ctx) -> u64 {
        if (ctx.host_ram_base && ctx.rcx < ctx.ram_size) {
            const char* msg = reinterpret_cast<const char*>(static_cast<const u8*>(ctx.host_ram_base) + ctx.rcx);
            log::info("TITLE_LOG", "{}", msg);
        }
        return 0;
    });

    // === xg.dll (Xbox Graphics API) ===
    thunk_mgr_.register_function("xg.dll", "XgSubmitCommandRing", [](HleCallContext& ctx) -> u64 {
        log::debug("HLE", "XgSubmitCommandRing: ring_gpa=0x{:X}, dwords={}", ctx.rcx, ctx.rdx);
        return 0; // S_OK
    });

    thunk_mgr_.register_function("xg.dll", "XgCreateDevice", [](HleCallContext& ctx) -> u64 {
        log::info("HLE", "XgCreateDevice invoked");
        return 0; // S_OK
    });
}

Result<storage::LoadedPeImage> Kernel::load_title_executable(
    std::string_view exe_path,
    void* host_ram_base,
    u64 ram_size
) {
    log::info("KERNEL", "Loading and preparing title PE64 executable: {}", exe_path);

    if (!vfs_ || !vfs_->exists(exe_path)) {
        log::error("KERNEL", "Executable not found in VFS: {}", exe_path);
        return ErrorCode::InvalidParameter;
    }

    auto node = vfs_->resolve(exe_path);
    auto data_res = node->read_all();
    if (!data_res) {
        return data_res.error();
    }

    // Load PE image into guest memory (base = 0x0040_0000 or preferred base)
    auto image_res = pe_loader_.load_image(*data_res, 0x00400000ULL, host_ram_base, ram_size);
    if (!image_res) {
        log::error("KERNEL", "Failed to load PE image");
        return image_res.error();
    }

    // Bind all imported symbols to HLE trampolines
    auto bind_res = thunk_mgr_.bind_imports(image_res->imports, host_ram_base, ram_size);
    if (!bind_res) {
        log::error("KERNEL", "Failed to bind PE imports");
        return bind_res.error();
    }

    log::info("KERNEL", "Title executable ready to execute at Entry Point 0x{:X}", image_res->entry_point);
    return *image_res;
}

} // namespace papaya::hle
