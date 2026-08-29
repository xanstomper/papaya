#include "papaya/common/logger.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include "papaya/storage/elf_loader.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

// Helper to construct a synthetic PlayStation 4/5 ELF executable
std::vector<papaya::u8> create_test_ps_elf() {
    using namespace papaya;
    using namespace papaya::storage;

    std::vector<u8> buffer(8192, 0);

    // 1. ELF64 Header
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(buffer.data());
    std::memcpy(ehdr->e_ident, "\x7f\x45\x4c\x46\x02\x01\x01\x09", 8); // ELF64, Little-Endian, FreeBSD ABI
    ehdr->e_type = ET_SCE_EXEC;
    ehdr->e_machine = EM_X86_64;
    ehdr->e_version = 1;
    ehdr->e_entry = 0x1000; // Entry point RVA relative to image base
    ehdr->e_phoff = sizeof(Elf64_Ehdr);
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 2;

    // 2. Program Headers (.text, .data)
    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(buffer.data() + sizeof(Elf64_Ehdr));

    // .text
    phdrs[0].p_type = PT_LOAD;
    phdrs[0].p_flags = PF_R | PF_X;
    phdrs[0].p_offset = 0x200;
    phdrs[0].p_vaddr = 0x1000;
    phdrs[0].p_paddr = 0x1000;
    phdrs[0].p_filesz = 0x200;
    phdrs[0].p_memsz = 0x200;
    phdrs[0].p_align = 0x1000;

    // .data
    phdrs[1].p_type = PT_LOAD;
    phdrs[1].p_flags = PF_R | PF_W;
    phdrs[1].p_offset = 0x400;
    phdrs[1].p_vaddr = 0x2000;
    phdrs[1].p_paddr = 0x2000;
    phdrs[1].p_filesz = 0x200;
    phdrs[1].p_memsz = 0x200;
    phdrs[1].p_align = 0x1000;

    // Machine code in .text:
    // 1. sub rsp, 40
    // 2. mov rcx, 0; mov rdx, 0; mov r8, 0x200000; mov r9, 0x200000; mov rax, [IAT_sceKernelAllocateDirectMemory]; call rax
    // 3. mov rcx, 0; mov rdx, 0x00402800 (pData); mov rax, [IAT_scePadReadState]; call rax
    // 4. add rsp, 40
    // 5. hlt
    u8* code = buffer.data() + phdrs[0].p_offset;
    size_t pos = 0;

    // sub rsp, 40
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xEC; code[pos++] = 0x28;

    // mov rcx, 0; mov rdx, 0
    code[pos++] = 0x48; code[pos++] = 0x31; code[pos++] = 0xC9;
    code[pos++] = 0x48; code[pos++] = 0x31; code[pos++] = 0xD2;

    // mov r8, 0x200000; mov r9, 0x200000
    code[pos++] = 0x49; code[pos++] = 0xC7; code[pos++] = 0xC0;
    code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x20; code[pos++] = 0x00;
    code[pos++] = 0x49; code[pos++] = 0xC7; code[pos++] = 0xC1;
    code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x20; code[pos++] = 0x00;

    // mov rax, 0x00402000 (IAT sceKernelAllocateDirectMemory)
    code[pos++] = 0x48; code[pos++] = 0xB8;
    u64 iat_alloc = 0x00402000ULL;
    std::memcpy(&code[pos], &iat_alloc, 8);
    pos += 8;
    // call qword ptr [rax]
    code[pos++] = 0xFF; code[pos++] = 0x10;

    // mov rcx, 0; mov rdx, 0x00402800
    code[pos++] = 0x48; code[pos++] = 0x31; code[pos++] = 0xC9;
    code[pos++] = 0x48; code[pos++] = 0xC7; code[pos++] = 0xC2;
    code[pos++] = 0x00; code[pos++] = 0x28; code[pos++] = 0x40; code[pos++] = 0x00;

    // mov rax, 0x00402008 (IAT scePadReadState)
    code[pos++] = 0x48; code[pos++] = 0xB8;
    u64 iat_pad = 0x00402008ULL;
    std::memcpy(&code[pos], &iat_pad, 8);
    pos += 8;
    // call qword ptr [rax]
    code[pos++] = 0xFF; code[pos++] = 0x10;

    // add rsp, 40
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xC4; code[pos++] = 0x28;

    // hlt
    code[pos++] = 0xF4;

    return buffer;
}

