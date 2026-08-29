#include "papaya/storage/pe_loader.hpp"
#include "papaya/common/logger.hpp"
#include "papaya/common/memory_utils.hpp"
#include <cstring>

namespace papaya::storage {

Result<ImageNtHeaders64> PeLoader::parse_headers(std::span<const u8> file_bytes) {
    if (file_bytes.size() < sizeof(ImageDosHeader)) {
        return ErrorCode::InvalidParameter;
    }

    const auto* dos = reinterpret_cast<const ImageDosHeader*>(file_bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        log::error("PE", "Invalid DOS signature: 0x{:04X} (expected 0x{:04X})", dos->e_magic, IMAGE_DOS_SIGNATURE);
        return ErrorCode::InvalidParameter;
    }

    if (dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(ImageNtHeaders64) > file_bytes.size()) {
        log::error("PE", "e_lfanew offset 0x{:X} out of bounds", dos->e_lfanew);
        return ErrorCode::InvalidParameter;
    }

    const auto* nt = reinterpret_cast<const ImageNtHeaders64*>(file_bytes.data() + dos->e_lfanew);
    if (nt->signature != IMAGE_NT_SIGNATURE) {
        log::error("PE", "Invalid NT signature: 0x{:08X}", nt->signature);
        return ErrorCode::InvalidParameter;
    }

    if (nt->file_header.machine != IMAGE_FILE_MACHINE_AMD64) {
        log::error("PE", "Unsupported machine type: 0x{:04X} (expected x86-64 AMD64)", nt->file_header.machine);
        return ErrorCode::InvalidParameter;
    }

    if (nt->optional_header.magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        log::error("PE", "Unsupported optional header magic: 0x{:04X}", nt->optional_header.magic);
        return ErrorCode::InvalidParameter;
    }

    return *nt;
}

Result<LoadedPeImage> PeLoader::load_image(
    std::span<const u8> file_bytes,
    GuestVirtAddr target_base,
    void* guest_memory_host_base,
    u64 guest_memory_size
) {
    auto nt_res = parse_headers(file_bytes);
    if (!nt_res) {
        return nt_res.error();
    }
    const auto& nt = *nt_res;

    u64 size_of_image = nt.optional_header.size_of_image;
    u64 preferred_base = nt.optional_header.image_base;
    GuestVirtAddr actual_base = (target_base != 0) ? target_base : preferred_base;

    log::info("PE", "Loading PE64 Image: preferred_base=0x{:X}, actual_base=0x{:X}, size=0x{:X} ({} KB)",
              preferred_base, actual_base, size_of_image, size_of_image / 1024);

    if (actual_base + size_of_image > guest_memory_size) {
        log::error("PE", "Image does not fit in guest memory (0x{:X} > 0x{:X})", actual_base + size_of_image, guest_memory_size);
        return ErrorCode::OutOfMemory;
    }

    auto* host_dest = static_cast<u8*>(guest_memory_host_base) + actual_base;

    // 1. Copy Headers
    u32 size_of_headers = nt.optional_header.size_of_headers;
    if (size_of_headers > file_bytes.size()) {
        size_of_headers = static_cast<u32>(file_bytes.size());
    }
    std::memcpy(host_dest, file_bytes.data(), size_of_headers);

    // 2. Copy Sections
    const auto* dos = reinterpret_cast<const ImageDosHeader*>(file_bytes.data());
    const auto* first_section = reinterpret_cast<const ImageSectionHeader*>(
        file_bytes.data() + dos->e_lfanew + sizeof(u32) + sizeof(ImageFileHeader) + nt.file_header.size_of_optional_header
    );

    for (u16 i = 0; i < nt.file_header.number_of_sections; ++i) {
        const auto& sec = first_section[i];
        char name[9] = {0};
        std::memcpy(name, sec.name, 8);

        u32 copy_size = std::min(sec.size_of_raw_data, sec.misc.virtual_size);
        if (sec.pointer_to_raw_data + copy_size > file_bytes.size()) {
            copy_size = static_cast<u32>(file_bytes.size() - sec.pointer_to_raw_data);
        }

        u8* sec_dest = host_dest + sec.virtual_address;
        if (copy_size > 0 && sec.pointer_to_raw_data > 0) {
            std::memcpy(sec_dest, file_bytes.data() + sec.pointer_to_raw_data, copy_size);
        }

        // Zero out uninitialized tail (e.g. .bss)
        if (sec.misc.virtual_size > copy_size) {
            std::memset(sec_dest + copy_size, 0, sec.misc.virtual_size - copy_size);
        }

        log::debug("PE", "Mapped section {:<8} -> RVA 0x{:08X} (size 0x{:X})",
                   name, sec.virtual_address, sec.misc.virtual_size);
    }

    // 3. Apply Base Relocations if relocated
    s64 delta = static_cast<s64>(actual_base) - static_cast<s64>(preferred_base);
    if (delta != 0) {
        auto reloc_res = apply_relocations(file_bytes, nt, host_dest, delta);
        if (!reloc_res) {
            log::error("PE", "Failed to apply base relocations");
            return reloc_res.error();
        }
    }

    // 4. Parse Imports
    auto imports_res = parse_imports(file_bytes, nt, actual_base);
    if (!imports_res) {
        log::error("PE", "Failed to parse import directory");
        return imports_res.error();
    }

    LoadedPeImage image{
        .preferred_base = preferred_base,
        .loaded_base = actual_base,
        .entry_point = actual_base + nt.optional_header.address_of_entry_point,
        .image_size = size_of_image,
        .imports = std::move(*imports_res)
    };

    log::info("PE", "PE64 Image successfully loaded! Entry Point: 0x{:X}, Imports: {}",
              image.entry_point, image.imports.size());

    return image;
}

Result<> PeLoader::apply_relocations(
    std::span<const u8> file_bytes,
    const ImageNtHeaders64& nt_headers,
    u8* image_dest,
    s64 delta
) {
    const auto& reloc_dir = nt_headers.optional_header.data_directory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (reloc_dir.virtual_address == 0 || reloc_dir.size == 0) {
        log::warn("PE", "No base relocation table found, but image was relocated");
        return {};
    }

    u32 offset = 0;
    while (offset < reloc_dir.size) {
        const auto* block = reinterpret_cast<const ImageBaseRelocation*>(image_dest + reloc_dir.virtual_address + offset);
        if (block->size_of_block == 0) {
            break;
        }

        u32 entries_count = (block->size_of_block - sizeof(ImageBaseRelocation)) / sizeof(u16);
        const auto* entries = reinterpret_cast<const u16*>(block + 1);

        for (u32 i = 0; i < entries_count; ++i) {
            u16 entry = entries[i];
            u8 type = entry >> 12;
            u16 rva_offset = entry & 0x0FFF;

            if (type == 10) { // IMAGE_REL_BASED_DIR64 (x86-64 64-bit address fixup)
                u64* target_addr = reinterpret_cast<u64*>(image_dest + block->virtual_address + rva_offset);
                *target_addr += delta;
            } else if (type != 0) { // 0 is IMAGE_REL_BASED_ABSOLUTE (padding)
                log::trace("PE", "Relocation type {} at RVA 0x{:X}", type, block->virtual_address + rva_offset);
            }
        }

        offset += block->size_of_block;
    }

    log::info("PE", "Applied base relocations with delta 0x{:X}", delta);
    return {};
}

Result<std::vector<PeImportEntry>> PeLoader::parse_imports(
    std::span<const u8> file_bytes,
    const ImageNtHeaders64& nt_headers,
    GuestVirtAddr loaded_base
) {
    std::vector<PeImportEntry> imports;
    const auto& import_dir = nt_headers.optional_header.data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir.virtual_address == 0 || import_dir.size == 0) {
        return imports;
    }

