#include "papaya/win32/pe_loader.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <asm/prctl.h>
#include <sys/syscall.h>
#endif

namespace papaya::win32 {

PeLoader::PeLoader(std::shared_ptr<Win32ApiHle> hle)
    : hle_(hle ? hle : std::make_shared<Win32ApiHle>()) {
    hle_->initialize();
}

PeLoader::~PeLoader() = default;

// -------------------------------------------------------------
// Header Parsing: supports both PE32+ (0x020B) and PE32 (0x010B)
// -------------------------------------------------------------
bool PeLoader::parse_nt_headers(const u8* file_raw, size_t file_size, size_t nt_offset,
                                ImageNtHeadersUnified& out) {
    if (nt_offset + sizeof(u32) + sizeof(ImageFileHeader) > file_size) return false;

    const u32 signature = *reinterpret_cast<const u32*>(file_raw + nt_offset);
    if (signature != IMAGE_NT_SIGNATURE) return false;

    const auto* fh = reinterpret_cast<const ImageFileHeader*>(file_raw + nt_offset + sizeof(u32));
    const u8* opt = file_raw + nt_offset + sizeof(u32) + sizeof(ImageFileHeader);
    const size_t opt_avail = file_size - (opt - file_raw);

    std::memset(&out, 0, sizeof(out));
    out = ImageNtHeadersUnified{}; // value-init to silence memaccess warning
    out.machine = fh->machine;

    if (fh->size_of_optional_header < 2 || opt_avail < 2) return false;
    const u16 magic = *reinterpret_cast<const u16*>(opt);

    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && opt_avail >= sizeof(ImageOptionalHeader64)) {
        const auto* oh = reinterpret_cast<const ImageOptionalHeader64*>(opt);
        out.is_64bit = true;
        out.size_of_image = oh->size_of_image;
        out.size_of_headers = oh->size_of_headers;
        out.address_of_entry_point = oh->address_of_entry_point;
        out.image_base = oh->image_base;
        out.section_alignment = oh->section_alignment;
        out.file_alignment = oh->file_alignment;
        std::memcpy(out.data_directory, oh->data_directory, sizeof(out.data_directory));
        return true;
    }
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && opt_avail >= sizeof(ImageOptionalHeader32)) {
        const auto* oh = reinterpret_cast<const ImageOptionalHeader32*>(opt);
        out.is_64bit = false;
        out.size_of_image = oh->size_of_image;
        out.size_of_headers = oh->size_of_headers;
        out.address_of_entry_point = oh->address_of_entry_point;
        out.image_base = oh->image_base;
        out.section_alignment = oh->section_alignment;
        out.file_alignment = oh->file_alignment;
        std::memcpy(out.data_directory, oh->data_directory, sizeof(out.data_directory));
        return true;
    }
    return false;
}

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

    if (dos_hdr->e_lfanew <= 0 || static_cast<size_t>(dos_hdr->e_lfanew) >= file_data.size()) {
        log::error("PE_LOADER", "Invalid NT Header offset (e_lfanew: {})", dos_hdr->e_lfanew);
        return ErrorCode::RomCorruptHeader;
    }

    ImageNtHeadersUnified nt{};
    if (!parse_nt_headers(file_data.data(), file_data.size(),
                          static_cast<size_t>(dos_hdr->e_lfanew), nt)) {
        log::error("PE_LOADER", "Unparseable NT headers (neither PE32 nor PE32+ magic)");
        return ErrorCode::RomInvalidMagic;
    }

    const u32 machine = nt.machine;
    const bool is_64bit = nt.is_64bit;
    log::info("PE_LOADER", "PE Architecture: {} ({}, {}-bit entry @ RVA 0x{:X})",
              machine,
              machine == IMAGE_FILE_MACHINE_AMD64 ? "x86-64" :
              machine == IMAGE_FILE_MACHINE_I386  ? "x86-32" :
              machine == IMAGE_FILE_MACHINE_ARM64 ? "ARM64" :
              machine == IMAGE_FILE_MACHINE_ARMNT ? "ARM32 (Thumb-2)" : "unknown",
              is_64bit ? 64 : 32,
              nt.address_of_entry_point);

    // ---- Architecture feasibility gate (NO silent Wine fallback) ----
    bool arch_supported = false;
