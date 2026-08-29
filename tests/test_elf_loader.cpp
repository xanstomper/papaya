#include "papaya/common/logger.hpp"
#include "papaya/storage/elf_loader.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

std::vector<papaya::u8> create_test_ps4_elf() {
    using namespace papaya;
    using namespace papaya::storage;

    std::vector<u8> buffer(4096, 0);

    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(buffer.data());
    std::memcpy(ehdr->e_ident, "\x7f\x45\x4c\x46\x02\x01\x01\x09", 8); // ELF64, Little-Endian, FreeBSD ABI (9)
    ehdr->e_type = ET_SCE_EXEC;
    ehdr->e_machine = EM_X86_64;
    ehdr->e_version = 1;
    ehdr->e_entry = 0x1000;
    ehdr->e_phoff = sizeof(Elf64_Ehdr);
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 2;

    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(buffer.data() + sizeof(Elf64_Ehdr));

    // Segment 0: PT_LOAD (.text)
    phdrs[0].p_type = PT_LOAD;
    phdrs[0].p_flags = PF_R | PF_X;
    phdrs[0].p_offset = 0x200;
    phdrs[0].p_vaddr = 0x1000;
    phdrs[0].p_paddr = 0x1000;
    phdrs[0].p_filesz = 0x100;
    phdrs[0].p_memsz = 0x100;
    phdrs[0].p_align = 0x1000;

    // Segment 1: PT_LOAD (.data / .bss)
    phdrs[1].p_type = PT_LOAD;
    phdrs[1].p_flags = PF_R | PF_W;
    phdrs[1].p_offset = 0x300;
    phdrs[1].p_vaddr = 0x2000;
    phdrs[1].p_paddr = 0x2000;
    phdrs[1].p_filesz = 0x50;
    phdrs[1].p_memsz = 0x200; // 0x1B0 bytes of .bss
    phdrs[1].p_align = 0x1000;

    // Put some code into .text: NOPs and RET
    buffer[0x200] = 0x90; // NOP
    buffer[0x201] = 0xC3; // RET

    // Put data into .data
    std::memcpy(buffer.data() + 0x300, "PapayaPS4", 9);

    return buffer;
}

std::vector<papaya::u8> create_test_ps5_elf() {
    auto buf = create_test_ps4_elf();
    // Add a Prospero note section
    auto* ehdr = reinterpret_cast<papaya::storage::Elf64_Ehdr*>(buf.data());
    ehdr->e_phnum = 3;

    auto* phdrs = reinterpret_cast<papaya::storage::Elf64_Phdr*>(buf.data() + sizeof(papaya::storage::Elf64_Ehdr));
    phdrs[2].p_type = papaya::storage::PT_NOTE;
    phdrs[2].p_flags = papaya::storage::PF_R;
    phdrs[2].p_offset = 0x500;
    phdrs[2].p_vaddr = 0x3000;
    phdrs[2].p_filesz = 64;
    phdrs[2].p_memsz = 64;

    std::memcpy(buf.data() + 0x500, "Prospero SDK v1.00", 18);
    return buf;
}

int main() {
    using namespace papaya;
    using namespace papaya::storage;

    log::info("TEST", "Running unit test: test_elf_loader");

    ElfLoader loader;
    std::vector<u8> ram(32 * MiB, 0);

    // 1. Test PS4 ELF Loader
    auto ps4_elf = create_test_ps4_elf();
    auto ps4_target = ElfLoader::detect_target(ps4_elf);
    assert(ps4_target.has_value());
    assert(*ps4_target == ConsoleTarget::PlayStation4);

    auto ps4_res = loader.load_image(ps4_elf, 0x00400000, ram.data(), ram.size());
    assert(ps4_res.has_value());
    assert(ps4_res->entry_point == 0x00401000);
    assert(ps4_res->segments.size() == 2);
    assert(!ps4_res->is_ps5);

    // Verify .data and .text in mapped RAM
    assert(ram[0x00401000] == 0x90);
    assert(std::memcmp(ram.data() + 0x00402000, "PapayaPS4", 9) == 0);
    // Verify .bss zeroed
    assert(ram[0x00402050] == 0x00);
    log::info("TEST", "PS4 ELF loaded, segments verified, .bss zero-filled!");

    // 2. Test PS5 ELF Loader
    auto ps5_elf = create_test_ps5_elf();
    auto ps5_target = ElfLoader::detect_target(ps5_elf);
    assert(ps5_target.has_value());
    assert(*ps5_target == ConsoleTarget::PlayStation5);

    auto ps5_res = loader.load_image(ps5_elf, 0x00400000, ram.data(), ram.size());
    assert(ps5_res.has_value());
    assert(ps5_res->is_ps5);
    log::info("TEST", "PS5 Prospero ELF auto-detected and loaded successfully!");

    log::info("TEST", ">>> test_elf_loader PASSED ALL CHECKS! <<<");
    return 0;
}
