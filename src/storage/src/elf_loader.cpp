#include "papaya/storage/elf_loader.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <algorithm>
#include <string_view>

namespace papaya::storage {

ElfLoader::ElfLoader() = default;
ElfLoader::~ElfLoader() = default;

Result<ConsoleTarget> ElfLoader::detect_target(std::span<const u8> file_data) {
    if (file_data.size() < sizeof(Elf64_Ehdr)) {
        return ErrorCode::ElfCorruptHeaders;
    }

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file_data.data());
    if (std::memcmp(ehdr->e_ident, "\x7f\x45\x4c\x46", 4) != 0) {
        return ErrorCode::ElfInvalidMagic;
    }

    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_machine != EM_X86_64) {
        return ErrorCode::ElfUnsupportedClass;
    }

    // Inspect program headers for PS5 (Prospero) markers
    if (ehdr->e_phoff + (ehdr->e_phnum * sizeof(Elf64_Phdr)) <= file_data.size()) {
        const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(file_data.data() + ehdr->e_phoff);
        for (u16 i = 0; i < ehdr->e_phnum; ++i) {
            // Check for PS5-specific program headers or Prospero SDK note sections
            if (phdrs[i].p_type == PT_NOTE && phdrs[i].p_filesz >= 16) {
                u64 off = phdrs[i].p_offset;
                if (off + phdrs[i].p_filesz <= file_data.size()) {
                    std::string_view note_view(reinterpret_cast<const char*>(file_data.data() + off), phdrs[i].p_filesz);
                    if (note_view.find("Prospero") != std::string_view::npos ||
                        note_view.find("PS5") != std::string_view::npos) {
                        return ConsoleTarget::PlayStation5;
                    }
                }
            }
        }
    }

    // Default to PS4 Orbis OS
    return ConsoleTarget::PlayStation4;
}

Result<LoadedElfImage> ElfLoader::load_image(
    std::span<const u8> file_data,
    GuestVirtAddr preferred_base,
    void* guest_memory_host_base,
    u64 guest_memory_size
) {
    if (file_data.size() < sizeof(Elf64_Ehdr)) {
        log::error("ELF", "Binary too small for ELF64 header (size={})", file_data.size());
        return ErrorCode::ElfCorruptHeaders;
    }

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file_data.data());
    if (std::memcmp(ehdr->e_ident, "\x7f\x45\x4c\x46", 4) != 0) {
        log::error("ELF", "Invalid ELF magic signature");
        return ErrorCode::ElfInvalidMagic;
    }

    if (ehdr->e_ident[4] != ELFCLASS64) {
        log::error("ELF", "Unsupported ELF class: expected 64-bit (ELFCLASS64)");
        return ErrorCode::ElfUnsupportedClass;
    }

    if (ehdr->e_machine != EM_X86_64) {
        log::error("ELF", "Unsupported architecture: expected AMD x86-64 (EM_X86_64)");
        return ErrorCode::ElfUnsupportedClass;
    }

    auto target_res = detect_target(file_data);
    ConsoleTarget target = target_res.has_value() ? *target_res : ConsoleTarget::PlayStation4;
    bool is_ps5 = (target == ConsoleTarget::PlayStation5);

    log::info("ELF", "Detected PlayStation Target: {} (OS ABI: {}, Type: 0x{:X})",
              is_ps5 ? "PlayStation 5 (Prospero OS)" : "PlayStation 4 (Orbis OS)",
              ehdr->e_ident[7], ehdr->e_type);

    LoadedElfImage loaded_image{};
    loaded_image.detected_target = target;
    loaded_image.is_ps5 = is_ps5;
    loaded_image.base_address = preferred_base;

    // Check program headers
    u64 phdr_table_size = static_cast<u64>(ehdr->e_phnum) * sizeof(Elf64_Phdr);
    if (ehdr->e_phoff + phdr_table_size > file_data.size()) {
        log::error("ELF", "Program header table exceeds file bounds");
        return ErrorCode::ElfCorruptHeaders;
    }

    const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(file_data.data() + ehdr->e_phoff);
    auto* host_ram = static_cast<u8*>(guest_memory_host_base);

    u64 max_vaddr = 0;
    const Elf64_Phdr* dynamic_phdr = nullptr;

    for (u16 i = 0; i < ehdr->e_phnum; ++i) {
        const auto& phdr = phdrs[i];

        if (phdr.p_type == PT_LOAD || phdr.p_type == PT_SCE_PROG || phdr.p_type == PT_SCE_RELRO) {
            if (phdr.p_memsz == 0) continue;

            GuestVirtAddr seg_vaddr = preferred_base + phdr.p_vaddr;
            u64 seg_end = seg_vaddr + phdr.p_memsz;
            if (seg_end > max_vaddr) max_vaddr = seg_end;

            if (seg_end > guest_memory_size) {
                log::error("ELF", "Segment [0x{:X}..0x{:X}] exceeds guest memory size (0x{:X})",
                           seg_vaddr, seg_end, guest_memory_size);
                return ErrorCode::MemoryMappingFailed;
            }

            // Copy file data
            if (phdr.p_filesz > 0) {
                if (phdr.p_offset + phdr.p_filesz > file_data.size()) {
                    log::error("ELF", "Segment data exceeds file boundary");
                    return ErrorCode::ElfCorruptHeaders;
                }
                std::memcpy(host_ram + seg_vaddr, file_data.data() + phdr.p_offset, phdr.p_filesz);
            }

            // Zero out .bss
            if (phdr.p_memsz > phdr.p_filesz) {
                std::memset(host_ram + seg_vaddr + phdr.p_filesz, 0, phdr.p_memsz - phdr.p_filesz);
            }

            loaded_image.segments.push_back({
                .type = phdr.p_type,
                .flags = phdr.p_flags,
                .virtual_address = seg_vaddr,
                .memory_size = phdr.p_memsz,
                .file_size = phdr.p_filesz
            });

            log::debug("ELF", "Mapped Segment {}: Type=0x{:X}, Flags=0x{:X}, GVA=0x{:X}, Size=0x{:X}",
                       i, phdr.p_type, phdr.p_flags, seg_vaddr, phdr.p_memsz);
        } else if (phdr.p_type == PT_DYNAMIC) {
            dynamic_phdr = &phdr;
        }
    }

    loaded_image.entry_point = preferred_base + ehdr->e_entry;
    loaded_image.total_image_size = max_vaddr > preferred_base ? (max_vaddr - preferred_base) : 0;

    if (dynamic_phdr) {
        parse_dynamic_section(file_data, *dynamic_phdr, loaded_image);
    }

    log::info("ELF", "Successfully loaded ELF64: Base=0x{:X}, Entry=0x{:X}, TotalSize=0x{:X}, Segments={}",
              loaded_image.base_address, loaded_image.entry_point, loaded_image.total_image_size, loaded_image.segments.size());

    return loaded_image;
}

