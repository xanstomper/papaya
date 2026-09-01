// Device-bound Vulkan pipeline creation test (Stage 4d).
//
// Full end-to-end on a real device when one exists: DXBC -> in-process
// glslang SPIR-V (Stage 4c) -> pipeline_map vertex/descriptor state (4b) ->
// VkPipeline, VkRenderPass, VkPipelineLayout (4d). Headless: own instance +
// first graphics-queue device (lavapipe/llvmpipe works). When no Vulkan
// device can be created the test SKIPS (exit 0) so CI stays hermetic.
#include "papaya/gpu/pipeline_map.hpp"
#include "papaya/gpu/shader_compile.hpp"
#include "papaya/gpu/shader_translator.hpp"
#include "papaya/gpu/vulkan_pipeline.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

using papaya::gpu::build_descriptor_layout;
using papaya::gpu::build_vertex_input;
using papaya::gpu::create_graphics_pipeline;
using papaya::gpu::destroy_graphics_pipeline;
using papaya::gpu::D3d11InputElement;
using papaya::gpu::dxbc_to_spirv;
using papaya::gpu::kAppendAlignedElement;
using papaya::gpu::kStageFragment;
using papaya::gpu::kStageVertex;
using papaya::gpu::PipelineSpec;
using papaya::u8;
using papaya::u32;
using papaya::u64;

static void put_u32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

// VS: v0 -> o0;  PS: t0 * o0 -> o0  (texture * interpolant).
static std::vector<u8> build_dxbc(bool vertex) {
    auto inst = [](u32 op, u32 len, u32 flags = 0) {
        return ((len & 0x1Fu) << 24) | ((flags & 0x7u) << 11) | (op & 0xFFu);
    };
    auto opd = [](u32 rt, u32 order, u32 dim, u32 mask, u32 sw) {
        return (rt << 12) | ((order & 3) << 20) | (dim & 3) | ((mask & 0xF) << 4) | sw;
    };
    auto swz = [](u32 x, u32 y, u32 z, u32 w) { return (x&3)<<4 | (y&3)<<6 | (z&3)<<8 | (w&3)<<10; };
    const u32 kId = swz(0, 1, 2, 3);
    std::vector<u32> stream;
    if (vertex) {
        stream = {
            inst(0x5F, 3), opd(1, 1, 3, 0xF, 0), 0,      // dcl_input v0
            inst(0x65, 3), opd(2, 1, 3, 0xF, 0), 0,      // dcl_output o0
            inst(0x68, 2), 1,
            inst(0x36, 5), opd(0, 1, 3, 0xF, 0), 0, opd(1, 1, 3, 0, kId), 0,  // mov r0, v0
            inst(0x00, 7), opd(2, 1, 3, 0xF, 0), 0, opd(0, 1, 3, 0, kId), 0,
            opd(1, 1, 3, 0, kId), 0,                    // add o0, r0, v0
        };
    } else {
        stream = {
            inst(0x5F, 3), opd(1, 1, 3, 0xF, 0), 0,      // dcl_input v0
            inst(0x65, 3), opd(2, 1, 3, 0xF, 0), 0,      // dcl_output o0
            inst(0x68, 2), 1,
            inst(0x58, 4, 3), opd(7, 1, 3, 0, 0), 0, 0x55555555,  // dcl_resource t0
            inst(0x5A, 3), opd(6, 1, 3, 0, 0), 0,        // dcl_sampler s0
            inst(0x45, 9), opd(0, 1, 3, 0xF, 0), 0, opd(1, 1, 3, 0, kId), 0,
            opd(7, 1, 3, 0, 0), 0, opd(6, 1, 3, 0, 0), 0,   // sample r0, v0.xy, t0, s0
            inst(0x00, 7), opd(2, 1, 3, 0xF, 0), 0, opd(0, 1, 3, 0, kId), 0,
            opd(1, 1, 3, 0, kId), 0,                    // add o0, r0, v0
        };
    }
    std::vector<u8> b;
    put_u32(b, 0x3000);
    put_u32(b, static_cast<u32>(stream.size()));
    for (u32 w : stream) put_u32(b, w);
    const u32 total = 44 + 12 + static_cast<u32>(b.size());
    std::vector<u8> blob;
    blob.insert(blob.end(), { 'D','X','B','C' });
    for (int i = 0; i < 16; ++i) blob.push_back(0);
    put_u32(blob, 1); put_u32(blob, 0); put_u32(blob, 0);
    put_u32(blob, total); put_u32(blob, 1); put_u32(blob, 44);
    put_u32(blob, 0x52444853);
    put_u32(blob, static_cast<u32>(b.size()));
    put_u32(blob, 56);
    blob.insert(blob.end(), b.begin(), b.end());
    return blob;
}

