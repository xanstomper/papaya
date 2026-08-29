#include "papaya/win32/pe_loader.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace papaya::win32 {

PeLoader::PeLoader(std::shared_ptr<Win32ApiHle> hle)
    : hle_(hle ? hle : std::make_shared<Win32ApiHle>()) {
    hle_->initialize();
}

PeLoader::~PeLoader() = default;

Result<LoadedPeImage> PeLoader::load_from_file(const std::filesystem::path& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        log::error("PE_LOADER", "Failed to open PE file: '{}'", file_path.string());
        return ErrorCode::FileNotFound;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<u8> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return ErrorCode::RomSectorReadFailed;
    }

    return load_from_memory(buffer);
}

Result<LoadedPeImage> PeLoader::load_from_memory(std::span<const u8> file_data) {
    if (file_data.size() < sizeof(ImageDosHeader)) {
        log::error("PE_LOADER", "File buffer smaller than DOS Header");
        return ErrorCode::RomCorruptHeader;
    }

    const auto* dos_hdr = reinterpret_cast<const ImageDosHeader*>(file_data.data());
    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        log::error("PE_LOADER", "Invalid DOS Magic 0x{:X} (expected 'MZ' 0x5A4D)", dos_hdr->e_magic);
        return ErrorCode::RomInvalidMagic;
    }

    if (dos_hdr->e_lfanew <= 0 || static_cast<size_t>(dos_hdr->e_lfanew) + sizeof(ImageNtHeaders64) > file_data.size()) {
        log::error("PE_LOADER", "Invalid NT Header offset (e_lfanew: {})", dos_hdr->e_lfanew);
        return ErrorCode::RomCorruptHeader;
    }

    const auto* nt_hdr = reinterpret_cast<const ImageNtHeaders64*>(file_data.data() + dos_hdr->e_lfanew);
    if (nt_hdr->signature != IMAGE_NT_SIGNATURE) {
        log::error("PE_LOADER", "Invalid NT Signature 0x{:X} (expected 'PE\\0\\0')", nt_hdr->signature);
        return ErrorCode::RomInvalidMagic;
    }

    u64 size_of_image = nt_hdr->optional_header.size_of_image;
    u64 original_base = nt_hdr->optional_header.image_base;

    // Allocate virtual memory for the entire image
    void* allocated_base = mmap(reinterpret_cast<void*>(original_base), size_of_image,
                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (allocated_base == MAP_FAILED) {
        // Fallback without hint address
        allocated_base = mmap(nullptr, size_of_image,
                              PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocated_base == MAP_FAILED) {
            log::error("PE_LOADER", "Failed to allocate {} bytes for PE image", size_of_image);
            return ErrorCode::MemoryMappingFailed;
        }
    }

    // Zero allocated memory
    std::memset(allocated_base, 0, size_of_image);

    // Copy PE headers to base
    u32 size_of_headers = nt_hdr->optional_header.size_of_headers;
    std::memcpy(allocated_base, file_data.data(), std::min(static_cast<size_t>(size_of_headers), file_data.size()));

    // Map each PE Section (.text, .rdata, .data, .reloc)
    auto map_res = map_sections(file_data.data(), nt_hdr, allocated_base);
    if (!map_res) {
        munmap(allocated_base, size_of_image);
        return map_res.error();
    }

    // Apply base relocations if loaded at different base
    if (reinterpret_cast<u64>(allocated_base) != original_base) {
        apply_relocations(nt_hdr, allocated_base, original_base);
    }

    // Resolve IAT imports
    resolve_imports(file_data.data(), nt_hdr, allocated_base);

    LoadedPeImage image{};
    image.image_base = allocated_base;
    image.size_of_image = size_of_image;
    image.entry_point = reinterpret_cast<void*>(reinterpret_cast<u8*>(allocated_base) + nt_hdr->optional_header.address_of_entry_point);
    image.machine = nt_hdr->file_header.machine;
    image.is_64bit = (nt_hdr->optional_header.magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

    log::info("PE_LOADER", "Loaded PE Image [Base: 0x{:X}, Entry: 0x{:X}, Size: {} KB, Sections: {}]",
              reinterpret_cast<u64>(image.image_base),
              reinterpret_cast<u64>(image.entry_point),
              image.size_of_image / 1024,
              nt_hdr->file_header.number_of_sections);

    return image;
}

Result<> PeLoader::map_sections(const u8* file_raw, const ImageNtHeaders64* nt_hdr, void* allocated_base) {
    const auto* section_hdrs = reinterpret_cast<const ImageSectionHeader*>(
        reinterpret_cast<const u8*>(&nt_hdr->optional_header) + nt_hdr->file_header.size_of_optional_header
    );

    u8* base_bytes = reinterpret_cast<u8*>(allocated_base);

    for (u16 i = 0; i < nt_hdr->file_header.number_of_sections; ++i) {
        const auto& sec = section_hdrs[i];
        char sec_name[9] = {0};
        std::memcpy(sec_name, sec.name, 8);

        u32 vaddr = sec.virtual_address;
        u32 vsize = sec.misc.virtual_size;
        u32 raw_ptr = sec.pointer_to_raw_data;
        u32 raw_size = sec.size_of_raw_data;

        if (raw_ptr > 0 && raw_size > 0) {
            std::memcpy(base_bytes + vaddr, file_raw + raw_ptr, std::min(vsize > 0 ? vsize : raw_size, raw_size));
        }

        log::debug("PE_LOADER", "Mapped Section '{}' -> RVA 0x{:X} (Size: {} bytes)", sec_name, vaddr, vsize);
    }

    return {};
}

Result<> PeLoader::apply_relocations(const ImageNtHeaders64* nt_hdr, void* allocated_base, u64 original_image_base) {
    const auto& reloc_dir = nt_hdr->optional_header.data_directory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (reloc_dir.virtual_address == 0 || reloc_dir.size == 0) return {};

    u64 delta = reinterpret_cast<u64>(allocated_base) - original_image_base;
    u8* base_bytes = reinterpret_cast<u8*>(allocated_base);
    u8* reloc_ptr = base_bytes + reloc_dir.virtual_address;
    u8* reloc_end = reloc_ptr + reloc_dir.size;

    u64 total_relocs = 0;
    while (reloc_ptr < reloc_end) {
        const auto* block = reinterpret_cast<const ImageBaseRelocation*>(reloc_ptr);
        if (block->size_of_block == 0) break;

        u32 count = (block->size_of_block - sizeof(ImageBaseRelocation)) / sizeof(u16);
        const u16* entries = reinterpret_cast<const u16*>(reloc_ptr + sizeof(ImageBaseRelocation));

        for (u32 i = 0; i < count; ++i) {
            u16 type = entries[i] >> 12;
            u16 offset = entries[i] & 0xFFF;

            if (type == IMAGE_REL_BASED_DIR64) {
                u64* target = reinterpret_cast<u64*>(base_bytes + block->virtual_address + offset);
                *target += delta;
                total_relocs++;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                u32* target = reinterpret_cast<u32*>(base_bytes + block->virtual_address + offset);
                *target += static_cast<u32>(delta);
                total_relocs++;
            }
        }

        reloc_ptr += block->size_of_block;
    }

    log::info("PE_LOADER", "Applied {} Base Relocations (Base Delta: 0x{:X})", total_relocs, delta);
    return {};
}

Result<> PeLoader::resolve_imports(const u8* file_raw, const ImageNtHeaders64* nt_hdr, void* allocated_base) {
    const auto& import_dir = nt_hdr->optional_header.data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir.virtual_address == 0 || import_dir.size == 0) return {};

    u8* base_bytes = reinterpret_cast<u8*>(allocated_base);
    auto* import_desc = reinterpret_cast<ImageImportDescriptor*>(base_bytes + import_dir.virtual_address);

    while (import_desc->name != 0) {
        const char* dll_name = reinterpret_cast<const char*>(base_bytes + import_desc->name);
        u32 thunk_rva = import_desc->original_first_thunk ? import_desc->original_first_thunk : import_desc->first_thunk;
        
        u64* lookup_thunk = reinterpret_cast<u64*>(base_bytes + thunk_rva);
        u64* iat_thunk = reinterpret_cast<u64*>(base_bytes + import_desc->first_thunk);

        while (*lookup_thunk != 0) {
            if (*lookup_thunk & 0x8000000000000000ULL) {
                // Import by Ordinal
                u16 ordinal = static_cast<u16>(*lookup_thunk & 0xFFFF);
                void* fn_ptr = hle_->resolve_symbol(dll_name, std::to_string(ordinal));
                *iat_thunk = reinterpret_cast<u64>(fn_ptr);
            } else {
                // Import by Name
                u32 name_rva = static_cast<u32>(*lookup_thunk & 0xFFFFFFFF);
                const char* func_name = reinterpret_cast<const char*>(base_bytes + name_rva + 2); // +2 skip hint
                void* fn_ptr = hle_->resolve_symbol(dll_name, func_name);
                *iat_thunk = reinterpret_cast<u64>(fn_ptr);
            }
            lookup_thunk++;
            iat_thunk++;
        }
        import_desc++;
    }

    return {};
}

void PeLoader::unload_image(LoadedPeImage& image) {
    if (image.image_base && image.size_of_image > 0) {
        munmap(image.image_base, image.size_of_image);
        image.image_base = nullptr;
        image.size_of_image = 0;
        image.entry_point = nullptr;
    }
}

} // namespace papaya::win32
