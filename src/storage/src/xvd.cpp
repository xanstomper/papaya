#include "papaya/storage/xvd.hpp"
#include "papaya/common/logger.hpp"
#include <fstream>
#include <cstring>

namespace papaya::storage {

XvdContainer::XvdContainer() = default;

XvdContainer::~XvdContainer() {
    is_open_ = false;
}

Result<> XvdContainer::open(const std::filesystem::path& path) {
    log::info("XVD", "Opening XVD container: {}", path.string());

    if (!std::filesystem::exists(path)) {
        log::error("XVD", "File does not exist: {}", path.string());
        return ErrorCode::InvalidParameter;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        log::error("XVD", "Cannot open file stream for: {}", path.string());
        return ErrorCode::InvalidParameter;
    }

    file.read(reinterpret_cast<char*>(&header_), sizeof(XvdHeader));
    if (file.gcount() < static_cast<std::streamsize>(sizeof(XvdHeader))) {
        log::error("XVD", "File smaller than XVD header size");
        return ErrorCode::XvdHeaderCorrupt;
    }

    // Check signature ("msxvd\0\0\0")
    if (std::memcmp(header_.signature, "msxvd", 5) != 0) {
        log::error("XVD", "Invalid XVD signature: {:02x}{:02x}{:02x}{:02x}",
                   header_.signature[0], header_.signature[1], header_.signature[2], header_.signature[3]);
        return ErrorCode::XvdInvalidSignature;
    }

    file_path_ = path;
    is_open_ = true;
    is_encrypted_ = (header_.volume_flags & 0x1) != 0;

    log::info("XVD", "Successfully mounted XVD: version {}, size {} GiB, encrypted: {}",
              header_.version, header_.volume_size / GiB, is_encrypted_);
    return {};
}

Result<std::vector<u8>> XvdContainer::read_sector(u64 sector_index, u32 sector_count) {
    if (!is_open_) {
        return ErrorCode::InvalidParameter;
    }

    u64 sector_size = header_.sector_size ? header_.sector_size : 4096;
    u64 offset = sizeof(XvdHeader) + (sector_index * sector_size);
    u64 read_bytes = sector_count * sector_size;

    std::ifstream file(file_path_, std::ios::binary);
    if (!file.is_open()) {
        return ErrorCode::InvalidParameter;
    }

    file.seekg(static_cast<std::streamoff>(offset));
    std::vector<u8> buffer(read_bytes);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(read_bytes));

    return buffer;
}

} // namespace papaya::storage
