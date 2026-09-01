#include "papaya/win32/win32_d3d.hpp"
#include "papaya/win32/win32_window.hpp"
#include "papaya/common/logger.hpp"
#include "papaya/gpu/shader_translator.hpp"
#include "papaya/gpu/shader_compile.hpp"
#include <cstring>
#include <cstdlib>
#include <string>

#ifdef PAPAYA_HAS_VULKAN
#include "papaya/gpu/vulkan_swapchain.hpp"
#include "papaya/gpu/pipeline_map.hpp"
#include "papaya/gpu/vulkan_pipeline.hpp"
#include <mutex>
#endif

// Assigning to a function pointer with a conversion from void* would be a
// pedantic warning; we build vtables of typed ms_abi function pointers.
namespace papaya::win32 {

namespace {
struct D3DDevice;
struct D3DContext;
struct SwapChain;
struct RenderTargetView;
struct D3DShader;
struct D3DInputLayout;

using msabi_void6 = u64 (*)(void*, u64, u64, u64, u64, u64);
using msabi_void4 = u64 (*)(void*, u64, u64, u64);
using msabi_void3 = u64 (*)(void*, u64, u64);
using msabi_void2 = u64 (*)(void*, u64);
using msabi_void1 = u64 (*)(void*);
using msabi_void0 = u64 (*)();

// ---- COM IUnknown (QueryInterface/AddRef/Release) shared thunks --------------
// All D3D/DXGI vtable thunks use the Microsoft x64 calling convention (ms_abi),
// because the guest invokes COM methods via the vtable with ms_abi semantics.
#define D3DMS __attribute__((ms_abi))

static D3DMS u64 thunk_query_interface(void* self, u64 riid, u64 ppv) {
    (void)self; (void)riid;
    if (ppv) *reinterpret_cast<void**>(ppv) = self;
    return 0; // S_OK
}
static D3DMS u64 thunk_add_ref(void*) { return 1; }
static D3DMS u64 thunk_release(void*) { return 0; }

struct D3DObject {
    void** vtbl;
};

// The real state our objects carry; vtbl[0..2] are the IUnknown thunks.
struct D3DDevice : D3DObject {
    D3DContext* ctx{nullptr};
};
struct D3DInputLayoutElement {
    u32 format{0};          // DXGI_FORMAT (e.g. 6 = R32G32B32_FLOAT)
    u32 semantic_index{0};
    u32 slot{0};
    u32 offset{0};
    u32 step_class{0};      // D3D11_INPUT_PER_VERTEX_DATA / _PER_INSTANCE_DATA
    u32 step_rate{0};
    std::string semantic;
};
struct D3DInputLayout : D3DObject {
    std::vector<D3DInputLayoutElement> elements;
};
struct D3D11Buffer : D3DObject {
    u32 size{0};                // D3D11_BUFFER_DESC.ByteWidth
    u32 stride_bytes{0};        // StructureByteStride (0 for raw usage)
    std::vector<u8> data;       // host storage written through Map/Unmap
};
struct D3DContext : D3DObject {
    // last render targets for ClearRenderTargetView
    void* rtv{nullptr};
    float clear_color[4]{0,0,0,0};
    // pipeline state captured for the future Vulkan pipeline builder
    D3DShader* vs{nullptr};
    D3DShader* ps{nullptr};
    D3DInputLayout* input_layout{nullptr};
    // vertex input captured from CreateBuffer/Map/Unmap/IASetVertexBuffers
    D3D11Buffer* vertex_buffer{nullptr};
    u32 vertex_stride{0};
    u32 vertex_offset{0};
};
struct SwapChain : D3DObject {
    void* hwnd{nullptr};
    u32 w{320}, h{240};
    u8* fb{nullptr};       // CPU backbuffer (RGBA)
    u32  fb_size{0};
    RenderTargetView* backbuffer_rtv{nullptr};
};
struct RenderTargetView : D3DObject {
    SwapChain* swap{nullptr};
};
struct D3DShader : D3DObject {
    std::string glsl;      // emitted GLSL from the DXBC->GLSL translation
    bool translated{false}; // false when outside the supported emission subset
    std::vector<u32> spirv;  // in-process GLSL->SPIR-V when glslang is linked
    bool compiled{false};    // spirv ready for the Vulkan pipeline builder
};

D3DDevice*  g_device  = nullptr;
D3DContext* g_context = nullptr;
SwapChain*  g_swap    = nullptr;

static D3DMS u64 dev_create_shader_impl(void* self, u64 code, u64 len, u64 linkage,
        u64 out, u32 stage) {
    (void)self; (void)linkage;
    auto* shader = new D3DShader();
    if (code && len) {
        const auto* bytes = reinterpret_cast<const u8*>(code);
        shader->translated = papaya::gpu::dxbc_to_glsl_stage({bytes, len}, stage, shader->glsl);
        if (shader->translated) {
            std::string err;
            shader->compiled =
                    papaya::gpu::compile_glsl_to_spirv(shader->glsl, stage, shader->spirv, err);
        }
    }
    if (out) *reinterpret_cast<void**>(out) = shader;
    return 0; // S_OK
}
static D3DMS u64 dev_create_vertex_shader(void* self, u64 code, u64 len, u64 linkage, u64 out) {
    return dev_create_shader_impl(self, code, len, linkage, out, papaya::gpu::kStageVertex);
}
static D3DMS u64 dev_create_pixel_shader(void* self, u64 code, u64 len, u64 linkage, u64 out) {
    return dev_create_shader_impl(self, code, len, linkage, out, papaya::gpu::kStageFragment);
}

static D3DMS u64 dev_create_buffer(void* self, u64 desc, u64 out) {
    (void)self;
    auto* buf = new D3D11Buffer();
    const u8* p = reinterpret_cast<const u8*>(desc);
    if (p) {
        u32 fields[6];
        std::memcpy(fields, p, sizeof(fields));   // D3D11_BUFFER_DESC
        buf->size = fields[0];
        buf->stride_bytes = fields[5];
        buf->data.assign(buf->size, 0);
    }
    if (out) *reinterpret_cast<void**>(out) = buf;
    return 0; // S_OK
}

static D3DMS u64 dev_create_input_layout(void* self, u64 descs, u64 num_elements,
        u64 shader_code, u64 code_len, u64 out) {
    (void)self; (void)shader_code; (void)code_len;
    auto* layout = new D3DInputLayout();
    const u8* p = reinterpret_cast<const u8*>(descs);
    for (u64 i = 0; i < num_elements && p; ++i) {
        // D3D11_INPUT_ELEMENT_DESC (x64): LPCSTR SemanticName(8) + 6 x UINT.
        D3DInputLayoutElement e;
        const char* sem = *reinterpret_cast<const char* const*>(p);
        u32 v[6];
        std::memcpy(v, p + 8, sizeof(v));
        if (sem) { std::string s(sem); if (s.size() < 256) e.semantic = std::move(s); }
        e.semantic_index = v[0];
        e.format = v[1];
        e.slot = v[2];
        e.offset = v[3];
        e.step_class = v[4];
        e.step_rate = v[5];
        layout->elements.push_back(std::move(e));
        p += 32;
    }
    if (out) *reinterpret_cast<void**>(out) = layout;
    return 0; // S_OK
}

static D3DMS u64 ctx_ps_set_shader(void* self, u64 shader, u64 instances, u64 count) {
    (void)instances; (void)count;
    static_cast<D3DContext*>(self)->ps = static_cast<D3DShader*>(reinterpret_cast<void*>(shader));
    return 0;
}
static D3DMS u64 ctx_vs_set_shader(void* self, u64 shader, u64 instances, u64 count) {
    (void)instances; (void)count;
    static_cast<D3DContext*>(self)->vs = static_cast<D3DShader*>(reinterpret_cast<void*>(shader));
    return 0;
}
static D3DMS u64 ctx_map(void* self, u64 resource, u64 subresource, u64 type,
        u64 flags, u64 mapped_out) {
    (void)self; (void)subresource; (void)type; (void)flags;
    auto* buf = static_cast<D3D11Buffer*>(reinterpret_cast<void*>(resource));
    if (!buf || !mapped_out) return 0x887A0005;   // DXGI_ERROR_INVALID_CALL
    // D3D11_MAPPED_SUBRESOURCE: { pData, RowPitch, DepthPitch } (3 pointers).
    auto* pdata = reinterpret_cast<void**>(mapped_out);
    pdata[0] = buf->data.data();
    pdata[1] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(buf->size));
    pdata[2] = pdata[1];
    return 0; // S_OK
}
static D3DMS u64 ctx_unmap(void* self, u64 resource, u64 subresource) {
    (void)self; (void)resource; (void)subresource;
    return 0;
}
static D3DMS u64 ctx_ia_set_vertex_buffers(void* self, u64 start_slot, u64 num,
        u64 buffers_ptr, u64 strides_ptr, u64 offsets_ptr) {
    auto* ctx = static_cast<D3DContext*>(self);
    if (start_slot == 0 && num >= 1 && buffers_ptr) {
        auto** bufs = reinterpret_cast<void**>(buffers_ptr);
        const u32* strides = strides_ptr ? reinterpret_cast<const u32*>(strides_ptr) : nullptr;
        const u32* offsets = offsets_ptr ? reinterpret_cast<const u32*>(offsets_ptr) : nullptr;
        ctx->vertex_buffer = static_cast<D3D11Buffer*>(bufs[0]);
        ctx->vertex_stride = strides ? strides[0] : 0;
        ctx->vertex_offset = offsets ? offsets[0] : 0;
    } else if (num == 0 || (buffers_ptr && !*reinterpret_cast<void**>(buffers_ptr))) {
        ctx->vertex_buffer = nullptr;
    }
    return 0;
}
static D3DMS u64 ctx_ia_set_input_layout(void* self, u64 layout) {
    static_cast<D3DContext*>(self)->input_layout =
            static_cast<D3DInputLayout*>(reinterpret_cast<void*>(layout));
    return 0;
}
static D3DMS u64 ctx_draw(void* self, u64 count, u64 start) { (void)self;(void)count;(void)start; return 0; }
static D3DMS u64 ctx_draw_indexed(void* self, u64 count, u64 start, u64 base) {
    (void)self;(void)count;(void)start;(void)base; return 0;
}

// ---- ID3D11Device methods ----------------------------------------------------
static D3DMS u64 dev_create_render_target_view(void* self, void* res, void* desc, void* rtv_out) {
    (void)res; (void)desc;
    auto* dev = static_cast<D3DDevice*>(self);
    auto* rtv = new RenderTargetView();
    // The RTV wraps the swapchain's backbuffer surface (the resource passed in).
    rtv->swap = nullptr;
    if (rtv_out) *reinterpret_cast<void**>(rtv_out) = rtv;
    (void)dev;
    return 0;
}

// ---- ID3D11DeviceContext methods --------------------------------------------
static D3DMS u64 ctx_om_set_render_targets(void* self, u64 count, void* rtv_ptr, void* dsv) {
    (void)count; (void)dsv;
    auto* ctx = static_cast<D3DContext*>(self);
    ctx->rtv = rtv_ptr ? *reinterpret_cast<void**>(rtv_ptr) : nullptr;
    return 0;
}
static D3DMS u64 ctx_clear_render_target_view(void* self, void* rtv, const float* rgba) {
    auto* ctx = static_cast<D3DContext*>(self);
    auto* target = (rtv) ? static_cast<RenderTargetView*>(rtv) : nullptr;
    ctx->rtv = rtv;
    if (!rgba) return 0;
    ctx->clear_color[0]=rgba[0]; ctx->clear_color[1]=rgba[1];
    ctx->clear_color[2]=rgba[2]; ctx->clear_color[3]=rgba[3];
    SwapChain* sc = g_swap;
    if (!sc || !sc->fb || !target) return 0;
    u32 n = sc->w * sc->h;
    u8 r=(u8)(rgba[0]*255), g2=(u8)(rgba[1]*255), b=(u8)(rgba[2]*255), a=(u8)(rgba[3]*255);
    u8* p = sc->fb;
    for (u32 i=0;i<n;i++){ p[0]=r; p[1]=g2; p[2]=b; p[3]=a; p+=4; }
    return 0;
}
static D3DMS u64 ctx_rsset_viewports(void* self, u64 count, void* vp) { (void)self;(void)count;(void)vp; return 0; }
static D3DMS u64 ctx_flush(void* self) { (void)self; return 0; }
static D3DMS u64 ctx_clear_state(void* self) { (void)self; return 0; }
// draw/shader set calls are no-ops (software surface doesn't run shaders)
static D3DMS u64 ctx_noop(void*, u64, u64, u64) { return 0; }

// ---- IDXGISwapChain methods -------------------------------------------------
// Build the PipelineSpec from the captured context state and render it via
// the swapchain's GPU path. Honest: only runs when the bound VS/PS are both
// translated+compiled and a vertex buffer is bound with a nonzero stride;
// shaders that use resources (cb/samplers) come back false -> CPU blit.
bool build_context_pipeline_spec(D3DContext* ctx, papaya::gpu::PipelineSpec& spec,
                                 const u8** verts, u32* stride, u32* count) {
    if (!ctx || !ctx->vs || !ctx->ps || !ctx->input_layout || !ctx->vertex_buffer ||
        ctx->vertex_stride == 0 || !ctx->vs->compiled || !ctx->ps->compiled)
        return false;
    if (!ctx->vs->spirv.empty() && !ctx->ps->spirv.empty()) {
        spec.vs_spirv = ctx->vs->spirv;
        spec.ps_spirv = ctx->ps->spirv;
    } else {
        return false;
    }
    std::vector<papaya::gpu::D3d11InputElement> elements;
    for (const auto& e : ctx->input_layout->elements) {
        papaya::gpu::D3d11InputElement el;
        el.semantic = e.semantic;
        el.semantic_index = e.semantic_index;
        el.dxgi_format = e.format;
        el.input_slot = e.slot;
        el.aligned_offset = e.offset;
        el.step_class = e.step_class;
        el.step_rate = e.step_rate;
        elements.push_back(std::move(el));
    }
    if (!papaya::gpu::build_vertex_input(elements, spec.vertex_bindings,
                                         spec.vertex_attributes))
        return false;
    // D3D11 fetches with the IASetVertexBuffers stride; honor it over the
    // layout-derived stride.
    for (auto& b : spec.vertex_bindings)
        if (b.binding == 0) b.stride = ctx->vertex_stride;
    spec.descriptors.clear();   // resource binds land in Stage 3f
    *verts = ctx->vertex_buffer->data.data();
    *stride = ctx->vertex_stride;
    *count = static_cast<u32>(ctx->vertex_buffer->size / ctx->vertex_stride);
    return *count > 0;
}

static D3DMS u64 sc_get_buffer(void* self, u64 index, u64 riid, void* bufp) {
    auto* sc = static_cast<SwapChain*>(self);
    if (index != 0) return 0x887A0022; // DXGI_ERROR_INVALID_CALL
    if (!sc->backbuffer_rtv) {
        sc->backbuffer_rtv = new RenderTargetView();
        sc->backbuffer_rtv->swap = sc;
    }
    if (bufp) *reinterpret_cast<void**>(bufp) = sc->backbuffer_rtv;
    (void)riid;
    return 0; // S_OK
}
static D3DMS u64 sc_present(void* self, u64 sync, u64 flags) {
    auto* sc = static_cast<SwapChain*>(self);
    (void)sync; (void)flags;
#ifdef PAPAYA_HAS_VULKAN
    // Opt-in real Vulkan present (PAPAYA_VULKAN=1): upload the CPU-swrast
    // backbuffer into a Vulkan swapchain image and present it via the host GPU.
    // Falls back to the X11 software blit below if Vulkan is unavailable.
    static papaya::gpu::VulkanSwapchain g_vk;
    static int vk_enabled = []() -> int { const char* e = getenv("PAPAYA_VULKAN"); return e && *e == '1'; }();
    if (vk_enabled && sc->hwnd) {
        if (!g_vk.is_ready()) {
            auto* wm = &window_manager();
            auto* w = wm->window_from_hwnd(sc->hwnd);
            if (w) {
                _XDisplay* disp = wm->display();
                auto xwin = wm->xwindow_of(sc->hwnd);
                g_vk.initialize(disp, xwin, sc->w, sc->h);
            }
        }
        // GPU pipeline path: if the context has a fully translated VS+PS and
        // bound vertices, render them into the swapchain and present.
        bool gpu_rendered = false;
        papaya::gpu::PipelineSpec spec;
        const u8* verts = nullptr;
        u32 stride = 0, count = 0;
        if (g_vk.is_ready() && build_context_pipeline_spec(g_context, spec, &verts, &stride, &count))
            gpu_rendered = g_vk.render_and_present(spec, verts, stride, count, nullptr, 0);
        if (gpu_rendered) return 0;
        if (g_vk.is_ready() && sc->fb) {
            u32 idx = g_vk.acquire();
            if (idx != 0xFFFFFFFFu && g_vk.upload_rgba(sc->fb, sc->w, sc->h)) {
                g_vk.present(idx);
                return 0;
            }
        }
    }
#endif
    // Present = upload the swapchain framebuffer to the native window with no
    // redundant per-frame memcpy: the guest writes into the same buffer the
    // window presents (fb is shared with the window's surface on first use).
    if (sc->hwnd && sc->fb) {
        auto* wm = &window_manager();
        // Ensure the window surface matches the swapchain size and share its
        // buffer so XPutImage uploads the guest's pixels directly (zero per-copy).
        u8* wfb = wm->surface_buffer(sc->hwnd, sc->w, sc->h);
        if (wfb && wfb == sc->fb) {
            // buffers already share storage -> upload directly, no copy
            wm->surface_present(sc->hwnd);
        } else if (wfb) {
            // first present: adopt the window surface as our backbuffer so all
            // later presents are zero-copy.
            sc->fb = wfb;
            sc->fb_size = static_cast<u32>(sc->w) * static_cast<u32>(sc->h) * 4;
            wm->surface_present(sc->hwnd);
        }
    }
    return 0; // S_OK
}
static D3DMS u64 sc_set_fullscreen(void* self, u64 b, void*) { (void)self;(void)b; return 0; }
static D3DMS u64 sc_get_desc(void* self, u64 out_desc) {
    auto* sc = static_cast<SwapChain*>(self);
    if (out_desc) { // DXGI_SWAP_CHAIN_DESC: Width=0 Height=4 OutputWindow=8...
        auto* d = reinterpret_cast<u32*>(out_desc);
        d[0] = sc->w;   // BufferDesc.Width
        d[1] = sc->h;   // BufferDesc.Height
    }
    return 0;
}
static D3DMS u64 sc_resize_buffers(void* self, u64 count, u64 w, u64 h, u64 fmt, u64 flags_) {
    auto* sc = static_cast<SwapChain*>(self);
    (void)count; (void)fmt; (void)flags_;
    sc->w = (u32)w; sc->h = (u32)h;
    u32 need = sc->w * sc->h * 4;
    if (sc->fb) sc->fb = static_cast<u8*>(realloc(sc->fb, need));
    else sc->fb = static_cast<u8*>(calloc(need,1));
    sc->fb_size = need;
    return 0;
}

// ---- vtable builders --------------------------------------------------------
// ID3D11Device: IUnknown(0,1,2) + CreateBuffer(3)... CreateRenderTargetView is #9.
void* build_device_vtbl() {
    auto* v = static_cast<void**>(calloc(64, sizeof(void*)));
    v[0]=reinterpret_cast<void*>(&thunk_query_interface);
    v[1]=reinterpret_cast<void*>(&thunk_add_ref);
    v[2]=reinterpret_cast<void*>(&thunk_release);
    v[3]=reinterpret_cast<void*>(&dev_create_buffer);          // CreateBuffer
    v[9]=reinterpret_cast<void*>(&dev_create_render_target_view);
    v[11]=reinterpret_cast<void*>(&dev_create_input_layout);   // CreateInputLayout
    v[12]=reinterpret_cast<void*>(&dev_create_vertex_shader);   // CreateVertexShader
    v[15]=reinterpret_cast<void*>(&dev_create_pixel_shader);    // CreatePixelShader
    // all other slots default null; guest calling them would fault, so fill no-ops
    return v;
}
// ID3D11DeviceContext vtable (authoritative order from wine d3d11.idl, matches
// the Windows SDK): IUnknown 0-2, ID3D11DeviceChild 3-6 (GetDevice, GetPrivate-
// Data, SetPrivateData, SetPrivateDataInterface), then the context methods:
// 9=PSSetShader, 11=VSSetShader, 12=DrawIndexed, 13=Draw, 17=IASetInputLayout,
// 18=IASetVertexBuffers, 33=OMSetRenderTargets, 43=RSSetViewports,
// 49=ClearRenderTargetView, 111=ClearState, 112=Flush.
void* build_context_vtbl() {
    auto* v = static_cast<void**>(calloc(128, sizeof(void*)));
    v[0]=reinterpret_cast<void*>(&thunk_query_interface);
    v[1]=reinterpret_cast<void*>(&thunk_add_ref);
    v[2]=reinterpret_cast<void*>(&thunk_release);
    v[9]=reinterpret_cast<void*>(&ctx_ps_set_shader);
    v[11]=reinterpret_cast<void*>(&ctx_vs_set_shader);
    v[12]=reinterpret_cast<void*>(&ctx_draw_indexed);
    v[13]=reinterpret_cast<void*>(&ctx_draw);
    v[14]=reinterpret_cast<void*>(&ctx_map);
    v[15]=reinterpret_cast<void*>(&ctx_unmap);
    v[17]=reinterpret_cast<void*>(&ctx_ia_set_input_layout);
    v[18]=reinterpret_cast<void*>(&ctx_ia_set_vertex_buffers);
    v[33]=reinterpret_cast<void*>(&ctx_om_set_render_targets);
    v[43]=reinterpret_cast<void*>(&ctx_rsset_viewports);
    v[49]=reinterpret_cast<void*>(&ctx_clear_render_target_view);
    v[111]=reinterpret_cast<void*>(&ctx_clear_state);
    v[112]=reinterpret_cast<void*>(&ctx_flush);
    return v;
}
// IDXGISwapChain (authoritative order from wine dxgi.idl): IUnknown 0-2,
// IDXGIObject 3-6 (private-data + GetParent), IDXGIDeviceSubObject 7=GetDevice,
// 8=Present, 9=GetBuffer, 10=SetFullscreenState, 11=GetFullscreenState,
// 12=GetDesc, 13=ResizeBuffers.
void* build_swapchain_vtbl() {
    auto* v = static_cast<void**>(calloc(32, sizeof(void*)));
    v[0]=reinterpret_cast<void*>(&thunk_query_interface);
    v[1]=reinterpret_cast<void*>(&thunk_add_ref);
    v[2]=reinterpret_cast<void*>(&thunk_release);
    v[8]=reinterpret_cast<void*>(&sc_present);
    v[9]=reinterpret_cast<void*>(&sc_get_buffer);
    v[10]=reinterpret_cast<void*>(&sc_set_fullscreen);
    v[12]=reinterpret_cast<void*>(&sc_get_desc);
    v[13]=reinterpret_cast<void*>(&sc_resize_buffers);
    return v;
}
// RTV: IUnknown only (opaque; ClearRenderTargetView is on the context).
void* build_rtv_vtbl() {
    auto* v = static_cast<void**>(calloc(16, sizeof(void*)));
    v[0]=reinterpret_cast<void*>(&thunk_query_interface);
    v[1]=reinterpret_cast<void*>(&thunk_add_ref);
    v[2]=reinterpret_cast<void*>(&thunk_release);
    return v;
}
} // namespace

