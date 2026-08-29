#include "papaya/common/logger.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/hv/memory_map.hpp"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace papaya;

    log::info("TEST", "Running unit test: test_kvm_boot");

    auto hv = hv::create_hypervisor(PlatformBackend::Kvm);
    assert(hv != nullptr);

    auto init_res = hv->initialize();
    if (!init_res) {
        log::error("TEST", "Hypervisor initialization failed in unit test: {}", error_to_string(init_res.error()));
        return 1;
    }

    hv::MemoryMap mem;
    auto map_res = mem.map_region("TestRAM", 0x0, 2 * MiB, false);
    assert(map_res.has_value());

    auto cfg_res = hv->configure_memory(mem);
    assert(cfg_res.has_value());

    // 16-bit real-mode machine code:
    // 0xB8, 0x2A, 0x00 -> mov ax, 42
    // 0xF4             -> hlt
    const u8 code[] = { 0xB8, 0x2A, 0x00, 0xF4 };
    void* host_addr = mem.get_host_pointer(0x1000);
    assert(host_addr != nullptr);
    std::memcpy(host_addr, code, sizeof(code));

    auto vcpu_res = hv->create_vcpu(0);
    assert(vcpu_res.has_value());
    auto vcpu = *vcpu_res;

    auto state_res = vcpu->setup_initial_state(0x1000, 0x4000);
    assert(state_res.has_value());

    auto exit_res = vcpu->run_once();
    assert(exit_res.has_value());
    assert(exit_res->reason == hv::ExitReason::Halt);

    auto regs_res = vcpu->get_registers();
    assert(regs_res.has_value());
    assert((regs_res->rax & 0xFFFF) == 42);

    log::info("TEST", "test_kvm_boot passed! (AX={})", regs_res->rax & 0xFFFF);
    return 0;
}
