#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/gpu/potato_interceptor.hpp"
#include "papaya/gpu/shader_stripper.hpp"
#include "papaya/gpu/swapchain_upscaler.hpp"
#include <memory>
#include <mutex>
#include <unordered_set>
#include <atomic>

namespace papaya::gpu {

struct VulkanPipelineCacheStats {
    std::atomic<u64> pipelines_compiled{0};
    std::atomic<u64> async_background_jobs{0};
    std::atomic<u64> cache_hits{0};
};

class VulkanLayerInterceptor {
public:
    VulkanLayerInterceptor(
        std::shared_ptr<PotatoInterceptor> potato,
        std::shared_ptr<ShaderStripper> shader_stripper,
        std::shared_ptr<SwapchainUpscaler> upscaler
    );
    ~VulkanLayerInterceptor();

    Result<> initialize();

    PotatoInterceptor& get_potato() { return *potato_; }
    ShaderStripper& get_shader_stripper() { return *shader_stripper_; }
    SwapchainUpscaler& get_upscaler() { return *upscaler_; }
    const VulkanPipelineCacheStats& get_pipeline_stats() const { return pipeline_stats_; }

    // Intercept image creation
    bool on_create_image(u32 width, u32 height, u32 format, u64 size_bytes);

    // Intercept sampler creation
    f32 on_create_sampler(f32 lod_bias, u32& max_aniso);

    // Intercept shader module
    bool on_create_shader_module(std::span<const u32> spirv, std::vector<u32>& modified_spirv);

    // Intercept swapchain creation
    void on_create_swapchain(u32 req_w, u32 req_h, u32& internal_w, u32& internal_h);

    // Async pipeline compilation dispatch (VK_EXT_graphics_pipeline_library)
    void queue_async_pipeline_compilation(u64 pipeline_hash);

private:
    std::shared_ptr<PotatoInterceptor> potato_;
    std::shared_ptr<ShaderStripper> shader_stripper_;
    std::shared_ptr<SwapchainUpscaler> upscaler_;

    VulkanPipelineCacheStats pipeline_stats_;
    std::unordered_set<u64> compiled_pso_cache_;
    std::mutex cache_mutex_;
    bool is_initialized_{false};
};

} // namespace papaya::gpu
