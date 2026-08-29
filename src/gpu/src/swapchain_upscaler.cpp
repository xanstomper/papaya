#include "papaya/gpu/swapchain_upscaler.hpp"
#include "papaya/common/logger.hpp"
#include <algorithm>

namespace papaya::gpu {

SwapchainUpscaler::SwapchainUpscaler(const SwapchainScaleConfig& config)
    : config_(config) {}

void SwapchainUpscaler::intercept_swapchain_dimensions(
    u32 requested_w,
    u32 requested_h,
    u32& internal_w,
    u32& internal_h
) {
    config_.display_width = requested_w;
    config_.display_height = requested_h;

    if (config_.enable_forced_resolution) {
        internal_w = config_.internal_width;
        internal_h = config_.internal_height;
        log::info("UPSCALER", "Forcing Internal Render Target: {}x{} -> Present Target: {}x{} ({:.1f}x reduction)",
                  internal_w, internal_h, requested_w, requested_h,
                  static_cast<f32>(requested_w * requested_h) / static_cast<f32>(internal_w * internal_h));
    } else {
        internal_w = requested_w;
        internal_h = requested_h;
    }
}

f32 SwapchainUpscaler::get_scale_factor_x() const {
    if (config_.internal_width == 0) return 1.0f;
    return static_cast<f32>(config_.display_width) / static_cast<f32>(config_.internal_width);
}

f32 SwapchainUpscaler::get_scale_factor_y() const {
    if (config_.internal_height == 0) return 1.0f;
    return static_cast<f32>(config_.display_height) / static_cast<f32>(config_.internal_height);
}

void SwapchainUpscaler::upscale_frame_rgba(
    const u8* src_540p,
    u8* dst_1080p,
    u32 src_w,
    u32 src_h,
    u32 dst_w,
    u32 dst_h
) {
    if (!src_540p || !dst_1080p || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;

    // Fast nearest-neighbor / bilinear scaling
    for (u32 y = 0; y < dst_h; ++y) {
        u32 src_y = std::min((y * src_h) / dst_h, src_h - 1);
        for (u32 x = 0; x < dst_w; ++x) {
            u32 src_x = std::min((x * src_w) / dst_w, src_w - 1);

            size_t src_idx = (src_y * src_w + src_x) * 4;
            size_t dst_idx = (y * dst_w + x) * 4;

            dst_1080p[dst_idx + 0] = src_540p[src_idx + 0];
            dst_1080p[dst_idx + 1] = src_540p[src_idx + 1];
            dst_1080p[dst_idx + 2] = src_540p[src_idx + 2];
            dst_1080p[dst_idx + 3] = src_540p[src_idx + 3];
        }
    }
}

} // namespace papaya::gpu
