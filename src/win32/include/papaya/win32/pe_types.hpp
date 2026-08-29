#pragma once

#include "papaya/common/types.hpp"
#include <array>

namespace papaya::win32 {

constexpr u16 IMAGE_DOS_SIGNATURE = 0x5A4D;     // "MZ"
constexpr u32 IMAGE_NT_SIGNATURE  = 0x00004550; // "PE\0\0"

constexpr u16 IMAGE_FILE_MACHINE_AMD64 = 0x8664;
constexpr u16 IMAGE_FILE_MACHINE_ARM64 = 0xAA64;
constexpr u16 IMAGE_FILE_MACHINE_I386  = 0x014c;
constexpr u16 IMAGE_FILE_MACHINE_ARMNT = 0x01c4; // ARM Thumb-2 (32-bit)

constexpr u16 IMAGE_NT_OPTIONAL_HDR64_MAGIC = 0x020B;
constexpr u16 IMAGE_NT_OPTIONAL_HDR32_MAGIC = 0x010B;

constexpr u32 IMAGE_DIRECTORY_ENTRY_EXPORT        = 0;
constexpr u32 IMAGE_DIRECTORY_ENTRY_IMPORT        = 1;
constexpr u32 IMAGE_DIRECTORY_ENTRY_RESOURCE      = 2;
constexpr u32 IMAGE_DIRECTORY_ENTRY_EXCEPTION     = 3;
constexpr u32 IMAGE_DIRECTORY_ENTRY_SECURITY      = 4;
constexpr u32 IMAGE_DIRECTORY_ENTRY_BASERELOC     = 5;
constexpr u32 IMAGE_DIRECTORY_ENTRY_DEBUG         = 6;
constexpr u32 IMAGE_DIRECTORY_ENTRY_ARCHITECTURE  = 7;
constexpr u32 IMAGE_DIRECTORY_ENTRY_GLOBALPTR     = 8;
constexpr u32 IMAGE_DIRECTORY_ENTRY_TLS           = 9;
constexpr u32 IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG   = 10;
constexpr u32 IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT  = 11;
constexpr u32 IMAGE_DIRECTORY_ENTRY_IAT           = 12;
constexpr u32 IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT  = 13;
constexpr u32 IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR= 14;

constexpr u32 IMAGE_SCN_MEM_EXECUTE = 0x20000000;
constexpr u32 IMAGE_SCN_MEM_READ    = 0x40000000;
constexpr u32 IMAGE_SCN_MEM_WRITE   = 0x80000000;
constexpr u32 IMAGE_SCN_CNT_CODE    = 0x00000020;
constexpr u32 IMAGE_SCN_CNT_INITIALIZED_DATA   = 0x00000040;
constexpr u32 IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;

constexpr u16 IMAGE_REL_BASED_ABSOLUTE = 0;
constexpr u16 IMAGE_REL_BASED_HIGH     = 1;
constexpr u16 IMAGE_REL_BASED_LOW      = 2;
constexpr u16 IMAGE_REL_BASED_HIGHLOW  = 3;
constexpr u16 IMAGE_REL_BASED_DIR64    = 10;

// PAGE_* protection constants (Win32) for per-section mprotect
constexpr u32 PAGE_NOACCESS          = 0x01;
constexpr u32 PAGE_READONLY          = 0x02;
constexpr u32 PAGE_READWRITE         = 0x04;
constexpr u32 PAGE_WRITECOPY         = 0x08;
constexpr u32 PAGE_EXECUTE           = 0x10;
constexpr u32 PAGE_EXECUTE_READ      = 0x20;
constexpr u32 PAGE_EXECUTE_READWRITE = 0x40;
constexpr u32 PAGE_EXECUTE_WRITECOPY = 0x80;

#pragma pack(push, 2)
struct ImageDosHeader {
    u16 e_magic;    // Magic number ("MZ")
    u16 e_cblp;     // Bytes on last page of file
    u16 e_cp;       // Pages in file
    u16 e_crlc;     // Relocations
    u16 e_cparhdr;  // Size of header in paragraphs
    u16 e_minalloc; // Minimum extra paragraphs needed
    u16 e_maxalloc; // Maximum extra paragraphs needed
    u16 e_ss;       // Initial (relative) SS value
    u16 e_sp;       // Initial SP value
    u16 e_csum;     // Checksum
    u16 e_ip;       // Initial IP value
    u16 e_cs;       // Initial (relative) CS value
    u16 e_lfarlc;   // File address of relocation table
    u16 e_ovno;     // Overlay number
    u16 e_res[4];   // Reserved words
    u16 e_oemid;    // OEM identifier (for e_oeminfo)
    u16 e_oeminfo;  // OEM information; e_oemid specific
    u16 e_res2[10]; // Reserved words
    s32 e_lfanew;   // File address of new exe header
};
#pragma pack(pop)

#pragma pack(push, 4)
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

// PE32+ (64-bit) optional header
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

// PE32 (32-bit) optional header
struct ImageOptionalHeader32 {
    u16 magic;
    u8  major_linker_version;
    u8  minor_linker_version;
    u32 size_of_code;
    u32 size_of_initialized_data;
    u32 size_of_uninitialized_data;
    u32 address_of_entry_point;
    u32 base_of_code;
    u32 base_of_data;          // PE32 only — not present in PE32+
    u32 image_base;            // 32-bit base!
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
    u32 size_of_stack_reserve;   // 32-bit sizes!
    u32 size_of_stack_commit;
    u32 size_of_heap_reserve;
    u32 size_of_heap_commit;
    u32 loader_flags;
    u32 number_of_rva_and_sizes;
    ImageDataDirectory data_directory[16];
};

struct ImageNtHeaders64 {
    u32 signature;
    ImageFileHeader file_header;
    ImageOptionalHeader64 optional_header;
};

struct ImageNtHeaders32 {
    u32 signature;
    ImageFileHeader file_header;
    ImageOptionalHeader32 optional_header;
};

// Normalized view of either PE32 or PE32+ NT headers.
struct ImageNtHeadersUnified {
    u16 machine{0};
    bool is_64bit{true};
    u32 size_of_image{0};
    u32 size_of_headers{0};
    u32 address_of_entry_point{0};
    u64 image_base{0};
    u32 section_alignment{0};
    u32 file_alignment{0};
    ImageDataDirectory data_directory[16]{};
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

struct WinPeb64 {
    u8    inherited_address_space;
    u8    read_image_file_exec_options;
    u8    being_debugged;
    u8    spare_bool;
    u32   pad0;
    void* mutant;
    void* image_base_address;
    void* ldr;
    void* process_parameters;
    void* sub_system_data;
    void* process_heap;
    void* fast_peb_lock;
    void* atl_thunk_slist_ptr;
    void* ifeo_key;
    u32   cross_process_flags;
    u32   pad1;
    void* user_shared_info_ptr;
    u32   system_reserved;
    u32   atl_thunk_slist_ptr32;
    void* api_set_map;
    u32   tls_expansion_counter;
    u32   pad2;
    void* tls_bitmap;
    u32   tls_bitmap_bits[2];
    void* read_only_shared_memory_base;
    void* shared_data;
    void* read_only_static_server_data;
    void* ansi_code_page_data;
    void* oem_code_page_data;
    void* unicode_case_table_data;
    u32   number_of_processors;
    u32   nt_global_flag;
};

struct WinTeb64 {
    void* exception_list;           // 0x00
    void* stack_base;               // 0x08
    void* stack_limit;              // 0x10
    void* sub_system_tib;           // 0x18
    void* fiber_data;               // 0x20
    void* arbitrary_user_pointer;   // 0x28
    WinTeb64* self;                 // 0x30 (%gs:0x30 points to TEB itself)
    void* environment_pointer;      // 0x38
    u64   client_id_proc;           // 0x40
    u64   client_id_thread;         // 0x48
    void* rpc_handle;               // 0x50
    void* tls_slots[64];            // 0x58 (%gs:0x58.. TLS array)
    WinPeb64* peb;                  // 0x60 (%gs:0x60 points to PEB)
    u32   last_error_value;         // 0x68
    u32   count_of_owned_critical_sections; // 0x6C
    void* csr_client_thread;        // 0x70
    void* win32_thread_info;        // 0x78
    u32   user32_reserved[26];      // 0x80
    u32   user_reserved[5];         // 0xE8
    void* wow32_reserved;           // 0x100
    u32   current_locale;           // 0x108
    u32   fp_software_status_register; // 0x10C
    void* reserved_for_debugger[54];// 0x110
    s32   exception_code;           // 0x2C0
    u32   pad;
    void* activation_context_stack_pointer; // 0x2C8
};
#pragma pack(pop)

} // namespace papaya::win32
