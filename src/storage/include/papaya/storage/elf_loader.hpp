#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <span>
#include <string>
#include <vector>

namespace papaya::storage {

// ELF Magic & Identifiers
constexpr u32 ELF_MAGIC = 0x464C457F; // "\x7fELF" in little-endian
constexpr u8  ELFCLASS64 = 2;
constexpr u8  ELFDATA2LSB = 1;
constexpr u16 ET_EXEC = 2;
constexpr u16 ET_DYN = 3;
constexpr u16 ET_SCE_EXEC = 0xFE00;
constexpr u16 ET_SCE_RELEXEC = 0xFE04;
constexpr u16 ET_SCE_DYNAMIC = 0xFE18;
constexpr u16 EM_X86_64 = 62; // AMD x86-64

// ELF OS ABI
constexpr u8 ELFOSABI_NONE = 0;
constexpr u8 ELFOSABI_FREEBSD = 9;

// Program Header Types
constexpr u32 PT_NULL = 0;
constexpr u32 PT_LOAD = 1;
constexpr u32 PT_DYNAMIC = 2;
constexpr u32 PT_INTERP = 3;
constexpr u32 PT_NOTE = 4;
constexpr u32 PT_SHLIB = 5;
constexpr u32 PT_PHDR = 6;
constexpr u32 PT_TLS = 7;
constexpr u32 PT_GNU_EH_FRAME = 0x6474E550;
constexpr u32 PT_GNU_STACK = 0x6474E551;
constexpr u32 PT_GNU_RELRO = 0x6474E552;

// Sony Proprietary Program Header Types
constexpr u32 PT_SCE_RELA = 0x60000000;
constexpr u32 PT_SCE_DYNLIBDATA = 0x61000000;
constexpr u32 PT_SCE_PROG = 0x61000001;
constexpr u32 PT_SCE_RELRO = 0x61000010;
constexpr u32 PT_SCE_COMMENT = 0x6FFFFF00;
constexpr u32 PT_SCE_VERSION = 0x6FFFFF01;

// Segment Flags
constexpr u32 PF_X = 0x1;
constexpr u32 PF_W = 0x2;
constexpr u32 PF_R = 0x4;

#pragma pack(push, 1)
struct Elf64_Ehdr {
    u8  e_ident[16];    // Magic number and other info
    u16 e_type;         // Object file type
    u16 e_machine;      // Architecture
    u32 e_version;      // Object file version
    u64 e_entry;        // Entry point virtual address
    u64 e_phoff;        // Program header table file offset
    u64 e_shoff;        // Section header table file offset
    u32 e_flags;        // Processor-specific flags
    u16 e_ehsize;       // ELF header size in bytes
    u16 e_phentsize;    // Program header table entry size
    u16 e_phnum;        // Program header table entry count
    u16 e_shentsize;    // Section header table entry size
    u16 e_shnum;        // Section header table entry count
    u16 e_shstrndx;     // Section header string table index
};

struct Elf64_Phdr {
    u32 p_type;         // Segment type
    u32 p_flags;        // Segment flags
    u64 p_offset;       // Segment file offset
    u64 p_vaddr;        // Segment virtual address
    u64 p_paddr;        // Segment physical address
    u64 p_filesz;       // Segment size in file
    u64 p_memsz;        // Segment size in memory
    u64 p_align;        // Segment alignment
};

struct Elf64_Dyn {
    s64 d_tag;          // Dynamic entry type
    union {
        u64 d_val;      // Integer value
        u64 d_ptr;      // Address value
    } d_un;
};
#pragma pack(pop)

struct LoadedSegment {
    u32 type{0};
    u32 flags{0};
    GuestVirtAddr virtual_address{0};
    u64 memory_size{0};
    u64 file_size{0};
};

struct LoadedElfImage {
    ConsoleTarget detected_target{ConsoleTarget::PlayStation4};
    GuestVirtAddr entry_point{0};
    GuestVirtAddr base_address{0};
    u64 total_image_size{0};
    std::vector<LoadedSegment> segments;
    std::vector<std::string> needed_libraries;
    std::string module_name;
    bool is_ps5{false};
};

class ElfLoader {
public:
    ElfLoader();
    ~ElfLoader();

    Result<LoadedElfImage> load_image(
        std::span<const u8> file_data,
        GuestVirtAddr preferred_base,
        void* guest_memory_host_base,
        u64 guest_memory_size
    );

    static Result<ConsoleTarget> detect_target(std::span<const u8> file_data);

private:
    Result<> parse_dynamic_section(
        std::span<const u8> file_data,
        const Elf64_Phdr& dyn_phdr,
        LoadedElfImage& image
    );
};

} // namespace papaya::storage
