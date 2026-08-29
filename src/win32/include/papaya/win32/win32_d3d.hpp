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

} // namespace papaya::win32