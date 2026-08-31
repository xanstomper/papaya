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

static std::vector<unsigned char> build_pe_seed() {
    // Minimal PE32+ skeleton: MZ + e_lfanew -> PE sig + COFF FileHeader
    // (AMD64, 1 section) + PE32+ OptionalHeader (magic 0x020B) + 1 section
    // header. Large enough to pass the "smaller than DOS header" gate and
    // reach the NT-header / section parsing that random mutations target.
    using papaya::u16;
    using papaya::u32;
    std::vector<unsigned char> b(512, 0);
    b[0] = 'M'; b[1] = 'Z';
    *reinterpret_cast<u32*>(b.data() + 0x3c) = 0x40;            // e_lfanew
    // PE signature (at 0x40)
    b[0x40] = 'P'; b[0x41] = 'E';
    // COFF FileHeader (at 0x44)
    *reinterpret_cast<u16*>(b.data() + 0x44) = 0x8664;          // Machine = AMD64
    *reinterpret_cast<u16*>(b.data() + 0x48) = 1;               // NumberOfSections
    *reinterpret_cast<u16*>(b.data() + 0x4A) = 0x00E0;          // SizeOfOptionalHeader (PE32+)
    // PE32+ OptionalHeader (at 0x58)
    *reinterpret_cast<u16*>(b.data() + 0x58) = 0x020B;          // Magic = PE32+
    // Section header (at 0x138): Name ".text", VirtualSize 0x1000
    b[0x138] = '.'; b[0x139] = 't'; b[0x13a] = 'e'; b[0x13b] = 'x'; b[0x13c] = 't';
    *reinterpret_cast<u32*>(b.data() + 0x140) = 0x1000;         // Misc.VirtualSize
    *reinterpret_cast<u32*>(b.data() + 0x144) = 0x1000;         // VirtualAddress
    return b;
}

int main(int argc, char** argv) {
    std::vector<unsigned char> buf = build_pe_seed();
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
