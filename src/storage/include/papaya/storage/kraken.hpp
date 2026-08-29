#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <span>
#include <vector>

namespace papaya::storage {

class KrakenDecompressor {
public:
    // Software Oodle Kraken & LZ4 stream decompressor
    static Result<std::vector<u8>> decompress(
        std::span<const u8> compressed_data,
        size_t uncompressed_size
    );

    static Result<> decompress_into(
        std::span<const u8> compressed_data,
        std::span<u8> output_buffer
    );
};

} // namespace papaya::storage
