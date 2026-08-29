#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <string_view>
#include <atomic>

namespace papaya::gpu {

enum class ShaderType {
    Unknown,
    Vertex,
    Fragment,
    Compute,
    PostProcessSSAO,
    PostProcessVolumetricFog,
    PostProcessMotionBlur,
    PostProcessBloom
};

struct ShaderStripperConfig {
    bool strip_compute_postprocess{true};
    bool strip_motion_blur{true};
    bool strip_volumetric_fog{true};
    bool strip_ssao{true};
};

class ShaderStripper {
public:
    explicit ShaderStripper(const ShaderStripperConfig& config = {});
    ~ShaderStripper() = default;

    // Analyzes SPIR-V bytecode (array of 32-bit words)
    ShaderType classify_spirv(std::span<const u32> spirv_code) const;

    // Checks if shader should be stripped and replaced with passthrough/no-op
    bool should_strip(std::span<const u32> spirv_code) const;

    // Generates a minimal no-op / passthrough SPIR-V bytecode module
    std::vector<u32> generate_noop_spirv(ShaderType type) const;

    u64 get_stripped_shader_count() const { return stripped_shader_count_.load(); }

private:
    ShaderStripperConfig config_;
    mutable std::atomic<u64> stripped_shader_count_{0};
};

} // namespace papaya::gpu
