#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <string>
#include <span>
#include <memory>

namespace papaya::storage {

// Standard PE / COFF constants
constexpr u16 IMAGE_DOS_SIGNATURE     = 0x5A4D;     // "MZ"
constexpr u32 IMAGE_NT_SIGNATURE      = 0x00004550; // "PE\0\0"
constexpr u16 IMAGE_FILE_MACHINE_AMD64 = 0x8664;    // x86-64
constexpr u16 IMAGE_NT_OPTIONAL_HDR64_MAGIC = 0x020B;

constexpr u32 IMAGE_DIRECTORY_ENTRY_EXPORT    = 0;
constexpr u32 IMAGE_DIRECTORY_ENTRY_IMPORT    = 1;
constexpr u32 IMAGE_DIRECTORY_ENTRY_RESOURCE  = 2;
constexpr u32 IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3;
constexpr u32 IMAGE_DIRECTORY_ENTRY_SECURITY  = 4;
constexpr u32 IMAGE_DIRECTORY_ENTRY_BASERELOC = 5;
constexpr u32 IMAGE_DIRECTORY_ENTRY_DEBUG     = 6;
constexpr u32 IMAGE_DIRECTORY_ENTRY_TLS       = 9;
constexpr u32 IMAGE_DIRECTORY_ENTRY_IAT       = 12;

#pragma pack(push, 1)

struct ImageDosHeader {
    u16 e_magic;    // "MZ"
    u16 e_cblp;
    u16 e_cp;
    u16 e_crlc;
    u16 e_cparhdr;
    u16 e_minalloc;
    u16 e_maxalloc;
    u16 e_ss;
    u16 e_sp;
    u16 e_csum;
    u16 e_ip;
    u16 e_cs;
    u16 e_lfarlc;
    u16 e_ovno;
    u16 e_res[4];
    u16 e_oemid;
    u16 e_oeminfo;
    u16 e_res2[10];
    s32 e_lfanew;   // Offset to NT Header
};

struct ImageDataDirectory {
    u32 virtual_address;
    u32 size;
};

struct ImageFileHeader {
    u16 machine;
    u16 number_of_sections;
    u32 time_date_stamp;
    u32 pointer_to_symbol_table;
    u32 number_of_symbols;
    u16 size_of_optional_header;
    u16 characteristics;
};

struct ImageOptionalHeader64 {
    u16 magic;
    u8  major_linker_version;
    u8  minor_linker_version;
    u32 size_of_code;
    u32 size_of_initialized_data;
    u32 size_of_uninitialized_data;
    u32 address_of_entry_point;
    u32 base_of_code;
    u64 image_base;
    u32 section_alignment;
    u32 file_alignment;
    u16 major_operating_system_version;
    u16 minor_operating_system_version;
    u16 major_image_version;
    u16 minor_image_version;
    u16 major_subsystem_version;
    u16 minor_subsystem_version;
    u32 win32_version_value;
    u32 size_of_image;
    u32 size_of_headers;
    u32 check_sum;
    u16 subsystem;
    u16 dll_characteristics;
    u64 size_of_stack_reserve;
    u64 size_of_stack_commit;
    u64 size_of_heap_reserve;
    u64 size_of_heap_commit;
    u32 loader_flags;
    u32 number_of_rva_and_sizes;
    ImageDataDirectory data_directory[16];
};

struct ImageNtHeaders64 {
    u32 signature;
    ImageFileHeader file_header;
    ImageOptionalHeader64 optional_header;
};

struct ImageSectionHeader {
    u8  name[8];
    union {
        u32 physical_address;
        u32 virtual_size;
    } misc;
    u32 virtual_address;
    u32 size_of_raw_data;
    u32 pointer_to_raw_data;
    u32 pointer_to_relocations;
    u32 pointer_to_linenumbers;
    u16 number_of_relocations;
    u16 number_of_linenumbers;
    u32 characteristics;
};

struct ImageImportDescriptor {
    union {
        u32 characteristics;
        u32 original_first_thunk; // RVA to INT (Import Name Table)
    };
    u32 time_date_stamp;
    u32 forwarder_chain;
    u32 name;                    // RVA to DLL name string
    u32 first_thunk;             // RVA to IAT (Import Address Table)
};

struct ImageBaseRelocation {
    u32 virtual_address;
    u32 size_of_block;
};

#pragma pack(pop)

struct PeImportEntry {
    std::string module_name;
    std::string function_name;
    u16 ordinal{0};
    bool is_ordinal{false};
    GuestVirtAddr iat_gva{0};
};

struct LoadedPeImage {
    GuestVirtAddr preferred_base{0};
    GuestVirtAddr loaded_base{0};
    GuestVirtAddr entry_point{0};
    u64 image_size{0};
    std::vector<PeImportEntry> imports;
};

class PeLoader {
public:
    PeLoader() = default;

    Result<LoadedPeImage> load_image(
        std::span<const u8> file_bytes,
        GuestVirtAddr target_base,
        void* guest_memory_host_base,
        u64 guest_memory_size
    );

    static Result<ImageNtHeaders64> parse_headers(std::span<const u8> file_bytes);

private:
    Result<> apply_relocations(
        std::span<const u8> file_bytes,
        const ImageNtHeaders64& nt_headers,
        u8* image_dest,
        s64 delta
    );

    Result<std::vector<PeImportEntry>> parse_imports(
        std::span<const u8> file_bytes,
        const ImageNtHeaders64& nt_headers,
        GuestVirtAddr loaded_base
    );
};

} // namespace papaya::storage