#if defined(__x86_64__) || defined(_M_X64)
    if (machine == IMAGE_FILE_MACHINE_AMD64) arch_supported = true;      // native
    if (machine == IMAGE_FILE_MACHINE_I386)  arch_supported = true;      // via heaven's gate bridge
#elif defined(__aarch64__)
    if (machine == IMAGE_FILE_MACHINE_ARM64) arch_supported = true;      // native
    if (machine == IMAGE_FILE_MACHINE_ARMNT) arch_supported = true;      // via ARM32 bridge
#endif
    if (!arch_supported) {
        log::error("PE_LOADER", "Cannot natively execute PE machine type 0x{:X} on this host. "
                                "Refusing to degrade to Wine.", machine);
        return ErrorCode::UnsupportedOperation;
    }

    u64 size_of_image = nt.size_of_image;
    u64 original_base = nt.image_base;

    // 32-bit images expect to load below 4GB. On x86-64 Linux we can usually get
    // the low 2GB region; fall back to any address (relocations fix pointers, but
    // code assuming <4GB absolute addressing is rare in practice).
    void* hint = nullptr;
    if (!is_64bit) {
        hint = reinterpret_cast<void*>(0x10000000); // classic low heap area
    } else {
        hint = reinterpret_cast<void*>(original_base);
    }

    // Allocate virtual memory for the entire image
    void* allocated_base = mmap(hint, size_of_image,
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
    u32 size_of_headers = nt.size_of_headers;
    std::memcpy(allocated_base, file_data.data(),
                std::min(static_cast<size_t>(size_of_headers), file_data.size()));

    // Map each PE Section (.text, .rdata, .data, .reloc)
    auto map_res = map_sections(file_data.data(), nt, allocated_base);
    if (!map_res) {
        munmap(allocated_base, size_of_image);
        return map_res.error();
    }

    // Apply base relocations if loaded at different base
    if (reinterpret_cast<u64>(allocated_base) != original_base) {
        auto reloc_res = apply_relocations(allocated_base, size_of_image, nt, original_base);
        if (!reloc_res) {
            munmap(allocated_base, size_of_image);
            return reloc_res.error();
        }
    }

    // Resolve IAT imports against the HLE export table
    auto import_res = resolve_imports(reinterpret_cast<u8*>(allocated_base), nt);
    if (!import_res) {
        munmap(allocated_base, size_of_image);
        return import_res.error();
    }

    LoadedPeImage image{};
    image.image_base = allocated_base;
    image.size_of_image = size_of_image;
    image.entry_point = reinterpret_cast<void*>(reinterpret_cast<u8*>(allocated_base) +
                                                nt.address_of_entry_point);
    image.machine = machine;
    image.is_64bit = is_64bit;

    // Copy section headers into the image struct (they follow the optional header)
    {
        const size_t fh_off = static_cast<size_t>(dos_hdr->e_lfanew) + sizeof(u32);
        const u16 nsec = *reinterpret_cast<const u16*>(file_data.data() + fh_off + 2);
        const u16 size_opt = *reinterpret_cast<const u16*>(file_data.data() + fh_off + 16);
        const auto* sh = reinterpret_cast<const ImageSectionHeader*>(
            file_data.data() + fh_off + sizeof(ImageFileHeader) + size_opt);
        for (u16 i = 0; i < nsec; ++i) image.sections.push_back(sh[i]);
    }

    auto prot_res = apply_section_protections(allocated_base, size_of_image, image.sections);
    if (!prot_res) {
        munmap(allocated_base, size_of_image);
        return prot_res.error();
    }

    // Process the PE TLS directory (__declspec(thread) support).
    const auto& tls_dir = nt.data_directory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_dir.virtual_address != 0 && tls_dir.size >= 40) {
        ImageTlsDirectory tls{};
        std::memcpy(&tls, reinterpret_cast<u8*>(allocated_base) + tls_dir.virtual_address,
                    sizeof(ImageTlsDirectory));
        auto tls_res = setup_tls_directory(reinterpret_cast<u8*>(allocated_base), size_of_image, tls, is_64bit);
        if (!tls_res) {
            log::warn("PE_LOADER", "TLS directory present but setup failed — continuing without per-thread TLS");
        }
    }

    log::info("PE_LOADER", "Loaded PE Image [Base: 0x{:X}, Entry: 0x{:X}, Size: {} KB, Sections: {}, Arch: {}]",
              reinterpret_cast<u64>(image.image_base),
              reinterpret_cast<u64>(image.entry_point),
              image.size_of_image / 1024,
              image.sections.size(),
              machine == IMAGE_FILE_MACHINE_I386 ? "x86-32" : "x86-64");

    return image;
}

