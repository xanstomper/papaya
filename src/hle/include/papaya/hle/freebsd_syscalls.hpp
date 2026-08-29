#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hle/memory_manager.hpp"
#include "papaya/hle/thread_manager.hpp"
#include <functional>
#include <unordered_map>

namespace papaya::hle {

// FreeBSD 9 & 12 Syscall Numbers (FreeBSD ABI)
namespace Syscalls {
    constexpr u32 kExit                 = 1;
    constexpr u32 kFork                 = 2;
    constexpr u32 kRead                 = 3;
    constexpr u32 kWrite                = 4;
    constexpr u32 kOpen                 = 5;
    constexpr u32 kClose                = 6;
    constexpr u32 kMunmap               = 73;
    constexpr u32 kMprotect             = 74;
    constexpr u32 kGetpid               = 20;
    constexpr u32 kThrCreate           = 430;
    constexpr u32 kThrExit             = 431;
    constexpr u32 kThrSelf             = 432;
    constexpr u32 kThrSuspendUcontext = 433;
    constexpr u32 kUmtxOp              = 454;
    constexpr u32 kMmap                 = 477;
    constexpr u32 kNamedobjCreate      = 557;
    constexpr u32 kNamedobjDelete      = 558;
    constexpr u32 kEventflagCreate     = 538;
    constexpr u32 kEventflagWait       = 540;
    constexpr u32 kEventflagSet        = 541;
}

// FreeBSD umtx_op operations
constexpr u32 UMTX_OP_WAIT_UINT_PRIVATE = 15;
constexpr u32 UMTX_OP_WAKE_PRIVATE      = 16;
constexpr u32 UMTX_OP_MUTEX_LOCK        = 5;
constexpr u32 UMTX_OP_MUTEX_UNLOCK      = 6;

struct SyscallContext {
    u64 syscall_num{0};
    u64 arg0{0}; // rdi
    u64 arg1{0}; // rsi
    u64 arg2{0}; // rdx
    u64 arg3{0}; // rcx / r10
    u64 arg4{0}; // r8
    u64 arg5{0}; // r9
    void* host_ram_base{nullptr};
    u64 host_ram_size{0};
};

using SyscallHandler = std::function<u64(SyscallContext&)>;

class FreeBsdSyscallDispatcher {
public:
    FreeBsdSyscallDispatcher(MemoryManager& mem_mgr, ThreadManager& thr_mgr);
    ~FreeBsdSyscallDispatcher();

    Result<> initialize();
    void register_syscall(u32 syscall_num, SyscallHandler handler);
    u64 dispatch(u32 syscall_num, SyscallContext& ctx);

private:
    void register_core_syscalls();

    MemoryManager& mem_mgr_;
    ThreadManager& thr_mgr_;
    std::unordered_map<u32, SyscallHandler> handlers_;
};

} // namespace papaya::hle
