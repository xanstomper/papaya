#include "papaya/common/logger.hpp"
#include "papaya/storage/kraken.hpp"
#include <cassert>
#include <vector>
#include <cstring>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::storage;

    log::info("TEST", "Running unit test: test_kraken");

    std::string original = "PlayStation 5 Prospero OS Oodle Kraken Asset Stream";
    std::vector<u8> compressed;

    // LZ token: 0xF0 (>=15 literals), followed by (51 - 15) = 36
    compressed.push_back(0xF0);
    compressed.push_back(static_cast<u8>(original.size() - 15));
    for (char c : original) {
        compressed.push_back(static_cast<u8>(c));
    }

    auto dec_res = KrakenDecompressor::decompress(compressed, original.size());
    assert(dec_res.has_value());
    assert(std::memcmp(dec_res->data(), original.data(), original.size()) == 0);

    log::info("TEST", "Decompressed string matches original: '{}'", original);
    log::info("TEST", ">>> test_kraken PASSED ALL CHECKS! <<<");
    return 0;
}
