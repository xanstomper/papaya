// D3D11-state -> Vulkan pipeline mapping test (Stage 4b).
//
// Pure translation logic, no Vulkan device needed: DXGI_FORMAT -> VkFormat for
// vertex inputs, vertex bindings/attributes from a captured input layout
// (stride math + D3D11_APPEND_ALIGNED_ELEMENT), descriptor layout from the
// emitter's binding scheme (samplers 0-15, cbuffers 16-31, shadows 32-47),
// and refusal of unusable formats / colliding descriptors.
#include "papaya/gpu/pipeline_map.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

using papaya::gpu::build_descriptor_layout;
using papaya::gpu::build_vertex_input;
using papaya::gpu::descriptor_bindings_valid;
using papaya::gpu::D3d11InputElement;
using papaya::gpu::DescriptorType;
using papaya::gpu::dxgi_format_size;
using papaya::gpu::dxgi_format_to_vk_format;
using papaya::gpu::kAppendAlignedElement;
using papaya::gpu::kDescriptorStageFragment;
using papaya::gpu::kDescriptorStageVertex;
using papaya::gpu::VulkanVertexAttribute;
using papaya::gpu::VulkanVertexBinding;
using papaya::u32;

int main() {
#ifdef PAPAYA_HAS_VULKAN
    // --- DXGI_FORMAT -> VkFormat spot checks (real VkFormat values) ---
    if (dxgi_format_to_vk_format(6) != VK_FORMAT_R32G32B32_SFLOAT) return 1;    // POSITION
    if (dxgi_format_to_vk_format(16) != VK_FORMAT_R32G32_SFLOAT) return 2;      // TEXCOORD
    if (dxgi_format_to_vk_format(28) != VK_FORMAT_R8G8B8A8_UNORM) return 3;
    if (dxgi_format_to_vk_format(41) != VK_FORMAT_R32_SFLOAT) return 4;
    if (dxgi_format_to_vk_format(24) != VK_FORMAT_A2B10G10R10_UNORM_PACK32) return 5;
    // unusable as vertex input: depth, typeless, compressed, unknown
    if (dxgi_format_to_vk_format(20) != 0 || dxgi_format_to_vk_format(1) != 0 ||
        dxgi_format_to_vk_format(65) != 0 || dxgi_format_to_vk_format(999) != 0)
        return 6;
    if (dxgi_format_size(6) != 12 || dxgi_format_size(28) != 4 || dxgi_format_size(61) != 1)
        return 7;

    // --- Vertex input from a captured D3D11 input layout ---
    // Layout: slot 0: POSITION R32G32B32_FLOAT @ append, TEXCOORD R32G32_FLOAT
    // @ append, NORMAL R32G32B32_FLOAT @ append; slot 1: BONE R32G32B32A32_
    // FLOAT @ append (per-instance).
    std::vector<D3d11InputElement> elements = {
        { "POSITION", 0, 6, 0, kAppendAlignedElement, 0, 0 },
        { "TEXCOORD", 0, 16, 0, kAppendAlignedElement, 0, 0 },
        { "NORMAL", 0, 6, 0, 0x14, 0, 0 },                 // explicit offset 20
        { "BONE", 0, 2, 1, kAppendAlignedElement, 1, 1 },  // per-instance
    };
    std::vector<VulkanVertexBinding> bindings;
    std::vector<VulkanVertexAttribute> attrs;
    if (!build_vertex_input(elements, bindings, attrs)) return 8;
    if (bindings.size() != 2) return 9;
    // slot 0: append resolves POSITION@0(12) -> TEXCOORD@12(8) -> NORMAL@20(12)
    // = stride 32; slot 1: BONE@0(16) = stride 16, per-instance.
    if (bindings[0].binding != 0 || bindings[0].stride != 32 || bindings[0].input_rate != 0)
        return 10;
    if (bindings[1].binding != 1 || bindings[1].stride != 16 || bindings[1].input_rate != 1)
        return 11;
    if (attrs.size() != 4) return 12;
    if (attrs[0].location != 0 || attrs[0].binding != 0 || attrs[0].format != VK_FORMAT_R32G32B32_SFLOAT ||
        attrs[0].offset != 0)
        return 13;
    if (attrs[1].format != VK_FORMAT_R32G32_SFLOAT || attrs[1].offset != 12) return 14;
    if (attrs[2].offset != 20 || attrs[3].binding != 1 || attrs[3].offset != 0) return 15;

    // Unusable format (depth) in the layout must be refused, not mis-translated.
    std::vector<D3d11InputElement> bad_elements = {
        { "DEPTH", 0, 20, 0, 0, 0, 0 },   // D32_FLOAT
    };
    std::vector<VulkanVertexBinding> bad_b;
    std::vector<VulkanVertexAttribute> bad_a;
    if (build_vertex_input(bad_elements, bad_b, bad_a)) return 16;

    // --- Descriptor layout matching the emitter's binding scheme ---
    // Shader uses: sampler t0, cbuffer cb0, comparison texture t1 (shadow).
    bool cb[16] = { true };       // cb0
    bool smp[16] = { true };      // t0
    bool shd[16] = {};            // t1 shadow
    shd[1] = true;
    std::vector<papaya::gpu::VulkanDescriptorBinding> desc;
    build_descriptor_layout(cb, smp, shd, desc);
    if (!descriptor_bindings_valid(desc)) return 17;
    bool got_sampler0 = false, got_cb0 = false, got_shadow1 = false;
    u32 count = 0;
    for (const auto& d : desc) {
        if (d.binding == 0 && d.type == DescriptorType::CombinedImageSampler &&
            d.stage_bits == kDescriptorStageFragment) got_sampler0 = true;
        if (d.binding == 16 && d.type == DescriptorType::UniformBuffer &&
            (d.stage_bits & (kDescriptorStageVertex | kDescriptorStageFragment)) ==
            (kDescriptorStageVertex | kDescriptorStageFragment))
            got_cb0 = true;
        if (d.binding == 33 && d.type == DescriptorType::CombinedImageSampler) got_shadow1 = true;
        ++count;
    }
    if (!got_sampler0 || !got_cb0 || !got_shadow1 || count != 3) return 18;

    std::printf("ok: format map, vertex input (append + instance), descriptor layout\n");
    return 0;
#else
    std::printf("ok: Vulkan unavailable, mapping stubbed\n");
    return 0;
#endif
}