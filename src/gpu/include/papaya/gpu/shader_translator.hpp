#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <span>

namespace papaya::gpu {

enum class ShaderStage {
    Vertex,
    Pixel,
    Compute,
    Geometry,
    Hull,
    Domain
};

struct TranslatedSpirV {
    ShaderStage stage;
    std::vector<u32> spirv_bytecode;
    u32 binding_count{0};
};

class IShaderTranslator {
public:
    virtual ~IShaderTranslator() = default;
    virtual Result<TranslatedSpirV> translate_gcn_to_spirv(ShaderStage stage, std::span<const u8> gcn_isa_bytes) = 0;
};

} // namespace papaya::gpu
