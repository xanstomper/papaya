// SPDX-License-Identifier: MIT
// PE-parser fuzz harness for papaya::win32::PeLoader.
//
// Target surface: PeLoader::load_from_memory — header parsing, section
// mapping, base relocations, import resolution, TLS directory and load-config
// processing on ARBITRARY byte spans. execute_native (code execution) is NOT
// invoked, so this is a parser fuzzer: crashes found here are host-severity
// memory-safety bugs in the PE mapping pipeline.
//
// Build matrix:
//   clang -DPAPAYA_ENABLE_FUZZING=ON  -> libFuzzer (LLVMFuzzerTestOneInput only)
//   gcc  -DPAPAYA_ENABLE_FUZZING=ON  -> ASan/UBSan smoke mode (driver main())
//
// The PeLoader is a process-singleton: its Win32ApiHle (X11/VK) init happens
// once, on first use, and is reused across iterations.
#include "papaya/win32/pe_loader.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <vector>

namespace pwin = papaya::win32;

// Invoked by libFuzzer (clang) and by the self-driver (gcc). Returns 0 so a
// benign parse failure (the common case for random bytes) is NOT a crash.
extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    // One PeLoader per process. Construction is lazy (HLE init on first call); a
    // failed/malformed load leaves the loader reusable for the next input.
    static pwin::PeLoader loader;
    auto img = loader.load_from_memory(std::span<const unsigned char>(data, size));
    if (img) loader.unload_image(*img);
    return 0;
}

#if !defined(__clang__)
// ---- Portable GCC/ASan driver (no libFuzzer available) -----------------
// Deterministic mutate-driven smoke: restore each byte after mutation so the
// seed stays recognizable to the parser across iterations. Feeding a real PE
// file (argv) exercises the happy path; with no args it fuzzes a tiny MZ stub.
static std::vector<unsigned char> read_file(const char* path) {
    std::vector<unsigned char> out;
    if (FILE* fp = std::fopen(path, "rb")) {
        std::fseek(fp, 0, SEEK_END);
        long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
        if (n > 0) std::fread(out.data(), 1, out.size(), fp);
        std::fclose(fp);
    }
    return out;
}

int main(int argc, char** argv) {
    std::vector<unsigned char> buf = {
        'M','Z', 0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'P','E','\0','\0', 0x4c, 0x01, 0x01, 0x00
    };
    long iters = 4096;
    bool replay = (argc > 1);
    if (replay) { buf = read_file(argv[1]); if (buf.empty()) { iters = 0; } }

    unsigned char* data = buf.data();
    std::size_t n = buf.size();
    std::mt19937 rng(0xC0FFEE);

    for (long i = 0; i < (replay ? 1 : iters); ++i) {
        std::size_t pos = n ? (rng() % n) : 0;
        unsigned char orig = data[pos];
        data[pos] = static_cast<unsigned char>(rng() & 0xff);
        LLVMFuzzerTestOneInput(data, n);
        data[pos] = orig;
    }
    std::printf("fuzz_pe_parse smoke: %ld iterations over %zu bytes, no crash\n",
                replay ? 1 : iters, n);
    return 0;
}
#endif // !defined(__clang__)