// ---- pubic entries ----------------------------------------------------------
void* d3d11_create_device(void** device_out, void** context_out) {
    auto* dev = new D3DDevice();
    dev->vtbl = static_cast<void**>(build_device_vtbl());
    auto* ctx = new D3DContext();
    ctx->vtbl = static_cast<void**>(build_context_vtbl());
    dev->ctx = ctx;
    g_device = dev; g_context = ctx;
    if (device_out) *device_out = dev;
    if (context_out) *context_out = ctx;
    return dev;
}

void* d3d11_create_swapchain(void* hwnd, u32 w, u32 h) {
    auto* sc = new SwapChain();
    sc->vtbl = static_cast<void**>(build_swapchain_vtbl());
    sc->hwnd = hwnd; sc->w = w; sc->h = h;
    sc->fb = static_cast<u8*>(calloc(w*h*4,1));
    sc->fb_size = w*h*4;
    g_swap = sc;
    return sc;
}

long d3d11_swapchain_present(void* swapchain, u32 sync_interval, u32 flags) {
    auto* sc = static_cast<SwapChain*>(swapchain);
    if (!sc || !sc->vtbl) return 0x887A0005; // DXGI_ERROR_INVALID_CALL
    return (long)sc_present(sc, sync_interval, flags);
}

void* d3d11_swapchain_get_buffer(void* swapchain, u32 index) {
    auto* sc = static_cast<SwapChain*>(swapchain);
    void* b = nullptr;
    sc_get_buffer(sc, index, 0, &b);
    return b;
}

