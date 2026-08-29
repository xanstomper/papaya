#include "papaya/storage/xvd.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>

namespace papaya::storage {

XvdContainer::XvdContainer() = default;
XvdContainer::~XvdContainer() {
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
}

Result<> XvdContainer::open(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        log::error("XVD", "XVD file not found: {}", path.string());
        return ErrorCode::FileNotFound;
    }

    file_stream_.open(path, std::ios::binary);
    if (!file_stream_) {
        log::error("XVD", "Failed to open XVD file: {}", path.string());
        return ErrorCode::InvalidParameter;
    }

    file_stream_.read(reinterpret_cast<char*>(&header_), sizeof(XvdHeader));
    if (file_stream_.gcount() < static_cast<std::streamsize>(sizeof(XvdHeader))) {
        log::error("XVD", "XVD file too small to contain valid header");
        return ErrorCode::XvdHeaderCorrupt;
    }

    if (std::memcmp(header_.signature, "msxvd\0\0\0", 8) != 0 &&
        std::memcmp(header_.signature, "msxvc\0\0\0", 8) != 0) {
        log::error("XVD", "Invalid XVD/XVC signature");
        return ErrorCode::XvdInvalidSignature;
    }

    file_path_ = path;
    is_open_ = true;
    is_encrypted_ = (header_.volume_flags & 1) != 0;

    log::info("XVD", "Opened XVD: '{}' [Version: 0x{:X}, Size: {} MB, Encrypted: {}]",
              path.filename().string(), header_.version, header_.volume_size / (1024 * 1024), is_encrypted_);

    return {};
}

Result<> XvdContainer::set_decryption_key(const AesXtsKey& key) {
    decryption_key_ = key;
    has_key_ = true;
    log::info("XVD", "Set 256-bit AES-XTS volume decryption key");
    return {};
}

Result<std::vector<u8>> XvdContainer::read_sector(u64 sector_index, u32 sector_count) {
    if (!is_open_) {
        return ErrorCode::InvalidParameter;
    }

    u64 sec_size = (header_.sector_size > 0) ? header_.sector_size : 512;
    u64 total_bytes = sec_size * sector_count;
    u64 file_offset = sizeof(XvdHeader) + (sector_index * sec_size);

    std::vector<u8> buffer(total_bytes);
    file_stream_.seekg(file_offset, std::ios::beg);
    file_stream_.read(reinterpret_cast<char*>(buffer.data()), total_bytes);

    size_t bytes_read = file_stream_.gcount();
    if (bytes_read < total_bytes) {
        buffer.resize(bytes_read);
    }

    // Decrypt if volume is encrypted and key is set
    if (is_encrypted_ && has_key_) {
        std::vector<u8> decrypted(buffer.size());
        for (u32 s = 0; s < sector_count; ++s) {
            size_t off = s * sec_size;
            if (off + sec_size <= buffer.size()) {
                auto dec_res = XvdCrypto::decrypt_aes_xts_sector(
                    std::span<const u8>(buffer.data() + off, sec_size),
                    decryption_key_,
                    sector_index + s,
                    std::span<u8>(decrypted.data() + off, sec_size)
                );
                if (!dec_res) {
                    return dec_res.error();
                }
            }
        }
        return decrypted;
    }

    return buffer;
}

} // namespace papaya::storage