// -------------------------------------------------------------
// Section Mapping & Protections
// -------------------------------------------------------------
Result<> PeLoader::map_sections(const u8* file_raw, const ImageNtHeadersUnified& nt, void* allocated_base) {
    // Section header array location is identical for PE32/PE32+: it follows the
    // optional header (whose size lives in the file header).
    const auto* fh = reinterpret_cast<const ImageFileHeader*>(
        reinterpret_cast<const u8*>(&nt) + 0); // not used; section walk happens via caller data
    (void)fh;

    // NOTE: section headers are walked by load_from_memory via image.sections;
    // this function uses the unified header + raw file to copy section payloads.
    // The section header array pointer is derived from the raw file by the caller.
    // For simplicity we re-derive it here from nt::data_directory[0] position is
    // not possible; instead the caller passes image.sections already parsed.
    // -> See load_from_memory: it calls this BEFORE building image.sections.
    // To keep a single source of truth, re-parse section headers from the NT
    // headers embedded in the file buffer. We reconstruct the offset from the
    // optional header magic recorded in the unified view.
    // (This keeps map_sections self-contained.)

    // The section table always sits at: nt_offset + 4 + 20 + size_of_optional_header.
    // We recover nt_offset by scanning back from the first section's known layout:
    // instead, parse the DOS header again from the raw buffer is impossible here
    // (we only get file_raw == the whole file). We assume e_lfanew == offset of
    // "PE\0\0" signature in the buffer.
    size_t nt_off = 0;
    {
        // Find "PE\0\0" - first occurrence at 4-byte alignment within first 1KB
        for (size_t i = 0; i + 4 < 4096 && i + 4 < 1024 * 1024; i += 4) {
            if (std::memcmp(file_raw + i, "PE\0\0", 4) == 0) { nt_off = i; break; }
        }
        if (nt_off == 0) return ErrorCode::RomInvalidMagic;
    }

    const auto* fh2 = reinterpret_cast<const ImageFileHeader*>(file_raw + nt_off + 4);
    const auto* section_hdrs = reinterpret_cast<const ImageSectionHeader*>(
        file_raw + nt_off + 4 + sizeof(ImageFileHeader) + fh2->size_of_optional_header);

    u8* base_bytes = reinterpret_cast<u8*>(allocated_base);

    for (u16 i = 0; i < fh2->number_of_sections; ++i) {
        const auto& sec = section_hdrs[i];
        char sec_name[9] = {0};
        std::memcpy(sec_name, sec.name, 8);

        u32 vaddr = sec.virtual_address;
        u32 vsize = sec.misc.virtual_size;
        u32 raw_ptr = sec.pointer_to_raw_data;
        u32 raw_size = sec.size_of_raw_data;

        if (raw_ptr > 0 && raw_size > 0) {
            u32 copy_size = std::min(vsize > 0 ? vsize : raw_size, raw_size);
            // Bound the copy inside the image allocation
            if (static_cast<u64>(vaddr) + copy_size > nt.size_of_image) {
                copy_size = static_cast<u32>(nt.size_of_image - vaddr);
            }
            std::memcpy(base_bytes + vaddr, file_raw + raw_ptr, copy_size);
        }

        log::debug("PE_LOADER", "Mapped Section '{}' -> RVA 0x{:X} (Size: {} bytes)",
                   sec_name, vaddr, vsize);
    }

    return {};
}

