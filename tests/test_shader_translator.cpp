// Unit test for the DXBC container parser (Stage 1 of D3D11 shader translation).
//
// Builds a synthetic-but-valid DXBC container (header + chunk directory +
// SHDR bytecode + ISGN/OSGN signatures) and verifies dxbc_parse extracts the
// chunk, bytecode and signature correctly. Also verifies malformed inputs
// (bad magic, truncated) return false without crashing.
#include "papaya/gpu/shader_translator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using papaya::gpu::dxbc_parse;
using papaya::gpu::dxbc_to_glsl;
using papaya::gpu::DxbcContainer;
using papaya::gpu::fourcc;
using papaya::u8;
using papaya::u32;

// Little-endian writer.
static void put_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

int main() {
    // ---- Build a valid DXBC container ----
    // SHDR chunk: [version u32][instruction stream ...]. We make the "stream"
    // a couple of dummy instruction words so the bytecode extraction is real.
    std::vector<u8> shdr_body;
    put_u32(shdr_body, 0x3000);          // shader version (VS 4.0-ish)
    put_u32(shdr_body, 0x00010002);      // dummy instruction words
    put_u32(shdr_body, 0x00000000);

    // ISGN chunk: one 32-byte element + semantic string at a known offset.
    std::vector<u8> isgn_body;
    put_u32(isgn_body, 1);               // element count
    const u32 elem_offset = 4;           // elements start right after count
    const u32 elem_size = 32;
    // Place the semantic string AFTER the element (so name_off is clean).
    u32 name_off = elem_offset + elem_size;
    put_u32(isgn_body, name_off);        // [0] semanticNameOffset (chunk-rel)
    put_u32(isgn_body, 0);               // [1] semanticIndex
    put_u32(isgn_body, 0);               // [2] systemValue NONE
    put_u32(isgn_body, 0);               // [3] componentType F32
    put_u32(isgn_body, 0);               // [4] register 0
    put_u32(isgn_body, 0x0F);            // [5] mask xyzw
    put_u32(isgn_body, 0x0F);            // [6] readWriteMask
    put_u32(isgn_body, 0);               // [7] stream
    const char* sem = "POSITION";
    for (const char* p = sem; *p; ++p) isgn_body.push_back(static_cast<u8>(*p));
    isgn_body.push_back(0);
    // 4-byte align.
    while ((isgn_body.size() & 3) != 0) isgn_body.push_back(0);

    // Compose chunks, compute directory. Header is 44 bytes; the directory
    // (2 entries x 12) follows immediately at offset 44.
    struct Chunk { u32 tag; std::vector<u8> body; };
    Chunk chunks[] = {
        { fourcc('S','H','D','R'), std::move(shdr_body) },
        { fourcc('I','S','G','N'), std::move(isgn_body) },
    };
    u32 total = 44 + static_cast<u32>(2 * 12);   // header + directory
    // directory entries first (they hold offsets relative to file start)
    std::vector<u8> dir;
    for (auto& c : chunks) {
        u32 off = total;
        u32 sz = static_cast<u32>(c.body.size());
        put_u32(dir, c.tag);
        put_u32(dir, sz);
        put_u32(dir, off);
        total += sz;
    }

    std::vector<u8> blob;
    blob.insert(blob.end(), { 'D','X','B','C' });          // magic @0
    for (int i = 0; i < 16; ++i) blob.push_back(0);        // checksum @4-19
    put_u32(blob, 1);                                      // blob version @20
    put_u32(blob, 0);                                      // creator length @24
    put_u32(blob, 0);                                      // creator offset @28
    put_u32(blob, total);                                  // total size @32
    put_u32(blob, 2);                                      // chunk count @36
    put_u32(blob, 44);                                     // chunk offset @40
    blob.insert(blob.end(), dir.begin(), dir.end());
    for (auto& c : chunks) blob.insert(blob.end(), c.body.begin(), c.body.end());

    // ---- Parse the valid container ----
    DxbcContainer out;
    if (!dxbc_parse({blob.data(), blob.size()}, out)) {
        std::printf("fail: valid DXBC rejected\n");
        return 1;
    }
    if (out.chunks.size() != 2) { std::printf("fail: chunk count %zu\n", out.chunks.size()); return 2; }
    if (out.shader_bytecode.size() < 2) { std::printf("fail: bytecode size %zu\n", out.shader_bytecode.size()); return 3; }
    if (out.input_signature.size() != 1) {
        std::printf("fail: input sig size %zu (expected 1)\n", out.input_signature.size());
        return 4;
    }
    if (out.input_signature[0].semantic != "POSITION") {
        std::printf("fail: semantic '%s'\n", out.input_signature[0].semantic.c_str());
        return 5;
    }

    // ---- Malformed inputs must fail cleanly ----
    std::vector<u8> bad_magic = { 'N','O','T','D','X','B','C' };
    DxbcContainer m;
    if (dxbc_parse({bad_magic.data(), bad_magic.size()}, m)) { std::printf("fail: bad magic parsed\n"); return 6; }
    std::vector<u8> truncated = blob;
    truncated.resize(20);   // too short for header
    if (dxbc_parse({truncated.data(), truncated.size()}, m)) { std::printf("fail: truncated parsed\n"); return 7; }

    // ---- End-to-end: DXBC container -> GLSL (Stage 3d) ----
    auto build_blob = [](std::vector<u32> shdr_words) {
        std::vector<u8> b;
        for (u32 w : shdr_words) put_u32(b, w);
        u32 total = 44 + 12 + static_cast<u32>(b.size());
        std::vector<u8> blob;
        blob.insert(blob.end(), { 'D','X','B','C' });
        for (int i = 0; i < 16; ++i) blob.push_back(0);
        put_u32(blob, 1);      // version
        put_u32(blob, 0);      // creator len
        put_u32(blob, 0);      // creator off
        put_u32(blob, total);  // total size
        put_u32(blob, 1);      // chunk count
        put_u32(blob, 44);     // chunk offset
        put_u32(blob, fourcc('S','H','D','R'));
        put_u32(blob, static_cast<u32>(b.size()));
        put_u32(blob, 56);     // chunk data offset
        blob.insert(blob.end(), b.begin(), b.end());
        return blob;
    };

    // dcl_input v0; dcl_output o0; dcl_temps 2; dcl_resource t0 (2D);
    // dcl_sampler s0;
    // mov r0, v0;  sample r1, v0.xy, t0, s0;  add o0, r0, r1
    auto inst = [](u32 op, u32 len, u32 flags = 0) {
        return ((len & 0x1Fu) << 24) | ((flags & 0x7u) << 11) | (op & 0xFFu);
    };
    auto opd = [](u32 rt, u32 order, u32 dim, u32 mask, u32 sw) {
        return (rt << 12) | ((order & 3) << 20) | (dim & 3) | ((mask & 0xF) << 4) | sw;
    };
    auto swz = [](u32 x, u32 y, u32 z, u32 w) { return (x&3)<<4 | (y&3)<<6 | (z&3)<<8 | (w&3)<<10; };
    const u32 kIdentity = swz(0, 1, 2, 3);
    std::vector<u32> stream = {
        inst(0x5F, 3), opd(1, 1, 3, 0xF, 0), 0,                   // dcl_input v0
        inst(0x65, 3), opd(2, 1, 3, 0xF, 0), 0,                   // dcl_output o0
        inst(0x68, 2), 2,                                        // dcl_temps 2
        inst(0x58, 4, 3), opd(7, 1, 3, 0, 0), 0, 0x55555555,     // dcl_resource t0 (2D)
        inst(0x5A, 3), opd(6, 1, 3, 0, 0), 0,                    // dcl_sampler s0
        inst(0x36, 5), opd(0, 1, 3, 0xF, 0), 0,
        opd(1, 1, 3, 0, kIdentity), 0,                           // mov r0, v0
        inst(0x45, 9), opd(0, 1, 3, 0xF, 0), 1,                  // sample r1, v0.xy, t0, s0
        opd(1, 1, 3, 0, kIdentity), 0,
        opd(7, 1, 3, 0, 0), 0,
        opd(6, 1, 3, 0, 0), 0,
        inst(0x00, 7), opd(2, 1, 3, 0xF, 0), 0,                  // add o0, r0, r1
        opd(0, 1, 3, 0, kIdentity), 0,
        opd(0, 1, 3, 0, kIdentity), 1,
    };
    std::vector<u32> shdr = { 0x3000, static_cast<u32>(stream.size()) };
    shdr.insert(shdr.end(), stream.begin(), stream.end());
    std::vector<u8> blob2 = build_blob(shdr);
    std::string glsl;
    if (!dxbc_to_glsl({blob2.data(), blob2.size()}, glsl)) {
        std::printf("fail: dxbc_to_glsl rejected valid shader\n"); return 8;
    }
    const auto has = [&](const char* s) { return glsl.find(s) != std::string::npos; };
    if (!has("#version 310 es") || !has("void main()")) {
        std::printf("fail: not a complete shader\n%s\n", glsl.c_str()); return 9;
    }
    if (!has("vec4 v0;") || !has("vec4 o0;") || !has("vec4 r0;") || !has("vec4 r1;") ||
        !has("uniform sampler2D t0;")) {
        std::printf("fail: missing declarations\n%s\n", glsl.c_str()); return 10;
    }
    if (!has("r0 = v0;") || !has("r1 = texture(t0, v0.xy);") ||
        !has("o0 = (r0 + r1);")) {
        std::printf("fail: wrong body\n%s\n", glsl.c_str()); return 11;
    }

    // When GLSLANG_VALIDATOR is set (scripts/verify_shader_glsl.sh), prove the
    // emitted shader COMPILES: glslang must produce SPIR-V with no errors.
    if (const char* validator = std::getenv("GLSLANG_VALIDATOR"); validator && *validator) {
        const char* frag = "/tmp/papaya_test_shader.frag";
        const char* spv = "/tmp/papaya_test_shader.spv";
        {
            FILE* f = std::fopen(frag, "w");
            if (f) { std::fputs(glsl.c_str(), f); std::fclose(f); }
        }
        std::string cmd = std::string(validator) + " -V -S frag -o " + spv + " " + frag +
                          " >/tmp/papaya_glslang.log 2>&1";
        const int rc = std::system(cmd.c_str());
        bool has_spv = false;
        if (FILE* f = std::fopen(spv, "rb")) {
            has_spv = std::fseek(f, 0, SEEK_END) == 0 && std::ftell(f) > 0;
            std::fclose(f);
        }
        if (rc != 0 || !has_spv) {
            std::printf("fail: glslang rejected emitted shader (rc=%d)\n%s\n", rc,
                        glsl.c_str());
            return 12;
        }
    }

    // Unsupported opcode inside a container must fail the whole translation.
    std::vector<u32> bad_stream = { inst(0x45, 9), opd(0, 1, 3, 0xF, 0), 0,
                                    opd(1, 1, 3, 0, kIdentity), 0,
                                    opd(7, 1, 3, 0, 0), 0,
                                    opd(6, 1, 3, 0, 0), 0 };   // sample without dcl_resource
    std::vector<u32> shdr2 = { 0x3000, static_cast<u32>(bad_stream.size()) };
    shdr2.insert(shdr2.end(), bad_stream.begin(), bad_stream.end());
    std::vector<u8> blob3 = build_blob(shdr2);
    if (dxbc_to_glsl({blob3.data(), blob3.size()}, glsl)) {
        std::printf("fail: unresolved-resource sample translated\n"); return 11;
    }

    std::printf("ok: dxbc parsed %zu chunks, %zu bytecode words, %zu input sigs, dxbc_to_glsl verified\n",
                out.chunks.size(), out.shader_bytecode.size(), out.input_signature.size());
    return 0;
}