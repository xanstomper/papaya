#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <array>

namespace papaya::storage {

// Magic headers: "msxvd\0\0\0" or "XVD\0"
constexpr u32 XVD_MAGIC_SIGNATURE = 0x6476736D; // "msvd" in little endian

#pragma pack(push, 1)
struct XvdHeader {
    u8  signature[8];       // "msxvd\0\0\0"
    u32 version;           // Format revision
    u32 volume_flags;      // Encryption / ReadOnly / Dynamic flags
    u64 sector_size;       // Virtual sector size (e.g. 512 or 4096)
    u64 volume_size;       // Total uncompressed volume size in bytes
    u64 dynamic_header_off;// Offset to dynamic allocation table
    u8  volume_guid[16];   // Unique Container GUID
    u8  encrypted_key[32]; // AES-XTS or AES-CBC volume key block
    u8  header_hash[32];   // SHA-256 integrity hash
};
#pragma pack(pop)

enum class XvdVolumeType {
    Fixed,
    Dynamic,
    Encrypted,
    PackageXvc
};

class XvdContainer {
public:
    XvdContainer();
    ~XvdContainer();

    Result<> open(const std::filesystem::path& path);
    Result<std::vector<u8>> read_sector(u64 sector_index, u32 sector_count = 1);
    
    const XvdHeader& get_header() const { return header_; }
    u64 get_total_size() const { return header_.volume_size; }
    bool is_encrypted() const { return is_encrypted_; }
    bool is_open() const { return is_open_; }

private:
    std::filesystem::path file_path_;
    XvdHeader header_{};
    bool is_open_{false};
    bool is_encrypted_{false};
};

} // namespace papaya::storage
