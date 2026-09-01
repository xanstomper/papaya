#pragma once

#include "papaya/common/types.hpp"
#include <string>
#include <vector>

// D3D11-state -> Vulkan pipeline mapping (Stage 4b).
//
// Converts the pipeline state the win32 D3D11 layer captures (input-layout
// elements, bound VS/PS, resource usage) into the Vulkan descriptors a
// VkPipeline needs: vertex input bindings/attributes from DXGI_FORMAT
// elements, and a descriptor-set layout that matches the bindings the GLSL
// emitter declares (samplers tN on 0-15, cbuffers cbN on 16-31, shadow
// samplers tN_shadow on 32-47). This is pure translation logic: no Vulkan
// device is needed to build these tables, so it is fully unit-testable.

namespace papaya::gpu {

// Captured D3D11_INPUT_ELEMENT_DESC (mirrors win32_d3d's D3DInputLayoutElement).
struct D3d11InputElement {
    std::string semantic;       // "POSITION", "TEXCOORD", ...
    u32 semantic_index{0};
    u32 dxgi_format{0};         // DXGI_FORMAT enum value
    u32 input_slot{0};          // 0-15
    u32 aligned_offset{0};      // or 0xFFFFFFFF = D3D11_APPEND_ALIGNED_ELEMENT
    u32 step_class{0};          // 0 = PER_VERTEX_DATA, 1 = PER_INSTANCE_DATA
    u32 step_rate{0};
};

constexpr u32 kAppendAlignedElement = 0xFFFFFFFFu;

// VkVertexInputBindingDescription-ish (plain values).
struct VulkanVertexBinding {
    u32 binding{0};
    u32 stride{0};
    u32 input_rate{0};          // 0 = per-vertex, 1 = per-instance
};

// VkVertexInputAttributeDescription-ish.
struct VulkanVertexAttribute {
    u32 location{0};            // shader input location (element order)
    u32 binding{0};             // input slot
    u32 format{0};              // VkFormat
    u32 offset{0};              // byte offset within the vertex
};

enum class DescriptorType : u32 { CombinedImageSampler = 0, UniformBuffer = 1 };

constexpr u32 kDescriptorStageVertex = 1;   // stage bits (VK-style flags)
constexpr u32 kDescriptorStageFragment = 2;

// VkDescriptorSetLayoutBinding-ish.
struct VulkanDescriptorBinding {
    u32 binding{0};
    DescriptorType type{DescriptorType::CombinedImageSampler};
    u32 stage_bits{0};          // kStageVertex | kStageFragment
};

// DXGI_FORMAT -> VkFormat for vertex inputs (0 = VK_FORMAT_UNDEFINED, i.e. the
// format is not usable as a vertex input: depth/typeless/compressed or unknown).
u32 dxgi_format_to_vk_format(u32 dxgi_format);

// Bytes per vertex element for a DXGI_FORMAT (0 = unknown/unsupported).
u32 dxgi_format_size(u32 dxgi_format);

// Vertex input state: one binding per used input slot (stride = max end of any
// element, D3D11_APPEND_ALIGNED_ELEMENT resolved at 4-byte alignment like the
// D3D10/11 runtime) and one attribute per element (location = element order).
// Returns false when a format is not a usable vertex input.
bool build_vertex_input(const std::vector<D3d11InputElement>& elements,
                        std::vector<VulkanVertexBinding>& bindings,
                        std::vector<VulkanVertexAttribute>& attributes);

// Descriptor set layout matching the emitter's bindings: samplers (incl.
// shadow samplers for comparison textures) on 0-15, cbuffers on 16-31,
// shadow samplers on 32-47. cbuffers span all shader stages they are bound to;
// samplers are fragment-stage.
void build_descriptor_layout(const bool cbuffers[16], const bool samplers[16],
                             const bool shadows[16],
                             std::vector<VulkanDescriptorBinding>& out);

// True when no two bindings collide (the emitter's scheme never collides by
// construction; used as a sanity check in tests).
bool descriptor_bindings_valid(const std::vector<VulkanDescriptorBinding>& bindings);

} // namespace papaya::gpu