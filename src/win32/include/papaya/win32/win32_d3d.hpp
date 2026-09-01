#pragma once

#include "papaya/common/types.hpp"

// Minimal software/CPU D3D11 + DXGI surface for the native Win32 HLE.
//
// Hardware-free, honest scope: supports the subset of COM/vtable calls a simple
// d3d11 sample uses to present a CPU-rendered (or clear-colored) frame to its
// swapchain, which we blit into the native X11 window via the GDI present path.
// It does NOT run shaders/GPU work; unsupported calls are safe no-ops so a
// guest that drives a simple clear+present frame loop keeps running.
//
// COM model: each D3D11/DXGI object is { void** vtbl; state... }. A guest
// `obj->Method(i)` resolves to vtbl[i], which points to our ms_abi thunks that
// operate on the object. IDXGISwapChain::Present drives surface_present().

namespace papaya::win32 {

// Returns a non-null fake D3D11 device (or NULL). device / context are out-ptrs
// to heap objects with usable vtables. device_context is ID3D11DeviceContext*.
void* d3d11_create_device(void** device_out, void** context_out);

// Creates a swapchain object bound to a native window (hwnd) + a CPU backbuffer.
// Used by D3D11CreateDeviceAndSwapChain and IDXGIFactory::CreateSwapChain.
void* d3d11_create_swapchain(void* hwnd, u32 w, u32 h);

// The core: Present the swapchain's CPU backbuffer into the native window.
// success=true to actually draw/flush. Returns HRESULT (S_OK).
long d3d11_swapchain_present(void* swapchain, u32 sync_interval, u32 flags);

// GetBuffer(0) -> the swapchain's back-buffer surface object (opaque).
void* d3d11_swapchain_get_buffer(void* swapchain, u32 index);

// ClearRenderTargetView: fill the RTV's CPU framebuffer with the clear color
// (float RGBA). The RTV wraps the swapchain's backbuffer, so Present shows it.
void d3d11_clear_rtv(void* rtv, const float rgba[4]);

// ID3D11Device::CreateVertexShader / CreatePixelShader are wired through the
// real DXBC->GLSL translation layer: the shader object keeps the emitted GLSL
// (translated=false when the bytecode is outside the supported subset; the
// call still succeeds so the guest keeps running).
//
// Read back the translation result from a shader object (for pipeline-layout
// consumers and tests). Returns nullptr for null shaders.
const char* d3d11_shader_get_glsl(void* shader, bool* translated);

// SPIR-V words produced by the in-process glslang compile (nullptr when the
// build lacks glslang or the shader is outside the supported subset).
const u32* d3d11_shader_get_spirv(void* shader, u32* word_count);

// Capture the pipeline state currently bound to the context: the VS/PS
// shader objects and the input layout (for the Vulkan pipeline builder).
void d3d11_context_pipeline_snapshot(void* ctx, void** vs, void** ps, void** layout);

// Input-layout introspection (elements captured from D3D11_INPUT_ELEMENT_DESC).
u32 d3d11_input_layout_count(void* layout);
const char* d3d11_input_layout_element(void* layout, u32 i, u32* semantic_index,
                                       u32* format);

// Vertex input captured from CreateBuffer/Map/Unmap/IASetVertexBuffers.
void d3d11_context_vertex_data(void* ctx, const u8** data, u32* count, u32* stride,
                               u32* offset);

// Bound constant buffer (stage 0 = vertex, 1 = pixel, slot 0-15): returns 1
// when bound, filling data/size (the buffer content written via Map).
u32 d3d11_context_cbuffer(void* ctx, u32 stage, u32 slot, const u8** data, u32* size);

// Bound sampled texture (RGBA8) via an SRV: returns 1 + data/dimensions.
u32 d3d11_context_texture(void* ctx, u32 stage, u32 slot, const u8** data, u32* w, u32* h);

// Render the bound translated pipeline into the given VulkanSwapchain and
// present it: builds the PipelineSpec from the context snapshot (VS/PS SPIR-V,
// input layout -> vertex input, bound vertex buffer). Returns false when the
// state is incomplete (no bound/complied shaders, no vertices, swapchain not
// ready, shaders using resources) - the caller then falls back to the CPU
// blit path. No-op false when built without Vulkan.
bool d3d11_context_draw_vertices(void* ctx, void* swapchain);

} // namespace papaya::win32