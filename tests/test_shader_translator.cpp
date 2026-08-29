#include "papaya/common/logger.hpp"
#include "papaya/gpu/shader_translator.hpp"
#include "papaya/gpu/gcn_isa.hpp"
#include <cassert>
#include <vector>
#include <iostream>
#include <cstring>

#ifdef PAPAYA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

// Helper to construct a synthetic GCN Vertex Shader bytecode stream
std::vector<papaya::u32> create_synthetic_gcn_vertex_shader() {
    using namespace papaya;
    std::vector<u32> code;

    // 1. v_mov_b32 v0, 1.0f (VOP1: opcode 0x301 -> 1, VDST = 0, SRC0 = 242 (1.0f))
    code.push_back(0x7E000000 | (0 << 17) | (1 << 9) | 242);

    // 2. v_mov_b32 v1, 2.0f (VOP1: VDST = 1, SRC0 = 244 (2.0f))
    code.push_back(0x7E000000 | (1 << 17) | (1 << 9) | 244);

    // 3. v_add_f32 v2, v0, v1 (VOP2: opcode 0x401 -> 1, VDST = 2, VSRC1 = 1, SRC0 = 256 (v0))
    code.push_back((1 << 25) | (2 << 17) | (1 << 9) | 256);

    // 4. v_mov_b32 v3, 1.0f (VOP1: VDST = 3, SRC0 = 242 (1.0f))
    code.push_back(0x7E000000 | (3 << 17) | (1 << 9) | 242);

    // 5. exp pos0, v0, v1, v2, v3 (EXP: target = 12 (POS0), en = 0x0F, done = 1)
    code.push_back(0xF8000000 | (1 << 11) | (12 << 4) | 0x0F);
    code.push_back((3 << 24) | (2 << 16) | (1 << 8) | 0);

    // 6. s_endpgm (SOPP: 0xBF800000 | (1 << 16))
    code.push_back(0xBF800000 | (1 << 16));

    return code;
}

// Helper to construct a synthetic GCN Pixel Shader bytecode stream
std::vector<papaya::u32> create_synthetic_gcn_pixel_shader() {
    using namespace papaya;
    std::vector<u32> code;

    // 1. v_mov_b32 v0, 1.0f (Red = 1.0f)
    code.push_back(0x7E000000 | (0 << 17) | (1 << 9) | 242);

    // 2. v_mov_b32 v1, 0.5f (Green = 0.5f)
    code.push_back(0x7E000000 | (1 << 17) | (1 << 9) | 240);

    // 3. v_mov_b32 v2, 0.0f (Blue = 0.0f)
    code.push_back(0x7E000000 | (2 << 17) | (1 << 9) | 128); // 128 = 0

    // 4. v_mov_b32 v3, 1.0f (Alpha = 1.0f)
    code.push_back(0x7E000000 | (3 << 17) | (1 << 9) | 242);

    // 5. exp mrt0, v0, v1, v2, v3 (EXP: target = 0 (MRT0), en = 0x0F, done = 1)
    code.push_back(0xF8000000 | (1 << 11) | (0 << 4) | 0x0F);
    code.push_back((3 << 24) | (2 << 16) | (1 << 8) | 0);

    // 6. s_endpgm
    code.push_back(0xBF800000 | (1 << 16));

    return code;
}

