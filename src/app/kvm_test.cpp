#include "kvm_test.hpp"
#include "papaya/common/logger.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/hv/memory_map.hpp"
#include <cstring>
#include <vector>

namespace papaya::app {

Result<> run_kvm_diagnostics() {
    log::info("TEST", "=================================================");
    log::info("TEST", "Starting Papaya KVM Hardware Virtualization Test");
    log::info("TEST", "=================================================");

    auto hv = hv::create_hypervisor(PlatformBackend::Kvm);
    if (!hv) {
        log::error("TEST", "Failed to create KVM hypervisor instance");
        return ErrorCode::HypervisorInitFailed;
    }

    auto init_res = hv->initialize();
    if (!init_res) {
        log::error("TEST", "Hypervisor initialization failed: {}", error_to_string(init_res.error()));
        return init_res;
    }

    // Allocate 16MB test guest memory
    hv::MemoryMap mem_map;
    constexpr u64 TEST_MEM_SIZE = 16 * MiB;
    constexpr GuestPhysAddr CODE_ENTRY_GPA = 0x1000;

    auto map_res = mem_map.map_region("Diagnostic_RAM", 0x0, TEST_MEM_SIZE, false);
    if (!map_res) {
        return map_res;
    }

    auto cfg_res = hv->configure_memory(mem_map);
    if (!cfg_res) {
        return cfg_res;
    }

    // Write x86 machine code payload into guest physical memory at CODE_ENTRY_GPA:
    // Prints "PAPAYA-EMU OK\n" via OUT 0xE0, AL and halts.
    const u8 payload[] = {
        0xB0, 'P',  0xE6, 0xE0, // mov al, 'P'; out 0xe0, al
        0xB0, 'A',  0xE6, 0xE0, // mov al, 'A'; out 0xe0, al
        0xB0, 'P',  0xE6, 0xE0, // mov al, 'P'; out 0xe0, al
        0xB0, 'A',  0xE6, 0xE0, // mov al, 'A'; out 0xe0, al
        0xB0, 'Y',  0xE6, 0xE0, // mov al, 'Y'; out 0xe0, al
        0xB0, 'A',  0xE6, 0xE0, // mov al, 'A'; out 0xe0, al
        0xB0, '-',  0xE6, 0xE0, // mov al, '-'; out 0xe0, al
        0xB0, 'E',  0xE6, 0xE0, // mov al, 'E'; out 0xe0, al
        0xB0, 'M',  0xE6, 0xE0, // mov al, 'M'; out 0xe0, al
        0xB0, 'U',  0xE6, 0xE0, // mov al, 'U'; out 0xe0, al
        0xB0, ' ',  0xE6, 0xE0, // mov al, ' '; out 0xe0, al
        0xB0, 'O',  0xE6, 0xE0, // mov al, 'O'; out 0xe0, al
        0xB0, 'K',  0xE6, 0xE0, // mov al, 'K'; out 0xe0, al
        0xB0, '\n', 0xE6, 0xE0, // mov al, '\n'; out 0xe0, al
        0xF4                    // hlt
    };

    void* dest_hva = mem_map.get_host_pointer(CODE_ENTRY_GPA);
    if (!dest_hva) {
        log::error("TEST", "Failed to resolve host virtual address for GPA 0x{:X}", CODE_ENTRY_GPA);
        return ErrorCode::MemoryMappingFailed;
    }
    std::memcpy(dest_hva, payload, sizeof(payload));
    log::info("TEST", "Injected {} bytes test payload at Guest Phys 0x{:X}", sizeof(payload), CODE_ENTRY_GPA);

    // Create vCPU #0
    auto vcpu_res = hv->create_vcpu(0);
    if (!vcpu_res) {
        return vcpu_res.error();
    }
    auto vcpu = *vcpu_res;

    auto state_res = vcpu->setup_initial_state(CODE_ENTRY_GPA, 0x8000);
    if (!state_res) {
        return state_res;
    }

    log::info("TEST", "Entering vCPU execution loop");
    std::string captured_output;
    bool halted = false;

    for (int step = 0; step < 100 && !halted; ++step) {
        auto exit_res = vcpu->run_once();
        if (!exit_res) {
            log::error("TEST", "vCPU run failed on step {}", step);
            return exit_res.error();
        }

        const auto& exit = *exit_res;
        if (exit.reason == hv::ExitReason::IoOut) {
            if (exit.address == 0xE0) {
                char ch = static_cast<char>(exit.data & 0xFF);
                captured_output += ch;
            }
        } else if (exit.reason == hv::ExitReason::Halt) {
            log::info("TEST", "vCPU executed HLT (clean guest stop) after {} exits", step + 1);
            halted = true;
        } else {
            log::warn("TEST", "Unexpected exit reason: {}", static_cast<int>(exit.reason));
            break;
        }
    }

    if (halted) {
        log::info("TEST", "Captured Guest Output: \"{}\"", captured_output.substr(0, captured_output.size() - 1));
        log::info("TEST", ">>> KVM VIRTUALIZATION ENGINE PASSED ALL CHECKS! <<<");
        return {};
    } else {
        log::error("TEST", "vCPU did not halt within iteration budget");
        return ErrorCode::VcpuRunFailed;
    }
}

} // namespace papaya::app
