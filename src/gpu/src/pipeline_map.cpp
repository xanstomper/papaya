// D3D11-state -> Vulkan pipeline mapping (see pipeline_map.hpp).
#include "papaya/gpu/pipeline_map.hpp"

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace papaya::gpu {

#ifdef PAPAYA_HAS_VULKAN

namespace {

// DXGI_FORMAT -> VkFormat + byte size for vertex inputs. 0 = unusable as a
// vertex input (typeless, depth-stencil, compressed, or unknown format).
struct FormatRow {
    u32 dxgi;
    u32 vk;
    u32 size;
};

constexpr FormatRow kFormats[] = {
    { 2, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
    { 3, VK_FORMAT_R32G32B32A32_UINT, 16 },
    { 4, VK_FORMAT_R32G32B32A32_SINT, 16 },
    { 6, VK_FORMAT_R32G32B32_SFLOAT, 12 },
    { 7, VK_FORMAT_R32G32B32_UINT, 12 },
    { 8, VK_FORMAT_R32G32B32_SINT, 12 },
    { 10, VK_FORMAT_R16G16B16A16_SFLOAT, 8 },
    { 11, VK_FORMAT_R16G16B16A16_UNORM, 8 },
    { 12, VK_FORMAT_R16G16B16A16_UINT, 8 },
    { 13, VK_FORMAT_R16G16B16A16_SNORM, 8 },
    { 14, VK_FORMAT_R16G16B16A16_SINT, 8 },
    { 16, VK_FORMAT_R32G32_SFLOAT, 8 },
    { 17, VK_FORMAT_R32G32_UINT, 8 },
    { 18, VK_FORMAT_R32G32_SINT, 8 },
    { 24, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 },
    { 25, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4 },
    { 26, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4 },   // same bit layout, R<->B order note
    { 28, VK_FORMAT_R8G8B8A8_UNORM, 4 },
    { 29, VK_FORMAT_R8G8B8A8_SRGB, 4 },
    { 30, VK_FORMAT_R8G8B8A8_UINT, 4 },
    { 31, VK_FORMAT_R8G8B8A8_SNORM, 4 },
    { 32, VK_FORMAT_R8G8B8A8_SINT, 4 },
    { 34, VK_FORMAT_R16G16_SFLOAT, 4 },
    { 35, VK_FORMAT_R16G16_UNORM, 4 },
    { 36, VK_FORMAT_R16G16_UINT, 4 },
    { 37, VK_FORMAT_R16G16_SNORM, 4 },
    { 38, VK_FORMAT_R16G16_SINT, 4 },
    { 41, VK_FORMAT_R32_SFLOAT, 4 },
    { 42, VK_FORMAT_R32_UINT, 4 },
    { 43, VK_FORMAT_R32_SINT, 4 },
    { 49, VK_FORMAT_R8G8_UNORM, 2 },
    { 50, VK_FORMAT_R8G8_UINT, 2 },
    { 51, VK_FORMAT_R8G8_SNORM, 2 },
    { 52, VK_FORMAT_R8G8_SINT, 2 },
    { 54, VK_FORMAT_R16_SFLOAT, 2 },
    { 56, VK_FORMAT_R16_UNORM, 2 },
    { 57, VK_FORMAT_R16_UINT, 2 },
    { 58, VK_FORMAT_R16_SNORM, 2 },
    { 59, VK_FORMAT_R16_SINT, 2 },
    { 61, VK_FORMAT_R8_UNORM, 1 },
    { 62, VK_FORMAT_R8_UINT, 1 },
    { 63, VK_FORMAT_R8_SNORM, 1 },
    { 64, VK_FORMAT_R8_SINT, 1 },
    { 85, VK_FORMAT_B8G8R8A8_UNORM, 4 },
    { 87, VK_FORMAT_B8G8R8A8_SRGB, 4 },
    { 88, VK_FORMAT_B8G8R8A8_UNORM, 4 },   // B8G8R8X8: alpha ignored
};

} // namespace

u32 dxgi_format_to_vk_format(u32 dxgi_format) {
    for (const auto& f : kFormats)
        if (f.dxgi == dxgi_format) return f.vk;
    return 0;   // VK_FORMAT_UNDEFINED
}

u32 dxgi_format_size(u32 dxgi_format) {
    for (const auto& f : kFormats)
        if (f.dxgi == dxgi_format) return f.size;
    return 0;
}

bool build_vertex_input(const std::vector<D3d11InputElement>& elements,
                        std::vector<VulkanVertexBinding>& bindings,
                        std::vector<VulkanVertexAttribute>& attributes) {
    u32 slot_end[16] = {};
    u32 slot_stride[16] = {};
    bool slot_used[16] = {};
    attributes.clear();
    bindings.clear();
    attributes.reserve(elements.size());
    for (u32 i = 0; i < elements.size(); ++i) {
        const auto& e = elements[i];
        if (e.input_slot >= 16) return false;
        if (dxgi_format_to_vk_format(e.dxgi_format) == 0) return false;   // unusable format
        const u32 size = dxgi_format_size(e.dxgi_format);
        u32 off = e.aligned_offset;
        if (off == kAppendAlignedElement) {
            off = (slot_end[e.input_slot] + 3u) & ~3u;   // 4-byte alignment, like D3D10/11
        }
        slot_end[e.input_slot] = off + size;
        if (off + size > slot_stride[e.input_slot]) slot_stride[e.input_slot] = off + size;
        slot_used[e.input_slot] = true;
        attributes.push_back({ i, e.input_slot, dxgi_format_to_vk_format(e.dxgi_format), off });
    }
    for (u32 b = 0; b < 16; ++b) {
        if (!slot_used[b]) continue;
        bindings.push_back({ b, slot_stride[b], 0 });
    }
    for (const auto& e : elements)
        if (e.step_class == 1)
            for (auto& b : bindings)
                if (b.binding == e.input_slot) b.input_rate = 1;
    return true;
}

void build_descriptor_layout(const bool cbuffers[16], const bool samplers[16],
                             const bool shadows[16],
                             std::vector<VulkanDescriptorBinding>& out) {
    out.clear();
    for (u32 s = 0; s < 16; ++s) {
        if (samplers[s]) out.push_back({ s, DescriptorType::CombinedImageSampler, kStageFragment });
        if (shadows[s]) out.push_back({ 32u + s, DescriptorType::CombinedImageSampler, kStageFragment });
    }
    for (u32 c = 0; c < 16; ++c)
        if (cbuffers[c])
            out.push_back({ 16u + c, DescriptorType::UniformBuffer,
                            kStageVertex | kStageFragment });
}

bool descriptor_bindings_valid(const std::vector<VulkanDescriptorBinding>& bindings) {
    for (size_t i = 0; i < bindings.size(); ++i)
        for (size_t j = i + 1; j < bindings.size(); ++j)
            if (bindings[i].binding == bindings[j].binding) return false;
    return true;
}

#else   // !PAPAYA_HAS_VULKAN: no Vulkan on the host, mapping table unavailable.

u32 dxgi_format_to_vk_format(u32) { return 0; }
u32 dxgi_format_size(u32) { return 0; }
bool build_vertex_input(const std::vector<D3d11InputElement>&, std::vector<VulkanVertexBinding>&,
                        std::vector<VulkanVertexAttribute>&) { return false; }
void build_descriptor_layout(const bool[16], const bool[16], const bool[16],
                             std::vector<VulkanDescriptorBinding>&) {}
bool descriptor_bindings_valid(const std::vector<VulkanDescriptorBinding>&) { return true; }

#endif

} // namespace papaya::gpu