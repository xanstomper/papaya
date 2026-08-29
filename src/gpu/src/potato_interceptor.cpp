#include "papaya/gpu/potato_interceptor.hpp"
#include "papaya/common/logger.hpp"
#include <algorithm>

namespace papaya::gpu {

PotatoInterceptor::PotatoInterceptor(const TextureOverrideConfig& config)
    : config_(config), flat_pixel_data_(config.flat_color_rgba) {}

void PotatoInterceptor::set_config(const TextureOverrideConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    flat_pixel_data_ = config.flat_color_rgba;
    log::info("POTATO", "Updated Texture Optimization Config [Mode: {}, LOD Bias: +{:.1f}, MaxDim: {}, 1x1 Strip: {}]",
              static_cast<int>(config_.mode), config_.mip_lod_bias, config_.max_texture_dimension,
              config_.enable_1x1_flat_replacement ? "ENABLED" : "DISABLED");
}

f32 PotatoInterceptor::intercept_sampler_lod_bias(f32 original_bias, u32& max_anisotropy) {
    if (config_.mode == PerformanceMode::Native) {
        return original_bias;
    }

    stats_.samplers_biased++;
    max_anisotropy = config_.anisotropic_clamp;

    // Add positive LOD bias to force mobile GPU to sample smallest available mipmaps
    f32 biased = original_bias + config_.mip_lod_bias;
    return std::max(biased, config_.mip_lod_bias);
}

bool PotatoInterceptor::should_strip_texture(u32 width, u32 height, u32 format, u64 original_size_bytes) {
    stats_.total_textures_created++;
    stats_.original_vram_bytes += original_size_bytes;

    if (config_.mode == PerformanceMode::Native) {
        return false;
    }

    if (config_.enable_1x1_flat_replacement &&
        (width > config_.max_texture_dimension || height > config_.max_texture_dimension)) {
        stats_.textures_stripped++;
        stats_.saved_vram_bytes += (original_size_bytes > 4) ? (original_size_bytes - 4) : 0;
        return true;
    }

    return false;
}

std::span<const u8> PotatoInterceptor::get_1x1_flat_buffer() const {
    return std::span<const u8>(reinterpret_cast<const u8*>(&flat_pixel_data_), sizeof(flat_pixel_data_));
}

void PotatoInterceptor::reset_stats() {
    stats_.total_textures_created = 0;
    stats_.textures_stripped = 0;
    stats_.samplers_biased = 0;
    stats_.original_vram_bytes = 0;
    stats_.saved_vram_bytes = 0;
}

} // namespace papaya::gpu
