#include "papaya/common/logger.hpp"
#include "papaya/storage/pe_loader.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

// Helper to construct a synthetic valid PE32+ (x86-64) binary in memory
std::vector<papaya::u8> create_synthetic_pe64() {
    using namespace papaya;
    using namespace papaya::storage;

    std::vector<u8> buffer(4096, 0);

    // 1. DOS Header
    auto* dos = reinterpret_cast<ImageDosHeader*>(buffer.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; // "MZ"
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

    // Write a test x86-64 instruction in .text: mov eax, 0x1337; ret
    u8* code = buffer.data() + sec->pointer_to_raw_data;
    code[0] = 0xB8; code[1] = 0x37; code[2] = 0x13; code[3] = 0x00; code[4] = 0x00; // mov eax, 0x1337
    code[5] = 0xC3; // ret

    return buffer;
}

int main() {
    using namespace papaya;
    using namespace papaya::storage;

    log::info("TEST", "Running unit test: test_pe_loader");

    auto pe_bytes = create_synthetic_pe64();
    assert(!pe_bytes.empty());

    // Parse headers
    auto nt_res = PeLoader::parse_headers(pe_bytes);
    assert(nt_res.has_value());
    assert(nt_res->file_header.machine == IMAGE_FILE_MACHINE_AMD64);
    assert(nt_res->optional_header.image_base == 0x00400000);

    // Allocate simulated guest RAM (16MB)
    std::vector<u8> guest_ram(16 * 1024 * 1024, 0);

    PeLoader loader;
    auto load_res = loader.load_image(pe_bytes, 0x00400000, guest_ram.data(), guest_ram.size());
    assert(load_res.has_value());

    const auto& img = *load_res;
    assert(img.loaded_base == 0x00400000);
    assert(img.entry_point == 0x00401000);

    // Verify machine code was correctly mapped into guest RAM at entry point
    const u8* entry_code = guest_ram.data() + img.entry_point;
    assert(entry_code[0] == 0xB8);
    assert(entry_code[1] == 0x37);
    assert(entry_code[2] == 0x13);
    assert(entry_code[5] == 0xC3);

    log::info("TEST", "test_pe_loader passed successfully!");
    return 0;
}
