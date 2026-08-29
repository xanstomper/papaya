#include "papaya/common/logger.hpp"
#include "papaya/hle/freebsd_syscalls.hpp"
#include "papaya/hle/memory_manager.hpp"
#include "papaya/hle/thread_manager.hpp"
#include <cassert>
#include <vector>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::hle;

    log::info("TEST", "Running unit test: test_freebsd_syscalls");

    std::vector<u8> host_ram(64 * MiB, 0);
    MemoryManager mem_mgr(ConsoleTarget::PlayStation4);
    assert(mem_mgr.initialize(host_ram.data(), host_ram.size()).has_value());

    HandleTable handle_table;
    ThreadManager thr_mgr(handle_table);

    FreeBsdSyscallDispatcher dispatcher(mem_mgr, thr_mgr);
    assert(dispatcher.initialize().has_value());

    // 1. Test SYS_getpid (20)
    SyscallContext ctx{};
    u64 pid = dispatcher.dispatch(Syscalls::kGetpid, ctx);
    assert(pid == 0x505);
    log::info("TEST", "SYS_getpid returned: 0x{:X}", pid);

    // 2. Test SYS_mmap (477)
    ctx.arg0 = 0;           // addr
    ctx.arg1 = 2 * MiB;     // length
    ctx.arg2 = SCE_PROT_ALL;// prot
    ctx.arg3 = SCE_MAP_ANONYMOUS | SCE_MAP_PRIVATE; // flags
    ctx.arg4 = static_cast<u64>(-1); // fd
    ctx.arg5 = 0;           // offset

    u64 mmap_addr = dispatcher.dispatch(Syscalls::kMmap, ctx);
    assert(mmap_addr != static_cast<u64>(-1));
    assert(mmap_addr > 0);
    log::info("TEST", "SYS_mmap returned GVA: 0x{:X}", mmap_addr);

    // 3. Test SYS_munmap (73)
    ctx.arg0 = mmap_addr;
    ctx.arg1 = 2 * MiB;
    u64 unmap_res = dispatcher.dispatch(Syscalls::kMunmap, ctx);
    assert(unmap_res == 0);

    // 4. Test SYS_thr_self (432)
    ctx.arg0 = 0x1000;
    ctx.host_ram_base = host_ram.data();
    ctx.host_ram_size = host_ram.size();
    u64 thr_res = dispatcher.dispatch(Syscalls::kThrSelf, ctx);
    assert(thr_res == 0);
    s64 written_tid = *reinterpret_cast<s64*>(host_ram.data() + 0x1000);
    assert(written_tid == static_cast<s64>(thr_mgr.get_current_tid()));

    log::info("TEST", ">>> test_freebsd_syscalls PASSED ALL CHECKS! <<<");
    return 0;
}
