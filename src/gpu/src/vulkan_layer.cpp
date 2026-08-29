#include "papaya/gpu/vulkan_layer.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu {

VulkanLayerInterceptor::VulkanLayerInterceptor(
    std::shared_ptr<PotatoInterceptor> potato,
    std::shared_ptr<ShaderStripper> shader_stripper,
    std::shared_ptr<SwapchainUpscaler> upscaler
) : potato_(potato ? std::move(potato) : std::make_shared<PotatoInterceptor>()),
    shader_stripper_(shader_stripper ? std::move(shader_stripper) : std::make_shared<ShaderStripper>()),
    upscaler_(upscaler ? std::move(upscaler) : std::make_shared<SwapchainUpscaler>()) {}

VulkanLayerInterceptor::~VulkanLayerInterceptor() = default;

Result<> VulkanLayerInterceptor::initialize() {
    log::info("VK_LAYER", "Initializing Papaya Vulkan 1.3 Optimization & Potato Interceptor Layer");
    is_initialized_ = true;
    return {};
}

bool VulkanLayerInterceptor::on_create_image(u32 width, u32 height, u32 format, u64 size_bytes) {
    return potato_->should_strip_texture(width, height, format, size_bytes);
}

f32 VulkanLayerInterceptor::on_create_sampler(f32 lod_bias, u32& max_aniso) {
    return potato_->intercept_sampler_lod_bias(lod_bias, max_aniso);
}

bool VulkanLayerInterceptor::on_create_shader_module(std::span<const u32> spirv, std::vector<u32>& modified_spirv) {
    if (shader_stripper_->should_strip(spirv)) {
        ShaderType type = shader_stripper_->classify_spirv(spirv);
        modified_spirv = shader_stripper_->generate_noop_spirv(type);
        log::debug("VK_LAYER", "Stripped heavy post-processing shader and replaced with no-op module");
        return true;
    }
    return false;
}

void VulkanLayerInterceptor::on_create_swapchain(u32 req_w, u32 req_h, u32& internal_w, u32& internal_h) {
    upscaler_->intercept_swapchain_dimensions(req_w, req_h, internal_w, internal_h);
}

void VulkanLayerInterceptor::queue_async_pipeline_compilation(u64 pipeline_hash) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (compiled_pso_cache_.find(pipeline_hash) != compiled_pso_cache_.end()) {
        pipeline_stats_.cache_hits++;
        return;
    }

    pipeline_stats_.async_background_jobs++;
    compiled_pso_cache_.insert(pipeline_hash);
    pipeline_stats_.pipelines_compiled++;
}

} // namespace papaya::gpu