    const auto* dos = reinterpret_cast<const ImageDosHeader*>(file_bytes.data());
    const auto* sections = reinterpret_cast<const ImageSectionHeader*>(
        file_bytes.data() + dos->e_lfanew + sizeof(u32) + sizeof(ImageFileHeader) + nt_headers.file_header.size_of_optional_header
    );

    auto rva_to_ptr = [&](u32 rva) -> const u8* {
        for (u16 s = 0; s < nt_headers.file_header.number_of_sections; ++s) {
            const auto& sec = sections[s];
            if (rva >= sec.virtual_address && rva < sec.virtual_address + sec.misc.virtual_size) {
                u32 offset_in_sec = rva - sec.virtual_address;
                if (sec.pointer_to_raw_data + offset_in_sec < file_bytes.size()) {
                    return file_bytes.data() + sec.pointer_to_raw_data + offset_in_sec;
                }
            }
        }
        return nullptr;
    };

    const u8* desc_ptr = rva_to_ptr(import_dir.virtual_address);
    if (!desc_ptr) {
        return imports;
    }

    const auto* cur_desc = reinterpret_cast<const ImageImportDescriptor*>(desc_ptr);
    while (cur_desc->name != 0 && cur_desc->first_thunk != 0) {
        const char* module_name = reinterpret_cast<const char*>(rva_to_ptr(cur_desc->name));
        std::string mod_str = module_name ? module_name : "UNKNOWN.DLL";

        u32 thunk_rva = cur_desc->original_first_thunk ? cur_desc->original_first_thunk : cur_desc->first_thunk;
        u32 iat_rva = cur_desc->first_thunk;

        const u8* thunk_ptr = rva_to_ptr(thunk_rva);
        if (thunk_ptr) {
            const auto* thunks = reinterpret_cast<const u64*>(thunk_ptr);
            size_t idx = 0;
            while (thunks[idx] != 0) {
                u64 val = thunks[idx];
                PeImportEntry entry;
                entry.module_name = mod_str;
                entry.iat_gva = loaded_base + iat_rva + (idx * sizeof(u64));

                if (val & 0x8000000000000000ULL) {
                    // Import by Ordinal
                    entry.is_ordinal = true;
                    entry.ordinal = static_cast<u16>(val & 0xFFFF);
                    entry.function_name = std::to_string(entry.ordinal);
                } else {
                    // Import by Name
                    u32 name_rva = static_cast<u32>(val & 0xFFFFFFFF);
                    const u8* hint_ptr = rva_to_ptr(name_rva);
                    if (hint_ptr) {
                        const char* fn_name = reinterpret_cast<const char*>(hint_ptr + 2); // Skip 2-byte Hint
                        entry.function_name = fn_name;
                    }
                }

                imports.push_back(std::move(entry));
                idx++;
            }
        }

        cur_desc++;
    }

    return imports;
}

} // namespace papaya::storage
