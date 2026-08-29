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

    // Unmaps and frees loaded image
    void unload_image(LoadedPeImage& image);

private:
    Result<> map_sections(const u8* file_raw, const ImageNtHeaders64* nt_hdr, void* allocated_base);
    Result<> apply_relocations(const ImageNtHeaders64* nt_hdr, void* allocated_base, u64 original_image_base);
    Result<> resolve_imports(const u8* file_raw, const ImageNtHeaders64* nt_hdr, void* allocated_base);

    std::shared_ptr<Win32ApiHle> hle_;
};

} // namespace papaya::win32
