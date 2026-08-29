#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"

namespace papaya::gpu {

enum class UpscalingFilter {
    IntegerScale,    // Pixel-perfect crisp nearest-neighbor integer scaling
    FastBilinear,    // Lightweight bilinear spatial filter
    SpatialFSR1      // AMD FSR 1.0 Spatial Edge-Adaptive Filtering
};

struct SwapchainScaleConfig {
    bool enable_forced_resolution{true};
    u32 internal_width{960};
    u32 internal_height{540};
    u32 display_width{1920};
    u32 display_height{1080};
    UpscalingFilter filter{UpscalingFilter::FastBilinear};
};

class SwapchainUpscaler {
public:
    explicit SwapchainUpscaler(const SwapchainScaleConfig& config = {});
    ~SwapchainUpscaler() = default;

    void set_config(const SwapchainScaleConfig& config) { config_ = config; }
    const SwapchainScaleConfig& get_config() const { return config_; }

    // Intercepts swapchain creation dimensions
    void intercept_swapchain_dimensions(u32 requested_w, u32 requested_h, u32& internal_w, u32& internal_h);

    // Calculates scaling factor
    f32 get_scale_factor_x() const;
    f32 get_scale_factor_y() const;

    // Fast software / compute bilinear pass emulator for test harness
    void upscale_frame_rgba(const u8* src_540p, u8* dst_1080p, u32 src_w, u32 src_h, u32 dst_w, u32 dst_h);

private:
    SwapchainScaleConfig config_;
};

} // namespace papaya::gpu
