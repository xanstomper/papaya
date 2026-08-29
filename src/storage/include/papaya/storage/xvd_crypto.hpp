#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <array>
#include <span>
#include <vector>

namespace papaya::storage {

// AES-128 Key (16 bytes)
using Aes128Key = std::array<u8, 16>;

// AES-128-XTS Key (32 bytes: Key1 for data, Key2 for tweak)
using AesXtsKey = std::array<u8, 32>;

class XvdCrypto {
public:
    // Software AES-128 ECB / CBC / XTS implementation (portable, no external library required)
    static Result<> decrypt_aes_128_ecb(
        std::span<const u8> ciphertext,
        const Aes128Key& key,
        std::span<u8> plaintext
    );

    static Result<> decrypt_aes_128_cbc(
        std::span<const u8> ciphertext,
        const Aes128Key& key,
        const std::array<u8, 16>& iv,
        std::span<u8> plaintext
    );

    static Result<> decrypt_aes_xts_sector(
        std::span<const u8> ciphertext,
        const AesXtsKey& xts_key,
        u64 sector_index,
        std::span<u8> plaintext
    );

private:
    static void aes_expand_key(const u8* key, u32* round_keys);
    static void aes_decrypt_block(const u8* in, u8* out, const u32* round_keys);
};

} // namespace papaya::storage