int main() {
#ifdef PAPAYA_HAS_VULKAN
    // ---- headless device ----
    VkInstance inst = VK_NULL_HANDLE;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || !inst) {
        std::printf("skip: no Vulkan instance\n");
        return 0;
    }
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    {
        u32 n = 0;
        vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst, &n, devs.data());
        if (!devs.empty()) phys = devs[0];
    }
    if (!phys) { vkDestroyInstance(inst, nullptr); std::printf("skip: no physical device\n"); return 0; }
    u32 qf = 0;
    {
        u32 n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qfps(n);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qfps.data());
        for (u32 i = 0; i < n; ++i)
            if (qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS || !device) {
        vkDestroyInstance(inst, nullptr);
        std::printf("skip: no graphics device\n");
        return 0;
    }

    // ---- DXBC -> SPIR-V (in-process glslang) ----
    const std::vector<u8> vs_dxbc = build_dxbc(true);
    const std::vector<u8> ps_dxbc = build_dxbc(false);
    std::vector<u32> vs_spv, ps_spv;
    std::string err;
    if (!dxbc_to_spirv({vs_dxbc.data(), vs_dxbc.size()}, kStageVertex, vs_spv, err) ||
        !dxbc_to_spirv({ps_dxbc.data(), ps_dxbc.size()}, kStageFragment, ps_spv, err)) {
        std::printf("fail: SPIR-V compile: %s\n", err.c_str());
        vkDestroyDevice(device, nullptr); vkDestroyInstance(inst, nullptr);
        return 1;
    }

    // ---- pipeline_map state: POSITION layout + t0 sampler + cb0 ----
    const std::vector<D3d11InputElement> elements = {
        { "POSITION", 0, 6, 0, kAppendAlignedElement, 0, 0 },
        { "TEXCOORD", 0, 16, 0, kAppendAlignedElement, 0, 0 },
    };
    std::vector<papaya::gpu::VulkanVertexBinding> vbind;
    std::vector<papaya::gpu::VulkanVertexAttribute> vattr;
    if (!build_vertex_input(elements, vbind, vattr)) return 2;
    bool cb[16] = { true };
    bool smp[16] = { true };
    bool shd[16] = {};
    std::vector<papaya::gpu::VulkanDescriptorBinding> desc;
    build_descriptor_layout(cb, smp, shd, desc);

    // ---- VkPipeline ----
    PipelineSpec spec;
    spec.vs_spirv = vs_spv;
    spec.ps_spirv = ps_spv;
    spec.vertex_bindings = vbind;
    spec.vertex_attributes = vattr;
    spec.descriptors = desc;
    papaya::gpu::GraphicsPipeline gp;
    if (!create_graphics_pipeline(reinterpret_cast<u64>(device), VK_FORMAT_B8G8R8A8_UNORM,
                                  spec, gp, err)) {
        std::printf("fail: pipeline creation: %s\n", err.c_str());
        vkDestroyDevice(device, nullptr); vkDestroyInstance(inst, nullptr);
        return 3;
    }
    if (gp.pipeline == 0 || gp.layout == 0 || gp.render_pass == 0 || gp.descriptor_layout == 0)
        return 4;
    destroy_graphics_pipeline(gp);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(inst, nullptr);
    std::printf("ok: DXBC -> glslang -> VkPipeline (+render pass +layout +descriptors)\n");
    return 0;
#else
    std::printf("skip: Vulkan unavailable in this build\n");
    return 0;
#endif
}