#include "papaya/common/logger.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include "papaya/storage/pe_loader.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

// Helper to construct a synthetic title PE that exercises HLE APIs
std::vector<papaya::u8> create_test_game_binary() {
    using namespace papaya;
    using namespace papaya::storage;

    std::vector<u8> buffer(8192, 0);

    // 1. DOS Header
    auto* dos = reinterpret_cast<ImageDosHeader*>(buffer.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = sizeof(ImageDosHeader);

    // 2. NT Headers
    auto* nt = reinterpret_cast<ImageNtHeaders64*>(buffer.data() + dos->e_lfanew);
    nt->signature = IMAGE_NT_SIGNATURE;
    nt->file_header.machine = IMAGE_FILE_MACHINE_AMD64;
    nt->file_header.number_of_sections = 2;
    nt->file_header.size_of_optional_header = sizeof(ImageOptionalHeader64);

    nt->optional_header.magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->optional_header.image_base = 0x00400000;
    nt->optional_header.section_alignment = 0x1000;
    nt->optional_header.file_alignment = 0x200;
    nt->optional_header.address_of_entry_point = 0x1000;
    nt->optional_header.size_of_image = 0x5000;
    nt->optional_header.size_of_headers = 0x200;

    // 3. Section Headers (.text, .rdata)
    auto* sections = reinterpret_cast<ImageSectionHeader*>(
        reinterpret_cast<u8*>(nt) + sizeof(u32) + sizeof(ImageFileHeader) + sizeof(ImageOptionalHeader64)
    );

    // .text
    std::memcpy(sections[0].name, ".text\0\0\0", 8);
    sections[0].virtual_address = 0x1000;
    sections[0].misc.virtual_size = 0x1000;
    sections[0].pointer_to_raw_data = 0x200;
    sections[0].size_of_raw_data = 0x200;

    // .rdata (IAT)
    std::memcpy(sections[1].name, ".rdata\0\0", 8);
    sections[1].virtual_address = 0x2000;
    sections[1].misc.virtual_size = 0x1000;
    sections[1].pointer_to_raw_data = 0x400;
    sections[1].size_of_raw_data = 0x200;

    // Machine code in .text:
    // 1. sub rsp, 40
    // 2. mov rcx, 0; mov rdx, 0x1000; mov rax, [IAT_VirtualAlloc]; call rax
    // 3. mov rcx, 0; mov rdx, 0x00402800 (pState); mov rax, [IAT_XInputGetState]; call rax
    // 4. add rsp, 40
    // 5. hlt
    u8* code = buffer.data() + sections[0].pointer_to_raw_data;
    size_t pos = 0;

    // sub rsp, 40
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xEC; code[pos++] = 0x28;

    // mov rcx, 0
    code[pos++] = 0x48; code[pos++] = 0x31; code[pos++] = 0xC9;
    // mov rdx, 0x1000
    code[pos++] = 0x48; code[pos++] = 0xC7; code[pos++] = 0xC2;
    code[pos++] = 0x00; code[pos++] = 0x10; code[pos++] = 0x00; code[pos++] = 0x00;
    // mov rax, 0x00402000 (IAT VirtualAlloc)
    code[pos++] = 0x48; code[pos++] = 0xB8;
    u64 iat_va = 0x00402000ULL;
    std::memcpy(&code[pos], &iat_va, 8);
    pos += 8;
    // call qword ptr [rax]
    code[pos++] = 0xFF; code[pos++] = 0x10;

    // mov rcx, 0
    code[pos++] = 0x48; code[pos++] = 0x31; code[pos++] = 0xC9;
    // mov rdx, 0x00402800
    code[pos++] = 0x48; code[pos++] = 0xC7; code[pos++] = 0xC2;
    code[pos++] = 0x00; code[pos++] = 0x28; code[pos++] = 0x40; code[pos++] = 0x00;
    // mov rax, 0x00402008 (IAT XInputGetState)
    code[pos++] = 0x48; code[pos++] = 0xB8;
    u64 iat_xi = 0x00402008ULL;
    std::memcpy(&code[pos], &iat_xi, 8);
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

    log::info("TEST", "Running unit test: test_full_game_loop");

    EmulatorConfig config{
        .target = ConsoleTarget::XboxOne,
        .backend = PlatformBackend::Kvm,
        .headless = true,
        .target_fps = 60,
        .boot_title_path = ""
    };

    EmulatorRuntime runtime(config);
    assert(runtime.initialize().has_value());

    // Inject controller input into user 0
    input::GamepadState mock_pad{};
    mock_pad.buttons = input::XINPUT_GAMEPAD_A | input::XINPUT_GAMEPAD_START;
    mock_pad.thumb_lx = 16000;
    runtime.get_input().set_gamepad_state(0, mock_pad);

    // Setup HLE IAT entries at 0x00402000 (VirtualAlloc) and 0x00402008 (XInputGetState)
    auto& kernel = runtime.get_kernel();
    auto& thunk_mgr = kernel.get_thunk_manager();

    void* ram = runtime.get_memory_map().get_host_pointer(0x0);
    u64 total_ram = runtime.get_memory_map().get_total_ram_size();
    assert(ram != nullptr);

    // Load synthetic game image directly into registered KVM guest RAM
    auto game_bytes = create_test_game_binary();
    storage::PeLoader loader;
    auto load_res = loader.load_image(game_bytes, 0x00400000, ram, total_ram);
    assert(load_res.has_value());

    std::vector<storage::PeImportEntry> imports = {
        { .module_name = "kernel32.dll", .function_name = "VirtualAlloc", .ordinal = 0, .is_ordinal = false, .iat_gva = 0x00402000 },
        { .module_name = "xinput1_4.dll", .function_name = "XInputGetState", .ordinal = 0, .is_ordinal = false, .iat_gva = 0x00402008 }
    };
    thunk_mgr.write_trampolines_to_guest(ram, total_ram);
    assert(thunk_mgr.bind_imports(imports, ram, total_ram).has_value());

    // Boot Primary vCPU
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
            log::info("TEST", "Synthetic game binary executed and halted cleanly!");
        }
    }

    assert(halted);

    // Verify XInput state was written to guest memory at 0x00402800
    const auto* pad_out = reinterpret_cast<const input::GamepadState*>(static_cast<u8*>(ram) + 0x00402800 + sizeof(u32));
    assert((pad_out->buttons & input::XINPUT_GAMEPAD_A) != 0);
    assert((pad_out->buttons & input::XINPUT_GAMEPAD_START) != 0);
    assert(pad_out->thumb_lx == 16000);

    log::info("TEST", "Verified guest read controller inputs from XInputGetState!");

    runtime.stop();

    log::info("TEST", ">>> test_full_game_loop PASSED ALL CHECKS! <<<");
    return 0;
}
