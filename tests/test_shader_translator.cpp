// Unit test for the DXBC container parser (Stage 1 of D3D11 shader translation).
//
// Builds a synthetic-but-valid DXBC container (header + chunk directory +
// SHDR bytecode + ISGN/OSGN signatures) and verifies dxbc_parse extracts the
// chunk, bytecode and signature correctly. Also verifies malformed inputs
// (bad magic, truncated) return false without crashing.
#include "papaya/gpu/shader_translator.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using papaya::gpu::dxbc_parse;
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

    std::printf("ok: dxbc parsed %zu chunks, %zu bytecode words, %zu input sigs\n",
                out.chunks.size(), out.shader_bytecode.size(), out.input_signature.size());
    return 0;
}