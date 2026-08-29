#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>

namespace papaya::gpu {

struct TextureOverrideConfig {
    PerformanceMode mode{PerformanceMode::PotatoMode};
    f32 mip_lod_bias{3.0f};           // Clamps mipmap level to +3.0 / +4.0
    u32 max_texture_dimension{256};    // If texture > 256x256, strip or replace
    bool enable_1x1_flat_replacement{true}; // Replaces heavy 4K textures with 1x1 flat color
    u32 flat_color_rgba{0x808080FF};   // Flat gray (128,128,128,255)
    u32 anisotropic_clamp{0};          // Disable anisotropic filtering (0x)
};

struct TextureStats {
    std::atomic<u64> total_textures_created{0};
    std::atomic<u64> textures_stripped{0};
    std::atomic<u64> samplers_biased{0};
    std::atomic<u64> original_vram_bytes{0};
    std::atomic<u64> saved_vram_bytes{0};
};

class PotatoInterceptor {
public:
    explicit PotatoInterceptor(const TextureOverrideConfig& config = {});
    ~PotatoInterceptor() = default;

    void set_config(const TextureOverrideConfig& config);
    const TextureOverrideConfig& get_config() const { return config_; }
    const TextureStats& get_stats() const { return stats_; }

    // Intercepts Vulkan / DXVK sampler creation and modifies mipLodBias
    f32 intercept_sampler_lod_bias(f32 original_bias, u32& max_anisotropy);

    // Intercepts texture image creation (vkCreateImage / CreateTexture2D)
    bool should_strip_texture(u32 width, u32 height, u32 format, u64 original_size_bytes);

    // Returns a 1x1 flat RGBA pixel buffer to replace the texture payload
    std::span<const u8> get_1x1_flat_buffer() const;

    // Resets counters
    void reset_stats();

private:
    TextureOverrideConfig config_;
    TextureStats stats_;
    u32 flat_pixel_data_{0x808080FF};
    mutable std::mutex mutex_;
};

} // namespace papaya::gpu
