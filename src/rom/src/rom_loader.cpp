#include "papaya/rom/rom_loader.hpp"
#include "papaya/common/logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace papaya::rom {

// Standard ISO 9660 Primary Volume Descriptor offset
constexpr u64 ISO_PVD_OFFSET = 16ULL * SECTOR_SIZE_ISO_DATA; // 0x8000

RomImageLoader::RomImageLoader() = default;

RomImageLoader::~RomImageLoader() {
    close();
}

void RomImageLoader::close() {
    if (is_open_) {
        if (owns_fd_ && file_descriptor_ >= 0) {
            ::close(file_descriptor_);
        }
        file_descriptor_ = -1;
        is_open_ = false;
        owns_fd_ = false;
        metadata_ = {};
        file_size_ = 0;
    }
}

u32 RomImageLoader::calculate_crc32(std::span<const u8> data, u32 initial_crc) {
    u32 crc = initial_crc;
    for (u8 byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

RomFormat RomImageLoader::detect_format(std::span<const u8> header_bytes, u64 file_size) {
    if (header_bytes.size() >= 4) {
        if (std::memcmp(header_bytes.data(), "CISO", 4) == 0) {
            return RomFormat::CsoCompressed;
        }
        if (std::memcmp(header_bytes.data(), "MComprHD", 8) == 0) {
            return RomFormat::ChdCompressed;
        }
        if (std::memcmp(header_bytes.data(), "PFS0", 4) == 0) {
            return RomFormat::NspSwitch;
        }
        if (std::memcmp(header_bytes.data(), "HEAD", 4) == 0) {
            return RomFormat::XciSwitch;
        }
    }

    if (file_size >= ISO_PVD_OFFSET + 6 && header_bytes.size() >= ISO_PVD_OFFSET + 6) {
        if (std::memcmp(header_bytes.data() + ISO_PVD_OFFSET + 1, "CD001", 5) == 0) {
            return RomFormat::Iso9660;
        }
    }

    return RomFormat::Iso9660;
}

Result<RomMetadata> RomImageLoader::open_file(const std::filesystem::path& file_path) {
    close();

    int fd = open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log::error("ROM", "Failed to open ROM file: '{}'", file_path.string());
        return ErrorCode::FileNotFound;
    }

    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 0) {
        ::close(fd);
        return ErrorCode::RomCorruptHeader;
    }

    owns_fd_ = true;
    metadata_.title_name = file_path.stem().string();

    return open_descriptor(fd, static_cast<u64>(sz));
}

Result<RomMetadata> RomImageLoader::open_descriptor(int fd, u64 total_size) {
    if (fd < 0 || total_size == 0) {
        return ErrorCode::InvalidParameter;
    }

    file_descriptor_ = fd;
    file_size_ = total_size;
    is_open_ = true;

    // Read header chunk for format auto-detection
    std::vector<u8> header_buf(std::min(total_size, static_cast<u64>(0x9000)), 0);
    ssize_t bytes_read = pread(file_descriptor_, header_buf.data(), header_buf.size(), 0);
    if (bytes_read <= 0) {
        close();
        return ErrorCode::RomCorruptHeader;
    }

    metadata_.format = detect_format(header_buf, total_size);
    metadata_.total_file_size_bytes = total_size;
    metadata_.sector_size = SECTOR_SIZE_ISO_DATA;
    metadata_.total_sectors = total_size / metadata_.sector_size;
    metadata_.calculated_crc32 = calculate_crc32(std::span<const u8>(header_buf.data(), std::min(header_buf.size(), size_t(4096))));

    if (metadata_.format == RomFormat::Iso9660) {
        parse_iso9660_metadata();
    }

    log::info("ROM", "Mounted ROM Image [Title: '{}', Serial: '{}', Format: 0x{:X}, Sectors: {}, Size: {} MB]",
              metadata_.title_name, metadata_.disc_serial_id,
              static_cast<int>(metadata_.format), metadata_.total_sectors, metadata_.total_file_size_bytes / MiB);

    return metadata_;
}

Result<> RomImageLoader::read_sector_lba(u64 lba, std::span<u8> destination_buffer) {
    if (!is_open_ || file_descriptor_ < 0) return ErrorCode::RomSectorReadFailed;
    if (destination_buffer.size() < metadata_.sector_size) return ErrorCode::InvalidParameter;

    u64 offset = lba * metadata_.sector_size;
    if (offset + metadata_.sector_size > file_size_) {
        return ErrorCode::RomSectorReadFailed;
    }

    ssize_t n = pread(file_descriptor_, destination_buffer.data(), metadata_.sector_size, offset);
    if (n != static_cast<ssize_t>(metadata_.sector_size)) {
        return ErrorCode::RomSectorReadFailed;
    }

    return {};
}

Result<> RomImageLoader::read_sectors_span(u64 start_lba, u32 sector_count, u8* destination) {
    if (!is_open_ || file_descriptor_ < 0 || !destination || sector_count == 0) {
        return ErrorCode::RomSectorReadFailed;
    }

    u64 offset = start_lba * metadata_.sector_size;
    u64 total_bytes = static_cast<u64>(sector_count) * metadata_.sector_size;
    if (offset + total_bytes > file_size_) {
        return ErrorCode::RomSectorReadFailed;
    }

    ssize_t n = pread(file_descriptor_, destination, total_bytes, offset);
    if (n != static_cast<ssize_t>(total_bytes)) {
        return ErrorCode::RomSectorReadFailed;
    }

    return {};
}

Result<> RomImageLoader::parse_iso9660_metadata() {
    if (file_size_ < ISO_PVD_OFFSET + SECTOR_SIZE_ISO_DATA) {
        return {};
    }

    std::vector<u8> pvd(SECTOR_SIZE_ISO_DATA, 0);
    ssize_t n = pread(file_descriptor_, pvd.data(), SECTOR_SIZE_ISO_DATA, ISO_PVD_OFFSET);
    if (n == SECTOR_SIZE_ISO_DATA && std::memcmp(pvd.data() + 1, "CD001", 5) == 0) {
        // Volume Identifier at offset 40 (32 bytes)
        std::string vol_id(reinterpret_cast<const char*>(pvd.data() + 40), 32);
        // Trim trailing spaces
        vol_id.erase(std::find_if(vol_id.rbegin(), vol_id.rend(), [](unsigned char ch) {
            return !std::isspace(ch) && ch != 0;
        }).base(), vol_id.end());

        if (!vol_id.empty()) {
            metadata_.title_name = vol_id;
        }

        // Application Identifier at offset 592 (128 bytes)
        std::string app_id(reinterpret_cast<const char*>(pvd.data() + 592), 32);
        app_id.erase(std::find_if(app_id.rbegin(), app_id.rend(), [](unsigned char ch) {
            return !std::isspace(ch) && ch != 0;
        }).base(), app_id.end());

        if (!app_id.empty()) {
            metadata_.disc_serial_id = app_id;
        }
    }

    return {};
}

} // namespace papaya::rom
