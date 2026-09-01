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
// (VkFormat, e.g. VK_FORMAT_B8G8R8A8_UNORM). vp_w/vp_h bake a STATIC
// viewport+scissor (matching D3D11 state semantics; dynamic viewport was
// observed to silently draw nothing on some drivers); 0/0 keeps the
// pseudo-dynamic path. Returns false + err on failure.
bool create_graphics_pipeline(u64 device, u32 color_format, u32 vp_w, u32 vp_h,
                              const PipelineSpec& spec,
                              GraphicsPipeline& out, std::string& err);

void destroy_graphics_pipeline(GraphicsPipeline& p);

// Render the pipeline into a target. `target_image` = 0 creates its own
// offscreen image (and fills `pixels_out` with RGBA8 readback when non-null);
// `target_image` != 0 renders into that image (an external target such as a
// swapchain image: a view + framebuffer are created for it, no readback).
// cbuffer_data is uploaded to every UniformBuffer binding (binding b reads at
// offset 16*(b-16) in a 64KB scratch UBO); image bindings get a 2x2 red
// test texture. This is the record/execute path: render pass, command
// buffer, descriptors, submit.
bool render_pipeline(u64 device, u64 physical_device, u32 w, u32 h, u32 color_format,
                     const PipelineSpec& spec, u64 target_image,
                     const u8* vertex_data, u32 vertex_stride, u32 vertex_count,
                     const u8* cbuffer_data, size_t cbuffer_size,
                     const u8* texture_data, u32 texture_w, u32 texture_h,
                     std::vector<u8>* pixels_out, std::string& err);

// Offscreen convenience wrapper: own target + readback pixels.
inline bool render_offscreen(u64 device, u64 physical_device, u32 w, u32 h,
                             u32 color_format, const PipelineSpec& spec,
                             const u8* vertex_data, u32 vertex_stride, u32 vertex_count,
                             const u8* cbuffer_data, size_t cbuffer_size,
                             std::vector<u8>& pixels, std::string& err) {
    return render_pipeline(device, physical_device, w, h, color_format, spec, 0,
                           vertex_data, vertex_stride, vertex_count,
                           cbuffer_data, cbuffer_size, nullptr, 0, 0, &pixels, err);
}

} // namespace papaya::gpu