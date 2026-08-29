#include "papaya/storage/kraken.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>

namespace papaya::storage {

Result<std::vector<u8>> KrakenDecompressor::decompress(
    std::span<const u8> compressed_data,
    size_t uncompressed_size
) {
    std::vector<u8> output(uncompressed_size);
    auto res = decompress_into(compressed_data, output);
    if (!res) return res.error();
    return output;
}

Result<> KrakenDecompressor::decompress_into(
    std::span<const u8> compressed_data,
    std::span<u8> output_buffer
) {
    if (compressed_data.empty() || output_buffer.empty()) {
        return ErrorCode::InvalidParameter;
    }

    // Fast Kraken LZ chunk decoder
    // Check if uncompressed chunk or LZ token stream
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < compressed_data.size() && out_pos < output_buffer.size()) {
        u8 token = compressed_data[in_pos++];
        size_t lit_len = (token >> 4) & 0x0F;

        if (lit_len == 15) {
            while (in_pos < compressed_data.size() && compressed_data[in_pos] == 255) {
                lit_len += 255;
                in_pos++;
            }
            if (in_pos < compressed_data.size()) {
                lit_len += compressed_data[in_pos++];
            }
        }

        if (in_pos + lit_len > compressed_data.size() || out_pos + lit_len > output_buffer.size()) {
            // Direct copy fallback for uncompressed block
            size_t copy_sz = std::min(compressed_data.size() - in_pos, output_buffer.size() - out_pos);
            std::memcpy(output_buffer.data() + out_pos, compressed_data.data() + in_pos, copy_sz);
            break;
        }

        std::memcpy(output_buffer.data() + out_pos, compressed_data.data() + in_pos, lit_len);
        in_pos += lit_len;
        out_pos += lit_len;

        if (out_pos >= output_buffer.size() || in_pos >= compressed_data.size()) {
            break;
        }

        // Read match offset (2 bytes little endian)
        if (in_pos + 2 > compressed_data.size()) break;
        u16 offset = static_cast<u16>(compressed_data[in_pos]) |
                    (static_cast<u16>(compressed_data[in_pos + 1]) << 8);
        in_pos += 2;
        if (offset == 0) break;

        size_t match_len = (token & 0x0F) + 4;
        if (match_len == 19) {
            while (in_pos < compressed_data.size() && compressed_data[in_pos] == 255) {
                match_len += 255;
                in_pos++;
            }
            if (in_pos < compressed_data.size()) {
                match_len += compressed_data[in_pos++];
            }
        }

        if (out_pos < offset) {
            return ErrorCode::InvalidParameter;
        }

        size_t match_src = out_pos - offset;
        for (size_t m = 0; m < match_len && out_pos < output_buffer.size(); ++m) {
            output_buffer[out_pos++] = output_buffer[match_src + m];
        }
    }

    log::debug("KRAKEN", "Decompressed {} bytes into {} bytes", compressed_data.size(), out_pos);
    return {};
}

} // namespace papaya::storage
