// Device-bound Vulkan pipeline creation (see vulkan_pipeline.hpp).
#include "papaya/gpu/vulkan_pipeline.hpp"

#include <cstring>
#include <functional>

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace papaya::gpu {

#ifdef PAPAYA_HAS_VULKAN

namespace {

constexpr u32 kStageBitVertex = VK_SHADER_STAGE_VERTEX_BIT;
constexpr u32 kStageBitFragment = VK_SHADER_STAGE_FRAGMENT_BIT;

} // namespace

bool create_graphics_pipeline(u64 device_u64, u32 color_format, u32 vp_w, u32 vp_h,
                              const PipelineSpec& spec,
                              GraphicsPipeline& out, std::string& err) {
    auto* device = reinterpret_cast<VkDevice>(device_u64);
    out = {};
    out.device = device_u64;

    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    auto fail = [&](const char* what, VkResult r) {
        err = std::string(what) + " (VkResult " + std::to_string(static_cast<long>(r)) + ")";
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (ps) vkDestroyShaderModule(device, ps, nullptr);
        destroy_graphics_pipeline(out);
        return false;
    };

    // ---- shader modules from the compiled SPIR-V ----
    auto make_module = [&](const std::vector<u32>& spirv, VkShaderModule* m) -> VkResult {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = spirv.size() * sizeof(u32);
        ci.pCode = spirv.data();
        return vkCreateShaderModule(device, &ci, nullptr, m);
    };
    if (spec.vs_spirv.empty() || spec.ps_spirv.empty()) return fail("empty SPIR-V", VK_ERROR_INITIALIZATION_FAILED);
    if (auto r = make_module(spec.vs_spirv, &vs); r != VK_SUCCESS) return fail("vkCreateShaderModule(vs)", r);
    if (auto r = make_module(spec.ps_spirv, &ps); r != VK_SUCCESS) return fail("vkCreateShaderModule(ps)", r);

    // ---- descriptor set layout from the pipeline_map bindings ----
    std::vector<VkDescriptorSetLayoutBinding> dsb;
    dsb.reserve(spec.descriptors.size());
    for (const auto& d : spec.descriptors) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = d.binding;
        b.descriptorType = d.type == DescriptorType::UniformBuffer
                                   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                   : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = 0;
        if (d.stage_bits & kDescriptorStageVertex) b.stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        if (d.stage_bits & kDescriptorStageFragment) b.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        dsb.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo dsl_ci{};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = static_cast<u32>(dsb.size());
    dsl_ci.pBindings = dsb.empty() ? nullptr : dsb.data();
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (auto r = vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &dsl); r != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout", r);
    out.descriptor_layout = reinterpret_cast<u64>(dsl);

    // ---- pipeline layout ----
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &dsl;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (auto r = vkCreatePipelineLayout(device, &pl_ci, nullptr, &layout); r != VK_SUCCESS)
        return fail("vkCreatePipelineLayout", r);
    out.layout = reinterpret_cast<u64>(layout);

    // ---- render pass: single color attachment, presentable ----
    VkAttachmentDescription att{};
    att.format = static_cast<VkFormat>(color_format);
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    VkRenderPassCreateInfo rp_ci{};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &att;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &sub;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (auto r = vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass); r != VK_SUCCESS)
        return fail("vkCreateRenderPass", r);
    out.render_pass = reinterpret_cast<u64>(render_pass);

    // ---- vertex input from the pipeline_map ----
    std::vector<VkVertexInputBindingDescription> vbind;
    for (const auto& b : spec.vertex_bindings)
        vbind.push_back({ b.binding, b.stride, b.input_rate ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                            : VK_VERTEX_INPUT_RATE_VERTEX });
    std::vector<VkVertexInputAttributeDescription> vattr;
    for (const auto& a : spec.vertex_attributes)
        vattr.push_back({ a.location, a.binding, static_cast<VkFormat>(a.format), a.offset });
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = static_cast<u32>(vbind.size());
    vi.pVertexBindingDescriptions = vbind.empty() ? nullptr : vbind.data();
    vi.vertexAttributeDescriptionCount = static_cast<u32>(vattr.size());
    vi.pVertexAttributeDescriptions = vattr.empty() ? nullptr : vattr.data();

    // ---- fixed function state ----
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;   // D3D11 default
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cb.blendEnable = spec.alpha_blend ? VK_TRUE : VK_FALSE;
    if (spec.alpha_blend) {
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &cb;
    // Static viewport + scissor matching the D3D11 target size (dynamic
    // viewport was observed to draw nothing on llvmpipe/intel with this
    // pipeline shape; D3D11 viewports are static state anyway).
    VkViewport svp{ 0, 0, static_cast<float>(vp_w), static_cast<float>(vp_h), 0, 1 };
    VkRect2D ssc{ {0, 0}, {vp_w, vp_h} };
    VkPipelineViewportStateCreateInfo vp_ci{};
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.pViewports = &svp;
    vp_ci.scissorCount = 1;
    vp_ci.pScissors = &ssc;

    // ---- stages + pipeline ----
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &blend;
    ci.pViewportState = &vp_ci;
    ci.layout = layout;
    ci.renderPass = render_pass;
    ci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    vkDestroyShaderModule(device, vs, nullptr);   // modules are consumed into the pipeline
    vkDestroyShaderModule(device, ps, nullptr);
    if (r != VK_SUCCESS) return fail("vkCreateGraphicsPipelines", r);
    out.pipeline = reinterpret_cast<u64>(pipeline);
    return true;
}

void destroy_graphics_pipeline(GraphicsPipeline& p) {
    auto* device = reinterpret_cast<VkDevice>(p.device);
    if (!device) return;
    if (p.pipeline) vkDestroyPipeline(device, reinterpret_cast<VkPipeline>(p.pipeline), nullptr);
    if (p.layout) vkDestroyPipelineLayout(device, reinterpret_cast<VkPipelineLayout>(p.layout), nullptr);
    if (p.render_pass) vkDestroyRenderPass(device, reinterpret_cast<VkRenderPass>(p.render_pass), nullptr);
    if (p.descriptor_layout)
        vkDestroyDescriptorSetLayout(device, reinterpret_cast<VkDescriptorSetLayout>(p.descriptor_layout), nullptr);
    p = {};
}

// ---- offscreen record/execute (Stage 4e) ------------------------------------

namespace {

u32 find_memory_type(VkPhysicalDevice phys, u32 type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0xFFFFFFFFu;
}

bool create_host_buffer(VkDevice device, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBufferUsageFlags usage, const void* src,
                        VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bc{};
    bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bc.size = size;
    bc.usage = usage;
    if (vkCreateBuffer(device, &bc, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, buf, &mr);
    const u32 type = find_memory_type(phys, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == 0xFFFFFFFFu) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, buf, mem, 0);
    if (src && size) {
        void* p = nullptr;
        if (vkMapMemory(device, mem, 0, size, 0, &p) != VK_SUCCESS) return false;
        std::memcpy(p, src, static_cast<size_t>(size));
        vkUnmapMemory(device, mem);
    }
    return true;
}

bool create_device_image(VkDevice device, VkPhysicalDevice phys, u32 w, u32 h,
                         VkFormat format, VkImageUsageFlags usage,
                         VkImage& image, VkDeviceMemory& mem) {
    VkImageCreateInfo ic{};
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = format;
    ic.extent = { w, h, 1 };
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = usage;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ic, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, image, &mr);
    const u32 type = find_memory_type(phys, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image, mem, 0);
    return true;
}

} // namespace

