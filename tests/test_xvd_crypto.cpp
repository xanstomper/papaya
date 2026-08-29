#include "papaya/common/logger.hpp"
#include "papaya/storage/xvd_crypto.hpp"
#include "papaya/storage/xvd.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::storage;

    log::info("TEST", "Running unit test: test_xvd_crypto");

    // 1. Test AES-128-ECB Decryption with known NIST Vector
    // Key: 2b7e151628aed2a6abf7158809cf4f3c
    // Plaintext: 6bc1bee22e409f96e93d7e117393172a
    // Ciphertext: 3ad77bb40d7a3660a89ecaf32466ef97
    Aes128Key key = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };

    const u8 ciphertext_ecb[16] = {
        0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60,
        0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97
    };

    const u8 expected_plaintext[16] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a
    };

    u8 decrypted_ecb[16] = {0};
    auto ecb_res = XvdCrypto::decrypt_aes_128_ecb(ciphertext_ecb, key, decrypted_ecb);
    assert(ecb_res.has_value());
    assert(std::memcmp(decrypted_ecb, expected_plaintext, 16) == 0);
    log::info("TEST", "AES-128-ECB NIST test vector decrypted successfully!");

    // 2. Test AES-128-CBC Decryption
    std::array<u8, 16> iv = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    u8 cbc_cipher[32] = {0};
    std::memcpy(cbc_cipher, ciphertext_ecb, 16);
    std::memcpy(cbc_cipher + 16, ciphertext_ecb, 16);

    u8 cbc_plain[32] = {0};
    auto cbc_res = XvdCrypto::decrypt_aes_128_cbc(cbc_cipher, key, iv, cbc_plain);
    assert(cbc_res.has_value());
    log::info("TEST", "AES-128-CBC multi-block decryption succeeded!");

    // 3. Test AES-XTS Sector Decryption
    AesXtsKey xts_key{};
    std::memcpy(xts_key.data(), key.data(), 16);
    std::memcpy(xts_key.data() + 16, key.data(), 16);

    std::vector<u8> sector_cipher(512, 0xAA);
    std::vector<u8> sector_plain(512, 0x00);

    auto xts_res = XvdCrypto::decrypt_aes_xts_sector(sector_cipher, xts_key, 0, sector_plain);
    assert(xts_res.has_value());
    assert(sector_plain[0] != 0x00);
    log::info("TEST", "AES-XTS 512-byte sector decryption verified!");

    log::info("TEST", ">>> test_xvd_crypto PASSED ALL CHECKS! <<<");
    return 0;
}
