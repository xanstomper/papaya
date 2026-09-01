// In-process DXBC -> SPIR-V test (Stage 4c).
//
// Drives the full pipeline in one process: DXBC container -> sm4_decode ->
// GLSL emitter -> glslang library -> SPIR-V words. Asserts the SPIR-V magic
// and a non-empty module. When PAPAYA_HAS_GLSLANG is off (no glslang build),
// the API must fail cleanly rather than crash.
#include "papaya/gpu/shader_compile.hpp"
#include "papaya/gpu/shader_translator.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using papaya::gpu::dxbc_to_spirv;
using papaya::gpu::kStageFragment;
using papaya::gpu::kStageVertex;
using papaya::u8;
using papaya::u32;

static void put_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

// The ALU+texture shader used by test_shader_translator: dcl_input v0,
// dcl_output o0, dcl_temps 2, dcl_resource t0 (2D), dcl_sampler s0,
// mov r0, v0; sample r1, v0.xy, t0, s0; add o0, r0, r1.
static std::vector<u8> build_dxbc() {
    auto inst = [](u32 op, u32 len, u32 flags = 0) {
        return ((len & 0x1Fu) << 24) | ((flags & 0x7u) << 11) | (op & 0xFFu);
    };
    auto opd = [](u32 rt, u32 order, u32 dim, u32 mask, u32 sw) {
        return (rt << 12) | ((order & 3) << 20) | (dim & 3) | ((mask & 0xF) << 4) | sw;
    };
    auto swz = [](u32 x, u32 y, u32 z, u32 w) { return (x&3)<<4 | (y&3)<<6 | (z&3)<<8 | (w&3)<<10; };
    const u32 kId = swz(0, 1, 2, 3);
    const std::vector<u32> stream = {
        inst(0x5F, 3), opd(1, 1, 3, 0xF, 0), 0,
        inst(0x65, 3), opd(2, 1, 3, 0xF, 0), 0,
        inst(0x68, 2), 2,
        inst(0x58, 4, 3), opd(7, 1, 3, 0, 0), 0, 0x55555555,
        inst(0x5A, 3), opd(6, 1, 3, 0, 0), 0,
        inst(0x36, 5), opd(0, 1, 3, 0xF, 0), 0, opd(1, 1, 3, 0, kId), 0,
        inst(0x45, 9), opd(0, 1, 3, 0xF, 0), 1, opd(1, 1, 3, 0, kId), 0,
        opd(7, 1, 3, 0, 0), 0, opd(6, 1, 3, 0, 0), 0,
        inst(0x00, 7), opd(2, 1, 3, 0xF, 0), 0, opd(0, 1, 3, 0, kId), 0,
        opd(0, 1, 3, 0, kId), 1,
    };
    std::vector<u8> b;
    put_u32(b, 0x3000);
    put_u32(b, static_cast<u32>(stream.size()));
    for (u32 w : stream) put_u32(b, w);
    const u32 total = 44 + 12 + static_cast<u32>(b.size());
    std::vector<u8> blob;
    blob.insert(blob.end(), { 'D','X','B','C' });
    for (int i = 0; i < 16; ++i) blob.push_back(0);
    put_u32(blob, 1); put_u32(blob, 0); put_u32(blob, 0);
    put_u32(blob, total); put_u32(blob, 1); put_u32(blob, 44);
    put_u32(blob, 0x52444853);          // 'SHDR'
    put_u32(blob, static_cast<u32>(b.size()));
    put_u32(blob, 56);
    blob.insert(blob.end(), b.begin(), b.end());
    return blob;
}

int main() {
    const std::vector<u8> blob = build_dxbc();
    std::vector<u32> spirv;
    std::string err;

    constexpr u32 kSpirvMagic = 0x07230203u;

#ifdef PAPAYA_HAS_GLSLANG
    if (!dxbc_to_spirv({blob.data(), blob.size()}, kStageFragment, spirv, err)) {
        std::printf("fail: fragment compile: %s\n", err.c_str()); return 1;
    }
    if (spirv.size() < 16 || spirv[0] != kSpirvMagic) {
        std::printf("fail: bad SPIR-V (words=%zu magic=%x)\n", spirv.size(),
                    spirv.empty() ? 0u : spirv[0]);
        return 2;
    }
    if (!dxbc_to_spirv({blob.data(), blob.size()}, kStageVertex, spirv, err)) {
        std::printf("fail: vertex compile: %s\n", err.c_str()); return 3;
    }
    if (spirv[0] != kSpirvMagic) return 4;
    const size_t vertex_words = spirv.size();

    // Garbage bytecode must fail cleanly with a message, no crash.
    std::vector<u8> garbage = { 1, 2, 3, 4, 5 };
    if (dxbc_to_spirv({garbage.data(), garbage.size()}, kStageFragment, spirv, err))
        { std::printf("fail: garbage compiled\n"); return 5; }
    if (err.empty()) { std::printf("fail: no error message\n"); return 6; }

    std::printf("ok: DXBC -> in-process glslang -> SPIR-V (vertex %zu words)\n",
                vertex_words);
    return 0;
#else
    // No glslang build: the API must refuse cleanly (the test then passes as a
    // skip; run with -DPAPAYA_GLSLANG_ROOT=... and rebuilt glslang to enable).
    if (dxbc_to_spirv({blob.data(), blob.size()}, kStageFragment, spirv, err))
        { std::printf("fail: compiled without glslang\n"); return 1; }
    std::printf("ok: glslang not linked, clean refusal (build with PAPAYA_GLSLANG_ROOT)\n");
    return 0;
#endif
}