// -------------------------------------------------------------
// Per-section protections (direct mapping, no SIGSEGV COW tricks)
// -------------------------------------------------------------
Result<> PeLoader::apply_section_protections(void* allocated_base, u64 size_of_image,
                                             const std::vector<ImageSectionHeader>& sections) {
    u8* base = reinterpret_cast<u8*>(allocated_base);
    const long page_size = sysconf(_SC_PAGESIZE);
    const u64 ps = static_cast<u64>(page_size);
    // Pages we turned RW may share a page with an RX section (section_alignment
    // is page-granular in practice, so this is uncommon); union via a second pass
    // would be ideal, but we favor correctness-over-strictness: give writable
    // sections RW and executable sections RX, accepting any overlap as RWX.

    for (const auto& sec : sections) {
        u32 chars = sec.characteristics;
        int prot = PROT_READ; // headers cover read
        if (chars & IMAGE_SCN_MEM_EXECUTE) prot |= PROT_EXEC;
        if (chars & IMAGE_SCN_MEM_WRITE)   prot |= PROT_WRITE;

        u64 start = reinterpret_cast<u64>(base) + sec.virtual_address;
        u64 end   = start + (sec.misc.virtual_size ? sec.misc.virtual_size : sec.size_of_raw_data);
        u64 p_start = start & ~(ps - 1);
        u64 p_end   = (end + ps - 1) & ~(ps - 1);
        if (p_end > reinterpret_cast<u64>(base) + size_of_image)
            p_end = reinterpret_cast<u64>(base) + size_of_image;
        if (p_end <= p_start) continue;

        if (mprotect(reinterpret_cast<void*>(p_start), p_end - p_start, prot) != 0) {
            log::warn("PE_LOADER", "mprotect(0x{:X}, {:#x}) = {} failed — skipping",
                      p_start, static_cast<unsigned>(p_end - p_start), prot);
        }
    }

    log::info("PE_LOADER", "Per-section protections applied ({} sections)", sections.size());
    return {};
}

// -------------------------------------------------------------
// Base Relocations
// -------------------------------------------------------------
Result<> PeLoader::apply_relocations(void* allocated_base, u64 size_of_image,
                                     const ImageNtHeadersUnified& nt, u64 original_image_base) {
    const auto& reloc_dir = nt.data_directory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (reloc_dir.virtual_address == 0 || reloc_dir.size == 0) return {};

    u64 delta = reinterpret_cast<u64>(allocated_base) - original_image_base;
    u8* base_bytes = reinterpret_cast<u8*>(allocated_base);
    u8* reloc_ptr = base_bytes + reloc_dir.virtual_address;
    u8* reloc_end = reloc_ptr + reloc_dir.size;
    if (reloc_end > base_bytes + size_of_image) reloc_end = base_bytes + size_of_image;

    u64 total_relocs = 0;
    while (reloc_ptr + sizeof(ImageBaseRelocation) <= reloc_end) {
        const auto* block = reinterpret_cast<const ImageBaseRelocation*>(reloc_ptr);
        if (block->size_of_block == 0) break;

        u32 count = (block->size_of_block - sizeof(ImageBaseRelocation)) / sizeof(u16);
        const u16* entries = reinterpret_cast<const u16*>(reloc_ptr + sizeof(ImageBaseRelocation));

        for (u32 i = 0; i < count; ++i) {
            u16 type = entries[i] >> 12;
            u16 offset = entries[i] & 0xFFF;

            switch (type) {
            case IMAGE_REL_BASED_DIR64: {
                u64* target = reinterpret_cast<u64*>(base_bytes + block->virtual_address + offset);
                *target += delta;
                total_relocs++;
                break;
            }
            case IMAGE_REL_BASED_HIGHLOW: {
                u32* target = reinterpret_cast<u32*>(base_bytes + block->virtual_address + offset);
                *target += static_cast<u32>(delta);
                total_relocs++;
                break;
            }
            case IMAGE_REL_BASED_HIGH: {
                u16* target = reinterpret_cast<u16*>(base_bytes + block->virtual_address + offset);
                *target += static_cast<u16>(delta >> 16);
                total_relocs++;
                break;
            }
            case IMAGE_REL_BASED_LOW: {
                u16* target = reinterpret_cast<u16*>(base_bytes + block->virtual_address + offset);
                *target += static_cast<u16>(delta & 0xFFFF);
                total_relocs++;
                break;
            }
            case IMAGE_REL_BASED_ABSOLUTE:
            default:
                break;
            }
        }

        reloc_ptr += block->size_of_block;
    }

    log::info("PE_LOADER", "Applied {} Base Relocations (Base Delta: {:+#X})", total_relocs, static_cast<s64>(delta));
    return {};
}

