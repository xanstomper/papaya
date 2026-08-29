#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/win32/pe_types.hpp"
#include "papaya/win32/win32_api_hle.hpp"
#include <filesystem>
#include <span>
#include <vector>
#include <memory>

namespace papaya::win32 {

struct LoadedPeImage {
    void* image_base{nullptr};
    u64 size_of_image{0};
    void* entry_point{nullptr};
    u16 machine{IMAGE_FILE_MACHINE_AMD64};
    bool is_64bit{true};
    std::vector<ImageSectionHeader> sections;
};

class PeLoader {
public:
    explicit PeLoader(std::shared_ptr<Win32ApiHle> hle = nullptr);
    ~PeLoader();

    // Loads PE from file path
    Result<LoadedPeImage> load_from_file(const std::filesystem::path& file_path);

    // Loads PE from raw memory buffer
    Result<LoadedPeImage> load_from_memory(std::span<const u8> file_data);

    // Set up Win32 TEB and PEB structures on host GS register
    Result<void*> setup_execution_environment(const LoadedPeImage& image);

    // Executes PE entry point in-process using MS ABI calling convention
    Result<int> execute_native(const LoadedPeImage& image, int argc = 0, char* argv[] = nullptr);

    // Unmaps and frees loaded image
    void unload_image(LoadedPeImage& image);

private:
    // Normalize either PE32 or PE32+ NT headers into a unified view
    static bool parse_nt_headers(const u8* file_raw, size_t file_size, size_t nt_offset,
                                 ImageNtHeadersUnified& out);

    Result<> map_sections(const u8* file_raw, const ImageNtHeadersUnified& nt, void* allocated_base);

    // Applies per-section mprotect (page-granular, includes headers R+X)
    Result<> apply_section_protections(void* allocated_base, u64 size_of_image,
                                       const std::vector<ImageSectionHeader>& sections);

    Result<> apply_relocations(void* allocated_base, u64 size_of_image,
                               const ImageNtHeadersUnified& nt, u64 original_image_base);

    Result<> resolve_imports(u8* base_bytes, const ImageNtHeadersUnified& nt);

    // Process the PE TLS directory: allocate per-thread TLS block from the
    // template, set the TLS index slot, and mark callbacks for run-on-load.
    Result<> setup_tls_directory(u8* base_bytes, u64 size_of_image,
                                 const ImageTlsDirectory& tls, bool is_64bit);

    std::shared_ptr<Win32ApiHle> hle_;
    WinTeb64* teb_{nullptr};
    WinPeb64* peb_{nullptr};
    // Per-process TLS block + a stable index (single-threaded for now).
    ImgTlsContext tls_;
};

} // namespace papaya::win32
