#include "papaya/common/logger.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/hv/memory_map.hpp"
#include "papaya/hv/kvm/kvm_long_mode.hpp"
#include "papaya/storage/pe_loader.hpp"
#include "papaya/hle/hle_thunk.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

// Helper to construct a PE64 test payload that calls an IAT import and tests GS base
std::vector<papaya::u8> create_test_pe64_with_import() {
    using namespace papaya;
    using namespace papaya::storage;

    std::vector<u8> buffer(4096, 0);

    // 1. DOS Header
    auto* dos = reinterpret_cast<ImageDosHeader*>(buffer.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = sizeof(ImageDosHeader);

    // 2. NT Headers
    auto* nt = reinterpret_cast<ImageNtHeaders64*>(buffer.data() + dos->e_lfanew);
    nt->signature = IMAGE_NT_SIGNATURE;
    nt->file_header.machine = IMAGE_FILE_MACHINE_AMD64;
    nt->file_header.number_of_sections = 1;
    nt->file_header.size_of_optional_header = sizeof(ImageOptionalHeader64);

    nt->optional_header.magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->optional_header.image_base = 0x00400000;
    nt->optional_header.section_alignment = 0x1000;
    nt->optional_header.file_alignment = 0x200;
    nt->optional_header.address_of_entry_point = 0x1000;
    nt->optional_header.size_of_image = 0x3000;
    nt->optional_header.size_of_headers = 0x200;

    // 3. Section Header (.text)
    auto* sec = reinterpret_cast<ImageSectionHeader*>(
        reinterpret_cast<u8*>(nt) + sizeof(u32) + sizeof(ImageFileHeader) + sizeof(ImageOptionalHeader64)
    );
    std::memcpy(sec->name, ".text\0\0\0", 8);
    sec->virtual_address = 0x1000;
    sec->misc.virtual_size = 0x1000;
    sec->pointer_to_raw_data = 0x200;
    sec->size_of_raw_data = 0x200;

    // 64-bit machine code in .text:
    // 1. Read GS:[0x30] (TEB self pointer) -> RAX:  65 48 8B 04 25 30 00 00 00 (mov rax, gs:[0x30])
    // 2. Send low byte of TEB pointer to port 0xE0: B0 20 E6 E0 (mov al, 0x20; out 0xe0, al)
    // 3. Setup Win64 ABI call: rcx = 50, rdx = 70
    //    48 C7 C1 32 00 00 00 (mov rcx, 50)
    //    48 C7 C2 46 00 00 00 (mov rdx, 70)
    //    48 B8 [IAT_ADDR] (mov rax, 0x00402000)
    //    FF 10 (call qword ptr [rax])
    // 4. Send low byte of result (120 = 0x78) to port 0xE0
    //    E6 E0 (out 0xe0, al)
    // 5. HLT (F4)
    u8* code = buffer.data() + sec->pointer_to_raw_data;
    size_t pos = 0;

    // mov rax, gs:[0x30]
    code[pos++] = 0x65; code[pos++] = 0x48; code[pos++] = 0x8B; code[pos++] = 0x04;
    code[pos++] = 0x25; code[pos++] = 0x30; code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x00;

    // mov al, 0xAA; out 0xE0, al (signal TEB check point)
    code[pos++] = 0xB0; code[pos++] = 0xAA; code[pos++] = 0xE6; code[pos++] = 0xE0;

    // mov rcx, 50
    code[pos++] = 0x48; code[pos++] = 0xC7; code[pos++] = 0xC1;
    code[pos++] = 50; code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x00;

    // mov rdx, 70
    code[pos++] = 0x48; code[pos++] = 0xC7; code[pos++] = 0xC2;
    code[pos++] = 70; code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x00;

    // sub rsp, 40 (allocate Win64 shadow space + alignment)
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xEC; code[pos++] = 0x28;

    // mov rax, 0x00402000 (IAT address)
    code[pos++] = 0x48; code[pos++] = 0xB8;
    u64 iat_addr = 0x00402000ULL;
    std::memcpy(&code[pos], &iat_addr, 8);
    pos += 8;

    // call qword ptr [rax]
    code[pos++] = 0xFF; code[pos++] = 0x10;

    // add rsp, 40 (restore stack)
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xC4; code[pos++] = 0x28;

    // out 0xE0, al (output return value in AL)
    code[pos++] = 0xE6; code[pos++] = 0xE0;

    // hlt
    code[pos++] = 0xF4;

    return buffer;
}

int main() {
    using namespace papaya;
    using namespace papaya::hv;
    using namespace papaya::hle;
    using namespace papaya::storage;

    log::info("TEST", "Running unit test: test_long_mode_pe_boot");

    auto hv = create_hypervisor(PlatformBackend::Kvm);
    assert(hv != nullptr);
    assert(hv->initialize().has_value());

    MemoryMap mem;
    assert(mem.map_region("TestRAM_512MB", 0x0, 512 * MiB, false).has_value());
    assert(hv->configure_memory(mem).has_value());

    void* ram = mem.get_host_pointer(0x0);
    assert(ram != nullptr);

    // 1. Initialize 64-bit page tables & TEB/PEB
    assert(kvm::KvmLongMode::initialize_page_tables(ram, 512 * MiB).has_value());
    assert(kvm::KvmLongMode::initialize_teb_peb(ram, 512 * MiB).has_value());

    // 2. Initialize HLE Thunk Manager
    HleThunkManager thunk_mgr(0x00100000);
    bool function_called = false;
    thunk_mgr.register_function("kernel32.dll", "AddNumbers", [&](HleCallContext& ctx) -> u64 {
        function_called = true;
        log::info("TEST", "HLE AddNumbers called: RCX={}, RDX={}", ctx.rcx, ctx.rdx);
        return ctx.rcx + ctx.rdx; // 50 + 70 = 120
    });

    thunk_mgr.write_trampolines_to_guest(ram, 512 * MiB);

    // 3. Load synthetic PE64 image
    auto pe_bytes = create_test_pe64_with_import();
    PeLoader loader;
    auto load_res = loader.load_image(pe_bytes, 0x00400000, ram, 512 * MiB);
    assert(load_res.has_value());

    // Bind import to IAT at 0x00402000
    std::vector<PeImportEntry> imports = {
        {
            .module_name = "kernel32.dll",
            .function_name = "AddNumbers",
            .ordinal = 0,
            .is_ordinal = false,
            .iat_gva = 0x00402000
        }
    };
    assert(thunk_mgr.bind_imports(imports, ram, 512 * MiB).has_value());

    // 4. Create vCPU and switch to 64-bit Long Mode
    auto vcpu_res = hv->create_vcpu(0);
    assert(vcpu_res.has_value());
    auto vcpu = *vcpu_res;

    constexpr GuestVirtAddr STACK_RSP = 0x00080000; // 512KB stack
    assert(vcpu->setup_long_mode(load_res->entry_point, STACK_RSP).has_value());

    // 5. Run Execution Loop
    bool teb_verified = false;
    u8 final_result = 0;
    bool halted = false;

    for (int step = 0; step < 50 && !halted; ++step) {
        auto exit_res = vcpu->run_once();
        assert(exit_res.has_value());

        const auto& exit = *exit_res;
        if (exit.reason == ExitReason::IoOut) {
            if (exit.address == HLE_HYPERCALL_IO_PORT) {
                // HLE Hypercall exit (port 0xEB)
                u32 thunk_id = static_cast<u32>(exit.data & 0xFFFFFFFF);
                auto thunk_res = thunk_mgr.execute_thunk(thunk_id, *vcpu, ram, 512 * MiB);
                assert(thunk_res.has_value());
            } else if (exit.address == 0xE0) {
                u8 val = static_cast<u8>(exit.data & 0xFF);
                if (val == 0xAA) {
                    teb_verified = true;
                    log::info("TEST", "Guest verified Windows TEB access via GS:[0x30]!");
                } else {
                    final_result = val;
                    log::info("TEST", "Guest output result: {}", (int)val);
                }
            }
        } else if (exit.reason == ExitReason::Halt) {
            log::info("TEST", "Guest executed HLT successfully");
            halted = true;
        } else {
            log::warn("TEST", "Unhandled exit: {}", static_cast<int>(exit.reason));
            break;
        }
    }

    assert(halted);
    assert(teb_verified);
    assert(function_called);
    assert(final_result == 120); // 50 + 70

    log::info("TEST", ">>> test_long_mode_pe_boot PASSED ALL CHECKS! (Result={}) <<<", (int)final_result);
    return 0;
}
