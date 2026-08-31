#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <memory>
#include <string>
#include <vector>

struct _XDisplay;

namespace papaya::gpu {

// Real Vulkan swapchain backend — the foundation of papaya's D3D -> Vulkan
// translation layer. Owns VkInstance/VkDevice/surface/swapchain and exposes
// acquire/present; papaya's D3D11 layer maps its IDXGISwapChain::Present onto
// this (opt-in via PAPAYA_VULKAN=1), with the CPU swrast path as fallback.
//
// Honest scope: this module provides the *presentation* half of the D3D->Vulkan
// bridge (device + swapchain + image acquire/present + blit upload). Shader
// translation (DXBC/HLSL -> SPIR-V) is the next, much larger phase.
class VulkanSwapchain {
public:
    VulkanSwapchain();
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    // Create instance/device/surface/swapchain for an X11 window.
    // Returns an error (and leaves the object unusable) when no Vulkan loader,
    // ICD or required queue/surface support is present — callers then fall back
    // to the CPU swrast present path.
    Result<> initialize(_XDisplay* display, std::uint64_t xwindow, u32 width, u32 height);

    void shutdown();

    bool is_ready() const { return ready_; }

    // Acquire the next swapchain image and return its index (UINT32_MAX on error).
    u32 acquire();

    // Present the current image. Returns VK-style result (0 == success).
    u32 present(u32 image_index);

    // Copy an RGBA8 CPU buffer into the acquired swapchain image.
    bool upload_rgba(const u8* rgba, u32 width, u32 height);

    // Diagnostics.
    const std::string& last_error() const { return last_error_; }
    const std::string& gpu_name() const { return gpu_name_; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool   ready_{false};
    u32    width_{0};
    u32    height_{0};
    u32    last_image_{0xFFFFFFFFu};
    std::string last_error_;
    std::string gpu_name_{"(none)"};
};

} // namespace papaya::gpu