void d3d11_clear_rtv(void* rtv, const float rgba[4]) {
    ctx_clear_render_target_view(g_context, rtv, rgba);
}

void d3d11_context_pipeline_snapshot(void* ctx, void** vs, void** ps, void** layout) {
    auto* c = static_cast<D3DContext*>(ctx);
    if (vs) *vs = c ? c->vs : nullptr;
    if (ps) *ps = c ? c->ps : nullptr;
    if (layout) *layout = c ? c->input_layout : nullptr;
}

u32 d3d11_input_layout_count(void* layout) {
    auto* l = static_cast<D3DInputLayout*>(layout);
    return l ? static_cast<u32>(l->elements.size()) : 0;
}

// Returns the semantic name; fills semantic_index/format when non-null.
const char* d3d11_input_layout_element(void* layout, u32 i, u32* semantic_index, u32* format) {
    auto* l = static_cast<D3DInputLayout*>(layout);
    if (!l || i >= l->elements.size()) return nullptr;
    if (semantic_index) *semantic_index = l->elements[i].semantic_index;
    if (format) *format = l->elements[i].format;
    return l->elements[i].semantic.c_str();
}

// SPIR-V words produced by the glslang-backed compile (nullptr when the host
// build lacks glslang or the shader is outside the supported subset).
const u32* d3d11_shader_get_spirv(void* shader, u32* word_count) {
    auto* s = static_cast<D3DShader*>(shader);
    if (!s || !s->compiled) {
        if (word_count) *word_count = 0;
        return nullptr;
    }
    if (word_count) *word_count = static_cast<u32>(s->spirv.size());
    return s->spirv.data();
}