int main() {
    using namespace papaya;
    using namespace papaya::gpu;

    log::info("TEST", "Running unit test: test_shader_translator");

    GcnShaderTranslator translator;

    // 1. Test Vertex Shader Translation
    auto vs_gcn = create_synthetic_gcn_vertex_shader();
    auto vs_span = std::span<const u8>(
        reinterpret_cast<const u8*>(vs_gcn.data()),
        vs_gcn.size() * sizeof(u32)
    );

    auto vs_spv_res = translator.translate_gcn_to_spirv(ShaderStage::Vertex, vs_span);
    assert(vs_spv_res.has_value());
    assert(vs_spv_res->stage == ShaderStage::Vertex);
    assert(!vs_spv_res->spirv_bytecode.empty());

    // Check SPIR-V header
    const auto& vs_spv = vs_spv_res->spirv_bytecode;
    assert(vs_spv[0] == 0x07230203); // Magic
    assert(vs_spv[1] == 0x00010300); // SPIR-V 1.3
    assert(vs_spv[3] > 0);          // Bound > 0

    log::info("TEST", "Vertex Shader translated successfully ({} SPIR-V dwords)", vs_spv.size());

    // 2. Test Pixel Shader Translation
    auto ps_gcn = create_synthetic_gcn_pixel_shader();
    auto ps_span = std::span<const u8>(
        reinterpret_cast<const u8*>(ps_gcn.data()),
        ps_gcn.size() * sizeof(u32)
    );

    auto ps_spv_res = translator.translate_gcn_to_spirv(ShaderStage::Pixel, ps_span);
    assert(ps_spv_res.has_value());
    assert(ps_spv_res->stage == ShaderStage::Pixel);
    assert(!ps_spv_res->spirv_bytecode.empty());

    const auto& ps_spv = ps_spv_res->spirv_bytecode;
    assert(ps_spv[0] == 0x07230203); // Magic
    assert(ps_spv[1] == 0x00010300); // SPIR-V 1.3

    log::info("TEST", "Pixel Shader translated successfully ({} SPIR-V dwords)", ps_spv.size());

#ifdef PAPAYA_HAS_VULKAN
    // 3. Test compilation with real Vulkan instance/device if available
    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "TestTranslator";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;

    if (vkCreateInstance(&ci, nullptr, &instance) == VK_SUCCESS) {
        uint32_t gpu_count = 0;
        vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
        if (gpu_count > 0) {
            std::vector<VkPhysicalDevice> gpus(gpu_count);
            vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());

            float queue_prio = 1.0f;
            VkDeviceQueueCreateInfo qci{};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = 0;
            qci.queueCount = 1;
            qci.pQueuePriorities = &queue_prio;

            VkDeviceCreateInfo dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            dci.queueCreateInfoCount = 1;
            dci.pQueueCreateInfos = &qci;

            VkDevice dev = VK_NULL_HANDLE;
            if (vkCreateDevice(gpus[0], &dci, nullptr, &dev) == VK_SUCCESS) {
                // Test creating Vulkan shader modules from translated SPIR-V
                VkShaderModuleCreateInfo vs_module_ci{};
                vs_module_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                vs_module_ci.codeSize = vs_spv.size() * sizeof(u32);
                vs_module_ci.pCode = vs_spv.data();

                VkShaderModule vs_mod = VK_NULL_HANDLE;
                VkResult vs_res = vkCreateShaderModule(dev, &vs_module_ci, nullptr, &vs_mod);
                assert(vs_res == VK_SUCCESS);
                log::info("TEST", "Vulkan driver compiled Vertex Shader Module successfully!");

                VkShaderModuleCreateInfo ps_module_ci{};
                ps_module_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                ps_module_ci.codeSize = ps_spv.size() * sizeof(u32);
                ps_module_ci.pCode = ps_spv.data();

                VkShaderModule ps_mod = VK_NULL_HANDLE;
                VkResult ps_res = vkCreateShaderModule(dev, &ps_module_ci, nullptr, &ps_mod);
                assert(ps_res == VK_SUCCESS);
                log::info("TEST", "Vulkan driver compiled Pixel Shader Module successfully!");

                vkDestroyShaderModule(dev, vs_mod, nullptr);
                vkDestroyShaderModule(dev, ps_mod, nullptr);
                vkDestroyDevice(dev, nullptr);
            }
        }
        vkDestroyInstance(instance, nullptr);
    }
#endif

    log::info("TEST", ">>> test_shader_translator PASSED ALL CHECKS! <<<");
    return 0;
}