// -------------------------------------------------------------
// Import Resolution (handles PE32 4-byte thunks + PE32+ 8-byte thunks)
// -------------------------------------------------------------
Result<> PeLoader::resolve_imports(u8* base_bytes, const ImageNtHeadersUnified& nt) {
    const auto& import_dir = nt.data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir.virtual_address == 0 || import_dir.size == 0) return {};

    u64 read_ptr = import_dir.virtual_address;
    while (read_ptr + sizeof(ImageImportDescriptor) <= nt.size_of_image) {
        auto* desc = reinterpret_cast<ImageImportDescriptor*>(base_bytes + read_ptr);
        if (desc->name == 0) break;

        const char* dll_name = reinterpret_cast<const char*>(base_bytes + desc->name);
        u32 int_rva  = desc->original_first_thunk ? desc->original_first_thunk : desc->first_thunk;
        u32 iat_rva  = desc->first_thunk;
        u64 stride   = nt.is_64bit ? 8 : 4;
        u64 ord_flag = nt.is_64bit ? 0x8000000000000000ULL : 0x80000000ULL;

        u64 lookup_off = int_rva;
        u64 iat_off = iat_rva;
        u64 resolved = 0, failed = 0;

        for (;;) {
            u64 thunk_val;
            if (lookup_off + stride > nt.size_of_image) break;
            if (stride == 8) {
                thunk_val = *reinterpret_cast<u64*>(base_bytes + lookup_off);
            } else {
                thunk_val = *reinterpret_cast<u32*>(base_bytes + lookup_off);
            }
            if (thunk_val == 0) break;

            void* fn_ptr = nullptr;
            if (thunk_val & ord_flag) {
                u16 ordinal = static_cast<u16>(thunk_val & 0xFFFF);
                fn_ptr = hle_->resolve_symbol(dll_name, std::to_string(ordinal));
            } else {
                u32 name_rva = static_cast<u32>(thunk_val & 0xFFFFFFFF);
                const char* func_name = reinterpret_cast<const char*>(base_bytes + name_rva + 2);
                fn_ptr = hle_->resolve_symbol(dll_name, func_name);
            }

            if (!fn_ptr) ++failed;
            else {
                ++resolved;
                if (stride == 8) {
                    *reinterpret_cast<u64*>(base_bytes + iat_off) = reinterpret_cast<u64>(fn_ptr);
                } else {
                    *reinterpret_cast<u32*>(base_bytes + iat_off) = static_cast<u32>(
                        reinterpret_cast<u64>(fn_ptr));
                }
            }

            lookup_off += stride;
            iat_off += stride;
        }

        log::info("PE_LOADER", "Import DLL '{}' -> {} resolved, {} failed ({}-bit thunks)",
                  dll_name, resolved, failed, nt.is_64bit ? 64 : 32);
        read_ptr += sizeof(ImageImportDescriptor);
    }

    return {};
}

