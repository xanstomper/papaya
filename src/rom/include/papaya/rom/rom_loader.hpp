#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/rom/rom_types.hpp"
#include <filesystem>
#include <vector>
#include <span>
#include <memory>

namespace papaya::rom {

class RomImageLoader {
public:
    RomImageLoader();
    ~RomImageLoader();

    // Opens a ROM/ISO from a host filesystem path (Linux) or raw descriptor (Android SAF)
    Result<RomMetadata> open_file(const std::filesystem::path& file_path);
    Result<RomMetadata> open_descriptor(int fd, u64 total_size);

    // Reads sectors at specified Logical Block Address (LBA)
    Result<> read_sector_lba(u64 lba, std::span<u8> destination_buffer);
    Result<> read_sectors_span(u64 start_lba, u32 sector_count, u8* destination);

    // Closes current image
    void close();

    bool is_open() const { return is_open_; }
    const RomMetadata& get_metadata() const { return metadata_; }

    // Auto-detect format from raw header bytes
    static RomFormat detect_format(std::span<const u8> header_bytes, u64 file_size);

    // Fast CRC32 calculation utility
    static u32 calculate_crc32(std::span<const u8> data, u32 initial_crc = 0xFFFFFFFF);

private:
    Result<> parse_iso9660_metadata();
    Result<> parse_cso_metadata();
    Result<> parse_switch_metadata();

    int file_descriptor_{-1};
    bool is_open_{false};
    bool owns_fd_{false};
    RomMetadata metadata_{};
    u64 file_size_{0};
};

} // namespace papaya::rom
