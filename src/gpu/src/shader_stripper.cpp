#include "papaya/gpu/shader_stripper.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu {

constexpr u32 SPIRV_MAGIC_NUMBER = 0x07230203;
constexpr u32 OP_EXECUTION_MODE  = 16;
constexpr u32 OP_ENTRY_POINT     = 15;

ShaderStripper::ShaderStripper(const ShaderStripperConfig& config)
    : config_(config) {}

ShaderType ShaderStripper::classify_spirv(std::span<const u32> spirv_code) const {
    if (spirv_code.size() < 5 || spirv_code[0] != SPIRV_MAGIC_NUMBER) {
        return ShaderType::Unknown;
    }

    size_t idx = 5;
    while (idx < spirv_code.size()) {
        u32 word = spirv_code[idx];
        u16 opcode = static_cast<u16>(word & 0xFFFF);
        u16 word_count = static_cast<u16>((word >> 16) & 0xFFFF);

        if (word_count == 0 || idx + word_count > spirv_code.size()) break;

        if (opcode == OP_ENTRY_POINT && word_count >= 3) {
            u32 execution_model = spirv_code[idx + 1];
            switch (execution_model) {
                case 0: return ShaderType::Vertex;
                case 4: return ShaderType::Fragment;
                case 5: return ShaderType::Compute;
                default: break;
            }
        }
        idx += word_count;
    }

    return ShaderType::Unknown;
}

bool ShaderStripper::should_strip(std::span<const u32> spirv_code) const {
    ShaderType type = classify_spirv(spirv_code);

    if (type == ShaderType::Compute && config_.strip_compute_postprocess) {
        stripped_shader_count_++;
        return true;
    }

    return false;
}

std::vector<u32> ShaderStripper::generate_noop_spirv(ShaderType type) const {
    // Minimal valid SPIR-V 1.3 compute or fragment shader that performs a return
    std::vector<u32> noop = {
        0x07230203, // Magic
        0x00010300, // Version 1.3
        0x00000000, // Generator
        0x00000005, // Bound
        0x00000000, // Schema
        // OpCapability Shader
        0x00020011, 0x00000001,
        // OpMemoryModel Logical GLSL450
        0x0003000E, 0x00000000, 0x00000001,
        // OpEntryPoint GLCompute 1 "main"
        0x0004000F, 0x00000005, 0x00000001, 0x6E69616D,
        // OpExecutionMode 1 LocalSize 1 1 1
        0x00060010, 0x00000001, 0x00000011, 0x00000001, 0x00000001, 0x00000001,
        // OpTypeVoid 2
        0x00020013, 0x00000002,
        // OpTypeFunction 3 2
        0x00030021, 0x00000003, 0x00000002,
        // OpFunction 2 1 0 3
        0x00050036, 0x00000002, 0x00000001, 0x00000000, 0x00000003,
        // OpLabel 4
        0x000200F8, 0x00000004,
        // OpReturn
        0x000100FD,
        // OpFunctionEnd
        0x00010038
    };

    return noop;
}

} // namespace papaya::gpu
