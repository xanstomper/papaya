#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/win32/pe_types.hpp"
#include "papaya/win32/win32_api_hle.hpp"
#include <filesystem>
#include <span>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <signal.h>

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

    // Applies per-section mprotect after mapping (page-granular, includes headers R+X)
    Result<> apply_section_protections(void* allocated_base, u64 size_of_image,
                                       const std::vector<ImageSectionHeader>& sections);

    // Split-fault emulation: switch protection at a page, restart the faulting x86-64
    // instruction via single-step trap flag.
    Result<> handle_guard_page_fault(u8* fault_addr, void* ctx);

    // Install a SIGSEGV handler that services guard-page write faults (WC pages) and
    // no-access faults (RO pages), flipping page protection and single-stepping past.
    void install_fault_handler();
    static void fault_handler(int sig, siginfo_t* info, void* ctx);

    Result<> apply_relocations(void* allocated_base, u64 size_of_image,
                               const ImageNtHeadersUnified& nt, u64 original_image_base);

    Result<> resolve_imports(u8* base_bytes, const ImageNtHeadersUnified& nt);

    std::shared_ptr<Win32ApiHle> hle_;
    WinTeb64* teb_{nullptr};
    WinPeb64* peb_{nullptr};

    // Guard-page bookkeeping for split-fault WC/RO emulation (page_start -> prot)
    std::map<u64, u32> guard_pages_;
    static std::map<u64, u32> s_guard_pages;      // static mirror for the signal handler
    static std::mutex s_guard_mutex;
    static thread_local void* s_active_loader;
};

} // namespace papaya::win32
