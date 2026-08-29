#pragma once

#include "papaya/common/types.hpp"
#include "papaya/gpu/shader_translator.hpp"
#include <vector>
#include <string>
#include <unordered_map>

namespace papaya::gpu::spirv {

// SPIR-V Core Opcodes
enum class SpvOp : u16 {
    OpNop = 0,
    OpSource = 3,
    OpName = 5,
    OpMemberName = 6,
    OpExtInstImport = 11,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstantTrue = 41,
    OpConstantFalse = 42,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpFunction = 54,
    OpFunctionParameter = 55,
    OpFunctionEnd = 56,
    OpFunctionCall = 57,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpVectorShuffle = 79,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpFNegate = 127,
    OpFAdd = 129,
    OpFSub = 131,
    OpFMul = 133,
    OpFDiv = 136,
    OpDot = 148,
    OpLabel = 248,
    OpBranch = 249,
    OpBranchConditional = 250,
    OpReturn = 253,
    OpReturnValue = 254
};

// SPIR-V Execution Model
enum class ExecutionModel : u32 {
    Vertex = 0,
    TessellationControl = 1,
    TessellationEvaluation = 2,
    Geometry = 3,
    Fragment = 4,
    GLCompute = 5
};

// SPIR-V Storage Class
enum class StorageClass : u32 {
    UniformConstant = 0,
    Input = 1,
    Uniform = 2,
    Output = 3,
    Workgroup = 4,
    CrossWorkgroup = 5,
    Private = 6,
    Function = 7,
    PushConstant = 9
};

// SPIR-V Decoration
enum class Decoration : u32 {
    RelaxedPrecision = 0,
    Block = 2,
    BufferBlock = 3,
    RowMajor = 4,
    ColMajor = 5,
    ArrayStride = 6,
    MatrixStride = 7,
    BuiltIn = 11,
    Location = 30,
    Binding = 33,
    DescriptorSet = 34,
    Offset = 35
};

// SPIR-V BuiltIn
enum class BuiltIn : u32 {
    Position = 0,
    PointSize = 1,
    ClipDistance = 3,
    VertexId = 5,
    InstanceId = 6,
    FragCoord = 15,
    FragDepth = 22,
    NumWorkgroups = 24,
    WorkgroupSize = 25,
    WorkgroupId = 26,
    LocalInvocationId = 27,
    GlobalInvocationId = 28
};

class SpirvBuilder {
public:
    SpirvBuilder();

    u32 alloc_id();

    // Type getters
    u32 type_void();
    u32 type_float(u32 width = 32);
    u32 type_int(u32 width = 32, bool is_signed = true);
    u32 type_vec(u32 component_type, u32 count);
    u32 type_pointer(StorageClass storage, u32 type);
    u32 type_function(u32 return_type, const std::vector<u32>& param_types = {});

    // Constants
    u32 constant_f32(f32 val);
    u32 constant_u32(u32 val);
    u32 constant_vec4(f32 x, f32 y, f32 z, f32 w);

    // Header & Declarations
    void set_memory_model(u32 addressing = 0, u32 memory = 1); // Logical GLSL450
    void add_capability(u32 capability = 1);                    // Shader
    void add_entry_point(ExecutionModel model, u32 fn_id, std::string_view name, const std::vector<u32>& interface_ids);
    void add_execution_mode(u32 fn_id, u32 mode, const std::vector<u32>& literals = {});

    // Decorate
    void decorate_location(u32 target_id, u32 location);
    void decorate_builtin(u32 target_id, BuiltIn builtin);
    void decorate_binding(u32 target_id, u32 set, u32 binding);

    // Variables & Functions
    u32 declare_variable(u32 ptr_type, StorageClass storage, std::string_view name = "");
    void begin_function(u32 fn_id, u32 return_type, u32 fn_type);
    void end_function();

    u32 begin_label();

    // Instructions
    u32 emit_load(u32 result_type, u32 pointer_id);
    void emit_store(u32 pointer_id, u32 value_id);
    u32 emit_fadd(u32 result_type, u32 a, u32 b);
    u32 emit_fsub(u32 result_type, u32 a, u32 b);
    u32 emit_fmul(u32 result_type, u32 a, u32 b);
    u32 emit_composite_construct(u32 result_type, const std::vector<u32>& constituents);
    void emit_return();

    std::vector<u32> assemble();

private:
    void write_instruction(std::vector<u32>& section, SpvOp op, const std::vector<u32>& operands);

    u32 next_id_{1};

    std::vector<u32> capabilities_;
    std::vector<u32> memory_model_;
    std::vector<u32> entry_points_;
    std::vector<u32> execution_modes_;
    std::vector<u32> names_;
    std::vector<u32> decorations_;
    std::vector<u32> types_and_constants_;
    std::vector<u32> variables_;
    std::vector<u32> function_body_;

    std::unordered_map<f32, u32> f32_constants_;
    std::unordered_map<u32, u32> u32_constants_;
    u32 void_type_id_{0};
    u32 float_type_id_{0};
    u32 int_type_id_{0};
    u32 vec4_type_id_{0};
};

} // namespace papaya::gpu::spirv