void d3d11_context_vertex_data(void* ctx_handle, const u8** data, u32* count,
                               u32* stride, u32* offset) {
    auto* ctx = static_cast<D3DContext*>(ctx_handle);
    if (data) *data = (ctx && ctx->vertex_buffer) ? ctx->vertex_buffer->data.data() : nullptr;
    if (count) *count = (ctx && ctx->vertex_buffer)
                                ? static_cast<u32>(ctx->vertex_buffer->size / (ctx->vertex_stride ? ctx->vertex_stride : 1)) : 0;
    if (stride) *stride = ctx ? ctx->vertex_stride : 0;
    if (offset) *offset = ctx ? ctx->vertex_offset : 0;
}

#ifdef PAPAYA_HAS_VULKAN
bool d3d11_context_draw_vertices(void* ctx_handle, void* swapchain_handle) {
    auto* ctx = static_cast<D3DContext*>(ctx_handle);
    auto* sc = static_cast<papaya::gpu::VulkanSwapchain*>(swapchain_handle);
    papaya::gpu::PipelineSpec spec;
    const u8* verts = nullptr;
    u32 stride = 0, count = 0;
    if (!sc || !build_context_pipeline_spec(ctx, spec, &verts, &stride, &count)) return false;
    return sc->render_and_present(spec, verts, stride, count, nullptr, 0);
}
#else
bool d3d11_context_draw_vertices(void*, void*) { return false; }
#endif

const char* d3d11_shader_get_glsl(void* shader, bool* translated) {
    auto* s = static_cast<D3DShader*>(shader);
    if (!s) {
        if (translated) *translated = false;
        return nullptr;
    }
    if (translated) *translated = s->translated;
    return s->glsl.c_str();
}

} // namespace papaya::win32