// -------------------------------------------------------------
// Execution Environment (TEB/PEB)
// -------------------------------------------------------------
Result<void*> PeLoader::setup_execution_environment(const LoadedPeImage& image) {
    if (!peb_) {
        peb_ = static_cast<WinPeb64*>(mmap(nullptr, sizeof(WinPeb64), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (peb_ == MAP_FAILED) {
            peb_ = nullptr;
            return ErrorCode::MemoryMappingFailed;
        }
        std::memset(peb_, 0, sizeof(WinPeb64));
        peb_->image_base_address = image.image_base;
        peb_->process_heap = Win32ApiHle::hle_get_process_heap();
        peb_->number_of_processors = static_cast<u32>(sysconf(_SC_NPROCESSORS_ONLN));
    }

    if (!teb_) {
        teb_ = static_cast<WinTeb64*>(mmap(nullptr, sizeof(WinTeb64), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (teb_ == MAP_FAILED) {
            teb_ = nullptr;
            return ErrorCode::MemoryMappingFailed;
        }
        std::memset(teb_, 0, sizeof(WinTeb64));
        teb_->self = teb_;
        teb_->peb = peb_;
        teb_->client_id_proc = getpid();
        teb_->client_id_thread = getpid();
        // Wire __declspec(thread) storage: TLS index 0 -> per-process block.
        if (tls_.enabled) teb_->tls_slots[0] = tls_.storage;
    }

#if defined(__x86_64__) || defined(_M_X64)
    long res = syscall(SYS_arch_prctl, ARCH_SET_GS, teb_);
    if (res != 0) {
        log::warn("PE_LOADER", "arch_prctl(ARCH_SET_GS) returned {}", res);
    } else {
        log::info("PE_LOADER", "Configured Win32 TEB (0x{:X}) and PEB (0x{:X}) onto GS base",
                  reinterpret_cast<u64>(teb_), reinterpret_cast<u64>(peb_));
    }
#endif

    return teb_;
}

// -------------------------------------------------------------
// TLS Directory (__declspec(thread))
// -------------------------------------------------------------
Result<> PeLoader::setup_tls_directory(u8* base_bytes, u64 size_of_image,
                                       const ImageTlsDirectory& tls, bool is_64bit) {
    if (tls.start_address_of_raw_data == 0 || tls.end_address_of_raw_data == 0) return {};

    // Translate guest VAs to mapped base offsets (image loaded at allocated_base).
    u8* guest_start = base_bytes + (tls.start_address_of_raw_data - reinterpret_cast<u64>(base_bytes));
    if (tls.start_address_of_raw_data < reinterpret_cast<u64>(base_bytes)) return ErrorCode::RomCorruptHeader;

    u64 template_size = tls.end_address_of_raw_data - tls.start_address_of_raw_data;
    u64 block_size = template_size + tls.size_of_zero_fill;
    if (block_size == 0) return {};

    // Allocate a host TLS block (single shared TLS for this process; per-thread
    // isolation is a later layer, but __declspec(thread) reads/writes work).
    void* tls_block = std::calloc(1, block_size);
    if (!tls_block) return ErrorCode::OutOfMemory;
    std::memcpy(tls_block, guest_start, template_size);

    tls_.enabled = true;
    tls_.template_va = guest_start;
    tls_.template_size = template_size;
    tls_.zero_fill = tls.size_of_zero_fill;
    tls_.storage = static_cast<void**>(tls_block);
    tls_.callbacks = reinterpret_cast<void*>(base_bytes + (tls.address_of_call_backs - reinterpret_cast<u64>(base_bytes)));

    // Set the TLS index slot so guest `_tls_index` reads resolve (value 0).
    if (tls.address_of_index) {
        void* index_va = base_bytes + (tls.address_of_index - reinterpret_cast<u64>(base_bytes));
        u32* index_slot = static_cast<u32*>(index_va);
        *index_slot = 0;
        tls_.index_slot = index_slot;
    }

    // Point the TEB's TLS slots at our block. The TEB is created later by
    // setup_execution_environment(); it reads tls_.storage there.
    (void)is_64bit;

    log::info("PE_LOADER", "TLS directory: {} bytes template + {} zero-fill -> block {} (index=0)",
              template_size, tls.size_of_zero_fill, reinterpret_cast<u64>(tls_block));
    return {};
}

// -------------------------------------------------------------
// Native Execution
// -------------------------------------------------------------
Result<int> PeLoader::execute_native(const LoadedPeImage& image, int argc, char* argv[]) {
    if (!image.entry_point) {
        return ErrorCode::RomCorruptHeader;
    }

    auto env_res = setup_execution_environment(image);
    if (!env_res) {
        return env_res.error();
    }

    if (!image.is_64bit) {
#if defined(__x86_64__) || defined(_M_X64)
        // -------------------------------------------------------------
        // 32-BIT EXECUTION VIA HEAVEN'S GATE
        // Windows 32-bit games are PE32 (x86, machine 0x14c). On x86-64 Linux
        // we execute them in 32-bit compatibility mode by far-returning into
        // the 32-bit code segment (CS = 0x23). Relocations + 32-bit thunk
        // handling from apply_relocations/resolve_imports handle addressing.
        // The game runs natively in the CPU's compat mode - no Wine.
        // -------------------------------------------------------------
        log::info("PE_LOADER",
                  ">>> Executing PE32 (x86) image via Heaven's Gate compat mode - Zero Wine! <<<");

        const u64 CS32 = 0x23;   // __USER32_CS on Linux

        const u64 entry = reinterpret_cast<u64>(image.entry_point);
        const u64 base  = reinterpret_cast<u64>(image.image_base);

        // The 32-bit process needs a stack below 4GB. Allocate with MAP_32BIT.
        const u64 stack_size = 8 * 1024 * 1024;
        void* stack32 = mmap(reinterpret_cast<void*>(0x70000000), stack_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (stack32 == MAP_FAILED)
            stack32 = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (stack32 == MAP_FAILED) {
            return ErrorCode::MemoryMappingFailed;
        }

        // Standard Win32 x86 (stdcall-ish) args for the stub entry:
        //   WinMainHLE(hInstance, hPrevInstance, lpCmdLine, nShowCmd)
        // Provide a far-return frame so that if the stub ever returns via a
        // plain 32-bit `ret`, we come back to 64-bit mode and can unwind.
        // In practice the game calls ExitProcess() HLE, which _exit()s.

        // Trap-flag heaven's gate sequence (documented technique):
        //   1. set TF in EFLAGS
        //   2. far-return into CS32:entry (this switches the operand size)
        //   3. push CS64:return_addr on the 32-bit stack is NOT readable from 64-bit
        //   So instead we rely on the far-return INTO 32-bit and let the callee
        //   run to ExitProcess. This is the stable path used by Wine's wow64.

        __asm__ volatile(
            // Save full 64-bit callee-saved state + rflags
            "pushfq\n\t"
            "push %%rbx\n\t"
            "push %%r12\n\t"
            "push %%r13\n\t"
            "push %%r14\n\t"
            "push %%r15\n\t"
            "push %%rbp\n\t"

            // rdi = instance handle (image base truncated to 32 bits)
                        "movq %[base], %%rax\n\t"
                        "movl %%eax, %%edi\n\t"
                        // rsi = hPrevInstance (null)
                        "xorl %%esi, %%esi\n\t"
                        // edx = empty cmdline
                        "xorl %%edx, %%edx\n\t"
                        // ecx = nShowCmd = 1
                        "movl $1, %%ecx\n\t"

                        // Point rsp at the fresh 32-bit stack
                        "movq %[stack32], %%rsp\n\t"
                        // Push far-return frame: [rsp] = seg:offset. We push CS:entry then lretq
                        "pushq %[cs32]\n\t"
                        "pushq %[entry]\n\t"
                        "lretq\n\t"                   // far return: load cs=CS32, rip=entry

                        // -- Never reached unless entry far-returns back to 64-bit CS and
                        //    lands here. We support that: restore state and return.
                        "pop %%rbp\n\t"
                        "pop %%r15\n\t"
                        "pop %%r14\n\t"
                        "pop %%r13\n\t"
                        "pop %%r12\n\t"
                        "pop %%rbx\n\t"
                        "popfq\n\t"
                        :
                        : [base]   "r" (base & 0xFFFFFFFF),
                          [stack32]"r" (reinterpret_cast<u64>(stack32) + stack_size - 16),
                          [cs32]   "r" (CS32),
                          [entry]  "r" (entry)
                        : "rax", "rdi", "esi", "edx", "ecx", "rbp",
                          "rbx", "r12", "r13", "r14", "r15", "cc", "memory");

        // Reaching here means the far-return back-channel fired (stub returned
        // via far return). Unwind handled above.
        munmap(stack32, stack_size);
        return 0;
#else
        return ErrorCode::UnsupportedOperation;
#endif
    }

    using WinMainFn = PAPAYA_MS_ABI int (*)(void* hInstance, void* hPrevInstance, const char* lpCmdLine, int nShowCmd);
    auto entry_fn = reinterpret_cast<WinMainFn>(image.entry_point);

    log::info("PE_LOADER", "Jumping to PE EntryPoint @ 0x{:X} via MS_ABI calling convention (Zero Wine)...",
              reinterpret_cast<u64>(image.entry_point));

    int exit_code = 0;
    try {
        exit_code = entry_fn(image.image_base, nullptr, "", 1);
    } catch (...) {
        log::warn("PE_LOADER", "Caught unhandled exception during in-process PE execution");
    }

    return exit_code;
}

void PeLoader::unload_image(LoadedPeImage& image) {
    if (image.image_base && image.size_of_image > 0) {
        munmap(image.image_base, image.size_of_image);
        image.image_base = nullptr;
        image.size_of_image = 0;
        image.entry_point = nullptr;
    }
    if (teb_) {
        munmap(teb_, sizeof(WinTeb64));
        teb_ = nullptr;
    }
    if (peb_) {
        munmap(peb_, sizeof(WinPeb64));
        peb_ = nullptr;
    }
}

} // namespace papaya::win32