Result<> ElfLoader::parse_dynamic_section(
    std::span<const u8> file_data,
    const Elf64_Phdr& dyn_phdr,
    LoadedElfImage& image
) {
    if (dyn_phdr.p_offset + dyn_phdr.p_filesz > file_data.size()) {
        return ErrorCode::ElfCorruptHeaders;
    }

    size_t dyn_count = dyn_phdr.p_filesz / sizeof(Elf64_Dyn);
    const auto* dyns = reinterpret_cast<const Elf64_Dyn*>(file_data.data() + dyn_phdr.p_offset);

    u64 strtab_off = 0;
    u64 strtab_sz = 0;
    std::vector<u64> needed_offsets;

    for (size_t i = 0; i < dyn_count; ++i) {
        if (dyns[i].d_tag == 0) break; // DT_NULL

        if (dyns[i].d_tag == 1) { // DT_NEEDED
            needed_offsets.push_back(dyns[i].d_un.d_val);
        } else if (dyns[i].d_tag == 5) { // DT_STRTAB
            strtab_off = dyns[i].d_un.d_ptr;
        } else if (dyns[i].d_tag == 10) { // DT_STRSZ
            strtab_sz = dyns[i].d_un.d_val;
        } else if (dyns[i].d_tag == 14) { // DT_SONAME
            // Module name
        }
    }

    // In a dumped ELF, strtab_off might be a virtual address relative to base
    if (strtab_off > 0 && strtab_off + strtab_sz <= file_data.size()) {
        const char* strtab = reinterpret_cast<const char*>(file_data.data() + strtab_off);
        for (u64 off : needed_offsets) {
            if (off < strtab_sz) {
                image.needed_libraries.emplace_back(strtab + off);
                log::debug("ELF", "DT_NEEDED: '{}'", image.needed_libraries.back());
            }
        }
    }

    return {};
}

} // namespace papaya::storage
