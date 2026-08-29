#include "papaya/hle/freebsd_syscalls.hpp"
#include "papaya/hle/sync_primitives.hpp"
#include "papaya/common/logger.hpp"
#include <iostream>

namespace papaya::hle {

FreeBsdSyscallDispatcher::FreeBsdSyscallDispatcher(MemoryManager& mem_mgr, ThreadManager& thr_mgr)
    : mem_mgr_(mem_mgr), thr_mgr_(thr_mgr) {}

FreeBsdSyscallDispatcher::~FreeBsdSyscallDispatcher() = default;

Result<> FreeBsdSyscallDispatcher::initialize() {
    log::info("KERNEL", "Initializing FreeBSD 9/12 Syscall Translation Layer");
    register_core_syscalls();
    return {};
}

void FreeBsdSyscallDispatcher::register_syscall(u32 syscall_num, SyscallHandler handler) {
    handlers_[syscall_num] = std::move(handler);
}

u64 FreeBsdSyscallDispatcher::dispatch(u32 syscall_num, SyscallContext& ctx) {
    auto it = handlers_.find(syscall_num);
    if (it != handlers_.end()) {
        return it->second(ctx);
    }

    log::warn("KERNEL", "Unhandled FreeBSD Syscall #{}: args=(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X})",
              syscall_num, ctx.arg0, ctx.arg1, ctx.arg2, ctx.arg3);
    return static_cast<u64>(-1);
}

void FreeBsdSyscallDispatcher::register_core_syscalls() {
    // SYS_mmap (477)
    register_syscall(Syscalls::kMmap, [this](SyscallContext& ctx) -> u64 {
        GuestVirtAddr addr = ctx.arg0;
        u64 len = ctx.arg1;
        u32 prot = static_cast<u32>(ctx.arg2);
        u32 flags = static_cast<u32>(ctx.arg3);
        s32 fd = static_cast<s32>(ctx.arg4);
        s64 off = static_cast<s64>(ctx.arg5);

        auto res = mem_mgr_.sys_mmap(addr, len, prot, flags, fd, off);
        return res.has_value() ? *res : static_cast<u64>(-1);
    });

    // SYS_munmap (73)
    register_syscall(Syscalls::kMunmap, [this](SyscallContext& ctx) -> u64 {
        GuestVirtAddr addr = ctx.arg0;
        u64 len = ctx.arg1;
        return mem_mgr_.sys_munmap(addr, len).has_value() ? 0 : static_cast<u64>(-1);
    });

    // SYS_mprotect (74)
    register_syscall(Syscalls::kMprotect, [this](SyscallContext& ctx) -> u64 {
        GuestVirtAddr addr = ctx.arg0;
        u64 len = ctx.arg1;
        u32 prot = static_cast<u32>(ctx.arg2);
        return mem_mgr_.sys_mprotect(addr, len, prot).has_value() ? 0 : static_cast<u64>(-1);
    });

    // SYS_getpid (20)
    register_syscall(Syscalls::kGetpid, [](SyscallContext&) -> u64 {
        return 0x505; // Orbis / Prospero process ID
    });

    // SYS_thr_create (430)
    register_syscall(Syscalls::kThrCreate, [this](SyscallContext& ctx) -> u64 {
        GuestVirtAddr ucontext = ctx.arg0;
        GuestVirtAddr id_ptr = ctx.arg1;
        s32 flags = static_cast<s32>(ctx.arg2);

        auto res = thr_mgr_.create_thread(ucontext, 0, 1 * MiB, flags);
        if (res.has_value()) {
            if (id_ptr != 0 && ctx.host_ram_base) {
                auto* ram = static_cast<u8*>(ctx.host_ram_base);
                *reinterpret_cast<s64*>(ram + id_ptr) = *res;
            }
            return 0; // Success
        }
        return static_cast<u64>(-1);
    });

    // SYS_thr_exit (431)
    register_syscall(Syscalls::kThrExit, [this](SyscallContext& ctx) -> u64 {
        thr_mgr_.exit_thread(thr_mgr_.get_current_tid());
        return 0;
    });

    // SYS_thr_self (432)
    register_syscall(Syscalls::kThrSelf, [this](SyscallContext& ctx) -> u64 {
        GuestVirtAddr id_ptr = ctx.arg0;
        u32 tid = thr_mgr_.get_current_tid();
        if (id_ptr != 0 && ctx.host_ram_base) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            *reinterpret_cast<s64*>(ram + id_ptr) = tid;
        }
        return 0;
    });

    // SYS_umtx_op (454)
    register_syscall(Syscalls::kUmtxOp, [](SyscallContext& ctx) -> u64 {
        GuestVirtAddr obj = ctx.arg0;
        u32 op = static_cast<u32>(ctx.arg1);
        u64 val = ctx.arg2;

        if (!ctx.host_ram_base || obj == 0) return static_cast<u64>(-1);
        auto* ram = static_cast<u8*>(ctx.host_ram_base);
        void* host_addr = ram + obj;

        if (op == UMTX_OP_WAIT_UINT_PRIVATE) {
            return HleFutex::wait_on_address(host_addr, val, sizeof(u32), 0) ? 0 : static_cast<u64>(-1);
        } else if (op == UMTX_OP_WAKE_PRIVATE) {
            HleFutex::wake_by_address_all(host_addr);
            return 0;
        }
        return 0;
    });

    // SYS_write (4) - Standard Output / Error for debugging
    register_syscall(Syscalls::kWrite, [](SyscallContext& ctx) -> u64 {
        s32 fd = static_cast<s32>(ctx.arg0);
        GuestVirtAddr buf = ctx.arg1;
        u64 nbytes = ctx.arg2;

        if (ctx.host_ram_base && buf != 0) {
            auto* ram = static_cast<u8*>(ctx.host_ram_base);
            const char* msg = reinterpret_cast<const char*>(ram + buf);
            std::string str(msg, std::min(nbytes, static_cast<u64>(1024)));
            log::info("SCE_STDOUT", "[fd {}]: {}", fd, str);
        }
        return nbytes;
    });
}

} // namespace papaya::hle
