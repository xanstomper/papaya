// D3D11 shader-creation integration test (Stage 3d).
//
// Drives the REAL ID3D11Device vtables: calls CreatePixelShader (slot 15)
// through the ms_abi thunk with a real DXBC container and verifies the shader
// object carries the translated GLSL (the exact output of the DXBC->GLSL
// pipeline: dxbc_parse -> sm4_decode -> sm4_emit_glsl). Garbage bytecode must
// yield a shader object with translated=false, never a crash.
#include "papaya/win32/win32_d3d.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using papaya::win32::d3d11_create_device;
using papaya::win32::d3d11_shader_get_glsl;
using papaya::u8;
using papaya::u32;
using papaya::u64;

#define D3DMS __attribute__((ms_abi))

static void put_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

// Minimal valid DXBC container with one SHDR chunk.
static std::vector<u8> build_dxbc(const std::vector<u32>& instructions) {
    std::vector<u8> b;
    auto put = [&](u32 v) { put_u32(b, v); };
    put(0x3000);                       // shader version (PS 4.0-ish)
    put(static_cast<u32>(instructions.size()));   // token count
    for (u32 w : instructions) put(w);
    const u32 total = 44 + 12 + static_cast<u32>(b.size());
    std::vector<u8> blob;
    blob.insert(blob.end(), { 'D','X','B','C' });
    for (int i = 0; i < 16; ++i) blob.push_back(0);
    put_u32(blob, 1);                  // blob version
    put_u32(blob, 0);                  // creator length
    put_u32(blob, 0);                  // creator offset
    put_u32(blob, total);              // total size
    put_u32(blob, 1);                  // chunk count
    put_u32(blob, 44);                 // chunk offset
    put_u32(blob, 0x52444853);         // 'SHDR'
    put_u32(blob, static_cast<u32>(b.size()));
    put_u32(blob, 56);                 // chunk data offset
    blob.insert(blob.end(), b.begin(), b.end());
    return blob;
}

int main() {
    // dcl_input v0; dcl_output o0; dcl_temps 1; mov r0, v0; add o0, r0, l(1,2,3,4)
    auto inst = [](u32 op, u32 len) { return ((len & 0x1Fu) << 24) | (op & 0xFFu); };
    auto opd = [](u32 rt, u32 order, u32 dim, u32 mask, u32 sw) {
        return (rt << 12) | ((order & 3) << 20) | (dim & 3) | ((mask & 0xF) << 4) | sw;
    };
    auto swz = [](u32 x, u32 y, u32 z, u32 w) { return (x&3)<<4 | (y&3)<<6 | (z&3)<<8 | (w&3)<<10; };
    auto fw = [](float f) { u32 x; std::memcpy(&x, &f, 4); return x; };
    const u32 kId = swz(0, 1, 2, 3);
    std::vector<u32> stream = {
        inst(0x5F, 3), opd(1, 1, 3, 0xF, 0), 0,
        inst(0x65, 3), opd(2, 1, 3, 0xF, 0), 0,
        inst(0x68, 2), 1,
        inst(0x36, 5), opd(0, 1, 3, 0xF, 0), 0, opd(1, 1, 3, 0, kId), 0,
        inst(0x00, 10), opd(2, 1, 3, 0xF, 0), 0, opd(0, 1, 3, 0, kId), 0,
        opd(4, 0, 3, 0, kId), fw(1.0f), fw(2.0f), fw(3.0f), fw(4.0f),
    };
    std::vector<u8> blob = build_dxbc(stream);

    void* dev = nullptr;
    void* ctx = nullptr;
    if (!d3d11_create_device(&dev, &ctx) || !dev) {
        std::printf("fail: device creation\n"); return 1;
    }
    void** vtbl = *reinterpret_cast<void***>(dev);
    // ID3D11Device::CreatePixelShader = slot 15 (after IUnknown + 12 methods).
    using create_shader_t = D3DMS long (*)(void*, void*, u64, void*, void*);
    auto create_pixel_shader = reinterpret_cast<create_shader_t>(vtbl[15]);

    void* shader = nullptr;
    long hr = create_pixel_shader(dev, blob.data(), blob.size(), nullptr, &shader);
    if (hr != 0 || !shader) { std::printf("fail: CreatePixelShader hr %ld\n", hr); return 2; }

    bool translated = false;
    const char* glsl = d3d11_shader_get_glsl(shader, &translated);
    if (!translated || !glsl) {
        std::printf("fail: shader not translated\n"); return 3;
    }
    if (std::strstr(glsl, "vec4 v0;") == nullptr ||
        std::strstr(glsl, "vec4 o0;") == nullptr ||
        std::strstr(glsl, "r0 = v0;") == nullptr ||
        std::strstr(glsl, "o0 = (r0 + vec4(1.0, 2.0, 3.0, 4.0));") == nullptr) {
        std::printf("fail: bad GLSL\n%s\n", glsl); return 4;
    }

    // Same call through CreateVertexShader (slot 12).
    auto create_vertex_shader = reinterpret_cast<create_shader_t>(vtbl[12]);
    void* vs = nullptr;
    if (create_vertex_shader(dev, blob.data(), blob.size(), nullptr, &vs) != 0 || !vs)
        { std::printf("fail: CreateVertexShader\n"); return 5; }

    // Garbage bytecode: call succeeds, translated=false (honest, no crash).
    std::vector<u8> garbage = { 1, 2, 3, 4, 5, 6, 7, 8 };
    void* bad = nullptr;
    if (create_pixel_shader(dev, garbage.data(), garbage.size(), nullptr, &bad) != 0 || !bad)
        { std::printf("fail: garbage shader call\n"); return 6; }
    bool bad_translated = true;
    const char* bad_glsl = d3d11_shader_get_glsl(bad, &bad_translated);
    if (bad_translated || (bad_glsl && bad_glsl[0] != '\0')) {
        std::printf("fail: garbage flagged translated (%d, '%s')\n", bad_translated,
                    bad_glsl ? bad_glsl : "");
        return 7;
    }
    if (d3d11_shader_get_glsl(nullptr, nullptr) != nullptr) {
        std::printf("fail: null shader\n"); return 8;
    }

    std::printf("ok: CreatePixelShader/CreateVertexShader DXBC->GLSL through vtable, garbage refused\n");
    return 0;
}