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
using papaya::win32::d3d11_context_pipeline_snapshot;
using papaya::win32::d3d11_input_layout_count;
using papaya::win32::d3d11_input_layout_element;
using papaya::win32::d3d11_shader_get_spirv;
using papaya::win32::d3d11_context_vertex_data;
using papaya::win32::d3d11_context_draw_vertices;
using papaya::win32::d3d11_context_cbuffer;
#include "papaya/gpu/vulkan_swapchain.hpp"
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

    // The shader object also carries in-process compiled SPIR-V (glslang).
#ifdef PAPAYA_HAS_GLSLANG
    u32 spv_words = 0;
    const u32* spv = d3d11_shader_get_spirv(shader, &spv_words);
    if (!spv || spv_words < 16 || spv[0] != 0x07230203u) {
        std::printf("fail: no SPIR-V on shader object\n"); return 5;
    }
#endif

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

    // ---- pipeline-state plumbing through the real context vtables ----
    // ID3D11DeviceContext (per d3d11.idl): 9=PSSetShader, 11=VSSetShader,
    // 17=IASetInputLayout; ID3D11Device: 11=CreateInputLayout.
    void** ctx_vtbl = *reinterpret_cast<void***>(ctx);
    using set_shader_t = D3DMS void (*)(void*, void*, void*, u32);
    using set_layout_t = D3DMS void (*)(void*, void*);
    using create_layout_t = D3DMS long (*)(void*, void*, u32, void*, u64, void*);

    // CreateInputLayout: POSITION (R32G32B32_FLOAT) + TEXCOORD (R32G32_FLOAT).
    struct D3D11_INPUT_ELEMENT_DESC {
        const char* name;
        u32 index, format, slot, offset, klass, rate;
    } descs[2] = {
        { "POSITION", 0, 6, 0, 0, 0, 0 },        // 6 = DXGI_FORMAT_R32G32B32_FLOAT
        { "TEXCOORD", 0, 16, 0, 12, 0, 0 },      // 16 = DXGI_FORMAT_R32G32_FLOAT
    };
    void* layout = nullptr;
    auto create_input_layout = reinterpret_cast<create_layout_t>(vtbl[11]);
    if (create_input_layout(dev, descs, 2, blob.data(), blob.size(), &layout) != 0 || !layout)
        { std::printf("fail: CreateInputLayout\n"); return 9; }
    if (d3d11_input_layout_count(layout) != 2) { std::printf("fail: layout count\n"); return 10; }
    u32 sem_idx = 99, fmt = 99;
    const char* sem = d3d11_input_layout_element(layout, 0, &sem_idx, &fmt);
    if (!sem || std::strcmp(sem, "POSITION") != 0 || sem_idx != 0 || fmt != 6)
        { std::printf("fail: layout element 0 (%s, %u, %u)\n", sem ? sem : "?", sem_idx, fmt); return 11; }

    // Bind the pipeline state through the context vtables.
    auto ps_set_shader = reinterpret_cast<set_shader_t>(ctx_vtbl[9]);
    auto vs_set_shader = reinterpret_cast<set_shader_t>(ctx_vtbl[11]);
    auto ia_set_layout = reinterpret_cast<set_layout_t>(ctx_vtbl[17]);
    ps_set_shader(ctx, shader, nullptr, 0);
    vs_set_shader(ctx, vs, nullptr, 0);
    ia_set_layout(ctx, layout);

    void *got_vs = nullptr, *got_ps = nullptr, *got_layout = nullptr;
    d3d11_context_pipeline_snapshot(ctx, &got_vs, &got_ps, &got_layout);
    if (got_ps != shader || got_vs != vs || got_layout != layout) {
        std::printf("fail: pipeline snapshot mismatch\n"); return 12;
    }
    // Snapshot returns nulls from a null context.
    d3d11_context_pipeline_snapshot(nullptr, &got_vs, &got_ps, &got_layout);
    if (got_vs || got_ps || got_layout) { std::printf("fail: null ctx snapshot\n"); return 13; }

    // ---- vertex input capture: CreateBuffer/Map/Unmap/IASetVertexBuffers ----
    using create_buffer_t = D3DMS long (*)(void*, void*, void*);
    using map_t = D3DMS long (*)(void*, void*, u32, u32, u32, void*);
    using unmap_t = D3DMS void (*)(void*, void*, u32);
    using ia_vb_t = D3DMS void (*)(void*, u32, u32, void**, u32*, u32*);
    auto create_buffer = reinterpret_cast<create_buffer_t>(vtbl[3]);
    auto map = reinterpret_cast<map_t>(ctx_vtbl[14]);
    auto unmap = reinterpret_cast<unmap_t>(ctx_vtbl[15]);
    auto ia_set_vb = reinterpret_cast<ia_vb_t>(ctx_vtbl[18]);
    u32 buf_desc[6] = { 36, 0, 2, 0, 0, 0 };   // ByteWidth=36, BindFlags=VB
    struct MappedType { void* p0; void* p1; void* p2; } mapped{};
    void* vbuf = nullptr;
    if (create_buffer(dev, buf_desc, &vbuf) != 0 || !vbuf) { std::printf("fail: CreateBuffer\n"); return 14; }
    if (map(ctx, vbuf, 0, 0, 0, &mapped) != 0 || !mapped.p0) { std::printf("fail: Map\n"); return 15; }
    const float tri[9] = { 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, -0.5f, -0.5f, 0.0f };
    std::memcpy(mapped.p0, tri, sizeof(tri));
    unmap(ctx, vbuf, 0);
    void* bufs[1] = { vbuf };
    u32 strides[1] = { 12 }, offsets[1] = { 0 };
    ia_set_vb(ctx, 0, 1, bufs, strides, offsets);

    const u8* vdata = nullptr;
    u32 vcount = 0, vstride = 0, voffset = 0;
    d3d11_context_vertex_data(ctx, &vdata, &vcount, &vstride, &voffset);
    if (!vdata || vcount != 3 || vstride != 12 || voffset != 0) {
        std::printf("fail: vertex capture (%u, %u)\n", vcount, vstride); return 16;
    }
    float got[3];
    std::memcpy(got, vdata, sizeof(got));
    if (got[0] != 0.0f || got[1] != 0.5f) { std::printf("fail: vertex data\n"); return 17; }

    // ---- constant buffer binds (Stage 3f): PSSetConstantBuffers (16) ----
    u32 cb_desc[6] = { 256, 0, 8, 0, 0, 0 };   // ByteWidth=256, BindFlags=CB(8)
    void* cbuf = nullptr;
    if (create_buffer(dev, cb_desc, &cbuf) != 0 || !cbuf) { std::printf("fail: cb CreateBuffer\n"); return 18; }
    MappedType cm{};
    if (map(ctx, cbuf, 0, 0, 0, &cm) != 0 || !cm.p0) { std::printf("fail: cb Map\n"); return 19; }
    const float cbdata[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    std::memcpy(cm.p0, cbdata, sizeof(cbdata));
    unmap(ctx, cbuf, 0);
    using set_cb_t = D3DMS void (*)(void*, u32, u32, void**);
    void* cbbufs[1] = { cbuf };
    auto ps_set_cb = reinterpret_cast<set_cb_t>(ctx_vtbl[16]);
    ps_set_cb(ctx, 0, 1, cbbufs);
    const u8* cbdata_out = nullptr;
    u32 cbsize = 0;
    if (!d3d11_context_cbuffer(ctx, 1, 0, &cbdata_out, &cbsize) || cbsize != 256) {
        std::printf("fail: cb capture (%u)\n", cbsize); return 20;
    }
    float gotcb[4];
    std::memcpy(gotcb, cbdata_out, sizeof(gotcb));
    if (gotcb[0] != 1.0f || gotcb[3] != 1.0f) { std::printf("fail: cb data\n"); return 21; }
    if (d3d11_context_cbuffer(ctx, 1, 4, &cbdata_out, &cbsize)) { std::printf("fail: unbound cb\n"); return 22; }

    // Draw glue: with an uninitialized swapchain it must refuse cleanly.
    papaya::gpu::VulkanSwapchain no_swapchain;
    if (d3d11_context_draw_vertices(ctx, &no_swapchain)) { std::printf("fail: draw without swapchain\n"); return 23; }
    if (d3d11_context_draw_vertices(ctx, nullptr)) { std::printf("fail: draw null swapchain\n"); return 24; }

    std::printf("ok: shader + input-layout + pipeline-state through real vtables, garbage refused\n");
    return 0;
}