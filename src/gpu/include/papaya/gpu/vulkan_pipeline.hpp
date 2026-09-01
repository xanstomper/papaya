#pragma once

#include "papaya/common/types.hpp"
#include "papaya/gpu/pipeline_map.hpp"
#include <string>
#include <vector>

// Device-bound Vulkan pipeline creation (Stage 4d).
//
// Builds a VkPipeline from the D3D11 pipeline state the runtime captured:
// VS/PS SPIR-V (already compiled in-process from DXBC, Stage 4c), vertex
// input from the input layout (Stage 4b mapping), and the descriptor set
// layout that matches the emitter's binding scheme. Pure Vulkan resource
// juggling: everything is taken from the plain structs of pipeline_map /
// shader_compile, so the builder is a thin, reviewable glue layer.

namespace papaya::gpu {

// Everything a graphics pipeline needs beyond the immutable state.
struct PipelineSpec {
    std::vector<u32> vs_spirv;        // vertex shader SPIR-V words
    std::vector<u32> ps_spirv;        // fragment shader SPIR-V words
    std::vector<VulkanVertexBinding> vertex_bindings;     // from build_vertex_input
    std::vector<VulkanVertexAttribute> vertex_attributes; // from build_vertex_input
    std::vector<VulkanDescriptorBinding> descriptors;     // from build_descriptor_layout
    bool alpha_blend{false};          // D3D11 blend state default: off
};

// Owned Vulkan objects for one pipeline (destroy with destroy_graphics_pipeline).
struct GraphicsPipeline {
    u64 device{0};                    // VkDevice
    u64 pipeline{0};                  // VkPipeline
    u64 layout{0};                    // VkPipelineLayout
    u64 render_pass{0};               // VkRenderPass (dedicated, presentable)
    u64 descriptor_layout{0};         // VkDescriptorSetLayout
};

// Create the pipeline on `device` for a color target of `color_format`
// (VkFormat, e.g. VK_FORMAT_B8G8R8A8_UNORM). Returns false + err on failure.
bool create_graphics_pipeline(u64 device, u32 color_format, const PipelineSpec& spec,
                              GraphicsPipeline& out, std::string& err);

void destroy_graphics_pipeline(GraphicsPipeline& p);

} // namespace papaya::gpu