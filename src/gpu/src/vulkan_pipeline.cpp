// Device-bound Vulkan pipeline creation (see vulkan_pipeline.hpp).
#include "papaya/gpu/vulkan_pipeline.hpp"

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace papaya::gpu {

#ifdef PAPAYA_HAS_VULKAN

namespace {

constexpr u32 kStageBitVertex = VK_SHADER_STAGE_VERTEX_BIT;
constexpr u32 kStageBitFragment = VK_SHADER_STAGE_FRAGMENT_BIT;

} // namespace

bool create_graphics_pipeline(u64 device_u64, u32 color_format, const PipelineSpec& spec,
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
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn_ci{};
    dyn_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount = 2;
    dyn_ci.pDynamicStates = dyn;

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
    ci.pDynamicState = &dyn_ci;
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

#else   // !PAPAYA_HAS_VULKAN

bool create_graphics_pipeline(u64, u32, const PipelineSpec&, GraphicsPipeline&, std::string& err) {
    err = "Vulkan not available in this build";
    return false;
}
void destroy_graphics_pipeline(GraphicsPipeline&) {}

#endif

} // namespace papaya::gpu