bool render_pipeline(u64 device_u64, u64 phys_u64, u32 w, u32 h, u32 color_format,
                      const PipelineSpec& spec, u64 target_image_u64,
                      const u8* vertex_data, u32 vertex_stride, u32 vertex_count,
                      const u8* cbuffer_data, size_t cbuffer_size,
                      std::vector<u8>* pixels_out, std::string& err) {
    auto* device = reinterpret_cast<VkDevice>(device_u64);
    auto* phys = reinterpret_cast<VkPhysicalDevice>(phys_u64);
    VkImage target_external = reinterpret_cast<VkImage>(target_image_u64);
    if (pixels_out) pixels_out->clear();
    std::vector<std::function<void()>> cleanup;
    auto guard = [&](std::function<void()> fn) { cleanup.push_back(std::move(fn)); };
    auto fail = [&](const char* what, VkResult r) -> bool {
        err = std::string(what) + " (VkResult " + std::to_string(static_cast<long>(r)) + ")";
        for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) (*it)();
        return false;
    };

    GraphicsPipeline gp;
    if (!create_graphics_pipeline(device_u64, color_format, w, h, spec, gp, err)) return false;
    guard([&] { destroy_graphics_pipeline(gp); });

    // ---- vertex buffer (host-visible, copied) ----
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    if (!create_host_buffer(device, phys, static_cast<VkDeviceSize>(vertex_stride) * vertex_count,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_data, vbuf, vmem))
        return fail("vertex buffer", VK_ERROR_OUT_OF_HOST_MEMORY);
    guard([&] { vkDestroyBuffer(device, vbuf, nullptr); vkFreeMemory(device, vmem, nullptr); });

    // ---- descriptor pool + set ----
    u32 ubo_count = 0, img_count = 0;
    for (const auto& d : spec.descriptors) {
        if (d.type == DescriptorType::UniformBuffer) ++ubo_count; else ++img_count;
    }
    const VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, ubo_count ? ubo_count : 1u },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, img_count ? img_count : 1u },
    };
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = pool_sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (auto r = vkCreateDescriptorPool(device, &dpi, nullptr, &pool); r != VK_SUCCESS)
        return fail("vkCreateDescriptorPool", r);
    guard([&] { vkDestroyDescriptorPool(device, pool, nullptr); });
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    const VkDescriptorSetLayout dsl = reinterpret_cast<VkDescriptorSetLayout>(gp.descriptor_layout);
    dai.pSetLayouts = &dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (auto r = vkAllocateDescriptorSets(device, &dai, &set); r != VK_SUCCESS)
        return fail("vkAllocateDescriptorSets", r);

    // ---- cbuffer scratch UBO (64KB; binding b reads at offset 16*(b-16)) ----
    constexpr VkDeviceSize kCbScratchSize = 64 * 1024;
    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory ubom = VK_NULL_HANDLE;
    if (!create_host_buffer(device, phys, kCbScratchSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            nullptr, ubo, ubom))
        return fail("cbuffer UBO", VK_ERROR_OUT_OF_HOST_MEMORY);
    guard([&] { vkDestroyBuffer(device, ubo, nullptr); vkFreeMemory(device, ubom, nullptr); });
    {
        void* p = nullptr;
        if (vkMapMemory(device, ubom, 0, kCbScratchSize, 0, &p) != VK_SUCCESS)
            return fail("vkMapMemory(ubo)", VK_ERROR_OUT_OF_HOST_MEMORY);
        std::memset(p, 0, static_cast<size_t>(kCbScratchSize));
        if (cbuffer_data && cbuffer_size) {
            const size_t copy = cbuffer_size < static_cast<size_t>(kCbScratchSize)
                                        ? cbuffer_size : static_cast<size_t>(kCbScratchSize);
            std::memcpy(p, cbuffer_data, copy);
        }
        vkUnmapMemory(device, ubom);
    }

    // ---- 2x2 red texture for image bindings ----
    VkImage tex = VK_NULL_HANDLE;
    VkDeviceMemory texm = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (img_count) {
        if (!create_device_image(device, phys, 2, 2, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT, tex, texm))
            return fail("test texture", VK_ERROR_OUT_OF_HOST_MEMORY);
        guard([&] { vkDestroyImage(device, tex, nullptr); vkFreeMemory(device, texm, nullptr); });
        // transition + write via a one-shot command (simple: use host-visible
        // memory by rebinding a host buffer? keep it correct with a command).
        VkImageViewCreateInfo ivc{};
        ivc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivc.image = tex;
        ivc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivc.format = VK_FORMAT_R8G8B8A8_UNORM;
        ivc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (auto r = vkCreateImageView(device, &ivc, nullptr, &tex_view); r != VK_SUCCESS)
            return fail("vkCreateImageView(tex)", r);
        guard([&] { vkDestroyImageView(device, tex_view, nullptr); });
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (auto r = vkCreateSampler(device, &sci, nullptr, &sampler); r != VK_SUCCESS)
            return fail("vkCreateSampler", r);
        guard([&] { vkDestroySampler(device, sampler, nullptr); });
    }

    // ---- descriptor writes ----
    std::vector<VkDescriptorBufferInfo> dinfo(ubo_count ? ubo_count : 1u);
    std::vector<VkDescriptorImageInfo> iinfo(img_count ? img_count : 1u);
    std::vector<VkWriteDescriptorSet> writes;
    u32 ui = 0, ii = 0;
    for (const auto& d : spec.descriptors) {
        if (d.type == DescriptorType::UniformBuffer) {
            dinfo[ui].buffer = ubo;
            dinfo[ui].offset = static_cast<VkDeviceSize>(16) * (d.binding - 16u);
            dinfo[ui].range = kCbScratchSize;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.dstBinding = d.binding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &dinfo[ui];
            writes.push_back(w);
            ++ui;
        } else {
            iinfo[ii].sampler = sampler;
            iinfo[ii].imageView = tex_view;
            iinfo[ii].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.dstBinding = d.binding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &iinfo[ii];
            writes.push_back(w);
            ++ii;
        }
    }
    if (!writes.empty()) vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()),
                                                writes.data(), 0, nullptr);

    // ---- render target: own offscreen image or an external one ----
    VkImage target = target_external;
    VkDeviceMemory target_mem = VK_NULL_HANDLE;
    if (!target) {
        if (!create_device_image(device, phys, w, h, static_cast<VkFormat>(color_format),
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 target, target_mem))
            return fail("render target", VK_ERROR_OUT_OF_HOST_MEMORY);
        guard([&] { vkDestroyImage(device, target, nullptr); vkFreeMemory(device, target_mem, nullptr); });
    }
    VkImageView target_view = VK_NULL_HANDLE;
    {
        VkImageViewCreateInfo ivc{};
        ivc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivc.image = target;
        ivc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivc.format = static_cast<VkFormat>(color_format);
        ivc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (auto r = vkCreateImageView(device, &ivc, nullptr, &target_view); r != VK_SUCCESS)
            return fail("vkCreateImageView(target)", r);
        guard([&] { vkDestroyImageView(device, target_view, nullptr); });
    }
    VkFramebuffer fb = VK_NULL_HANDLE;
    {
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = reinterpret_cast<VkRenderPass>(gp.render_pass);
        fci.attachmentCount = 1;
        fci.pAttachments = &target_view;
        fci.width = w;
        fci.height = h;
        fci.layers = 1;
        if (auto r = vkCreateFramebuffer(device, &fci, nullptr, &fb); r != VK_SUCCESS)
            return fail("vkCreateFramebuffer", r);
        guard([&] { vkDestroyFramebuffer(device, fb, nullptr); });
    }

    // ---- host-visible readback buffer (only for own targets with readback) ----
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rbm = VK_NULL_HANDLE;
    if (pixels_out) {
        if (!create_host_buffer(device, phys, static_cast<VkDeviceSize>(w) * h * 4,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr, rb, rbm))
            return fail("readback buffer", VK_ERROR_OUT_OF_HOST_MEMORY);
        guard([&] { vkDestroyBuffer(device, rb, nullptr); vkFreeMemory(device, rbm, nullptr); });
    }

    // ---- command buffer ----
    VkCommandPool cpool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (auto r = vkCreateCommandPool(device, &cpci, nullptr, &cpool); r != VK_SUCCESS)
            return fail("vkCreateCommandPool", r);
        guard([&] { vkDestroyCommandPool(device, cpool, nullptr); });
    }
    VkCommandBuffer cb = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cpool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (auto r = vkAllocateCommandBuffers(device, &cbai, &cb); r != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers", r);
    }

    // Record once from image-layout-undefined targets (texture data is filled
    // before use through a staging copy embedded in the same command buffer).
    auto record = [&](VkImage img, VkImageView view, VkFramebuffer fbuf) -> bool {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
        // texture: layout transition + fill (2x2 red)
        if (img_count && tex) {
            VkImageMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = tex;
            bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
            const u8 red[4] = { 255, 0, 0, 255 };
            const VkImageSubresourceRange tex_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 reinterpret_cast<const VkClearColorValue*>(&red), 1, &tex_range);
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        }
        // render pass
        VkClearValue clear{};
        clear.color = { { 0.0f, 0.0f, 1.0f, 1.0f } };   // blue background
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = reinterpret_cast<VkRenderPass>(gp.render_pass);
        rbi.framebuffer = fbuf;
        rbi.renderArea = { 0, 0, w, h };
        rbi.clearValueCount = 1;
        rbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          reinterpret_cast<VkPipeline>(gp.pipeline));
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                reinterpret_cast<VkPipelineLayout>(gp.layout), 0, 1, &set, 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
            vkCmdDraw(cb, vertex_count, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        if (!pixels_out) return vkEndCommandBuffer(cb) == VK_SUCCESS;
        // target -> readback buffer
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = img;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy bic{};
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bic.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bic);
        return vkEndCommandBuffer(cb) == VK_SUCCESS;
    };
    if (!record(target, target_view, fb)) return fail("command record", VK_ERROR_UNKNOWN);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (auto r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); r != VK_SUCCESS)
        return fail("vkQueueSubmit", r);
    if (auto r = vkDeviceWaitIdle(device); r != VK_SUCCESS)
        return fail("vkDeviceWaitIdle", r);

    // ---- read back (only for own offscreen targets) ----
    if (pixels_out) {
        pixels_out->resize(static_cast<size_t>(w) * h * 4);
        void* p = nullptr;
        if (vkMapMemory(device, rbm, 0, pixels_out->size(), 0, &p) != VK_SUCCESS)
            return fail("vkMapMemory(readback)", VK_ERROR_OUT_OF_HOST_MEMORY);
        std::memcpy(pixels_out->data(), p, pixels_out->size());
        vkUnmapMemory(device, rbm);
    }

    for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) (*it)();
    return true;
}

#else   // !PAPAYA_HAS_VULKAN

bool create_graphics_pipeline(u64, u32, u32, u32, const PipelineSpec&, GraphicsPipeline&,
                                std::string& err) {
    err = "Vulkan not available in this build";
    return false;
}
void destroy_graphics_pipeline(GraphicsPipeline&) {}

#endif

} // namespace papaya::gpu