int main() {
    using namespace papaya;
    using namespace papaya::frontend;

    log::info("TEST", "Running unit test: test_full_game_loop (PlayStation 4/5)");

    EmulatorConfig config{
        .target = ConsoleTarget::PlayStation4,
        .backend = PlatformBackend::Kvm,
        .headless = true,
        .target_fps = 60,
        .boot_title_path = ""
    };

    EmulatorRuntime runtime(config);
    assert(runtime.initialize().has_value());

    // Inject DualShock / DualSense controller input (Cross + Square buttons pressed)
    input::GamepadState mock_pad{};
    mock_pad.buttons = 0x00002000 | 0x00004000; // Cross + Square
    mock_pad.thumb_lx = 12000;
    runtime.get_input().set_gamepad_state(0, mock_pad);

    auto& kernel = runtime.get_kernel();
    auto& thunk_mgr = kernel.get_thunk_manager();

    void* ram = runtime.get_memory_map().get_host_pointer(0x0);
    u64 total_ram = runtime.get_memory_map().get_total_ram_size();
    assert(ram != nullptr);

    // Load PlayStation ELF directly into registered guest memory
    auto elf_bytes = create_test_ps_elf();
    storage::ElfLoader loader;
    auto load_res = loader.load_image(elf_bytes, 0x00400000, ram, total_ram);
    assert(load_res.has_value());
    assert(load_res->entry_point == 0x00401000);

    // Setup IAT table for Sony NIDs / PRX functions
    std::vector<storage::PeImportEntry> imports = {
        { .module_name = "libkernel.prx", .function_name = "sceKernelAllocateDirectMemory", .ordinal = 0, .is_ordinal = false, .iat_gva = 0x00402000 },
        { .module_name = "libScePad.prx", .function_name = "scePadReadState", .ordinal = 0, .is_ordinal = false, .iat_gva = 0x00402008 }
    };
    thunk_mgr.write_trampolines_to_guest(ram, total_ram);
    assert(thunk_mgr.bind_imports(imports, ram, total_ram).has_value());

    // Boot Primary vCPU at entry point
    auto vcpu_res = runtime.get_hypervisor().create_vcpu(0);
    assert(vcpu_res.has_value());
    auto vcpu = *vcpu_res;
    assert(vcpu->setup_long_mode(load_res->entry_point, 0x00080000).has_value());

    // Execute game loop
    bool halted = false;
    for (int step = 0; step < 50 && !halted; ++step) {
        auto exit_res = vcpu->run_once();
        assert(exit_res.has_value());

        const auto& exit = *exit_res;
        if (exit.reason == hv::ExitReason::IoOut) {
            if (exit.address == hle::HLE_HYPERCALL_IO_PORT) {
                u32 thunk_id = static_cast<u32>(exit.data & 0xFFFFFFFF);
                assert(thunk_mgr.execute_thunk(thunk_id, *vcpu, ram, total_ram).has_value());
            }
        } else if (exit.reason == hv::ExitReason::Halt) {
            halted = true;
            log::info("TEST", "PlayStation title executed and halted cleanly!");
        }
    }

    assert(halted);

    // Verify ScePad state was written to guest memory at 0x00402800
    const auto* pad_out = reinterpret_cast<const u32*>(static_cast<u8*>(ram) + 0x00402800);
    assert((*pad_out & 0x00002000) != 0); // Cross button
    assert((*pad_out & 0x00004000) != 0); // Square button

    log::info("TEST", "Verified PlayStation guest read controller inputs from scePadReadState!");

    runtime.stop();

    log::info("TEST", ">>> test_full_game_loop PASSED ALL CHECKS! <<<");
    return 0;
}
