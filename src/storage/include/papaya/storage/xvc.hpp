#pragma once

#include "papaya/common/types.hpp"
#include "papaya/storage/xvd.hpp"

namespace papaya::storage {

#pragma pack(push, 1)
struct XvcChunkEntry {
    u64 raw_offset;        // Physical byte offset in container
    u32 compressed_size;   // Size on disk
    u32 uncompressed_size; // Decompressed size
    u8  chunk_sha256[32];  // Chunk checksum
};

struct XvcPackageHeader {
    u8  magic[4];          // "XVC\0"
    u32 content_version;
    u64 total_chunks;
    u64 manifest_offset;
};
#pragma pack(pop)

} // namespace papaya::storage
