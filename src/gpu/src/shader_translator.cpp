#include "papaya/gpu/shader_translator.hpp"
#include "papaya/gpu/spirv_builder.hpp"
#include "papaya/gpu/gcn_isa.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu {

Result<TranslatedSpirV> GcnShaderTranslator::translate_gcn_to_spirv(
    ShaderStage stage,
    std::span<const u8> gcn_isa_bytes
) {
    if (gcn_isa_bytes.empty()) {
        return ErrorCode::InvalidParameter;
    }

    size_t dword_count = gcn_isa_bytes.size() / sizeof(u32);
    auto dwords = std::span<const u32>(
        reinterpret_cast<const u32*>(gcn_isa_bytes.data()),
        dword_count
    );

    switch (stage) {
        case ShaderStage::Vertex:  return translate_vertex(dwords);
        case ShaderStage::Pixel:   return translate_pixel(dwords);
        case ShaderStage::Compute: return translate_compute(dwords);
        default:                   return ErrorCode::UnsupportedOperation;
    }
}

Result<TranslatedSpirV> GcnShaderTranslator::translate_vertex(std::span<const u32> dwords) {
    using namespace spirv;
    using namespace isa;

    SpirvBuilder builder;

    // Types
    u32 void_t = builder.type_void();
    u32 float_t = builder.type_float(32);
    u32 vec4_t = builder.type_vec(float_t, 4);

    u32 out_ptr_t = builder.type_pointer(StorageClass::Output, vec4_t);
    u32 in_ptr_t = builder.type_pointer(StorageClass::Input, vec4_t);
    u32 fn_t = builder.type_function(void_t);

    // Builtin output: gl_Position (vec4)
    u32 gl_pos_var = builder.declare_variable(out_ptr_t, StorageClass::Output, "gl_Position");
    builder.decorate_builtin(gl_pos_var, BuiltIn::Position);

    // Input: Location 0 (in_position)
    u32 in_pos_var = builder.declare_variable(in_ptr_t, StorageClass::Input, "in_position");
    builder.decorate_location(in_pos_var, 0);

    // Output: Location 0 (out_color / varying)
    u32 out_color_var = builder.declare_variable(out_ptr_t, StorageClass::Output, "out_color");
    builder.decorate_location(out_color_var, 0);

    // Main function
    u32 main_fn = builder.alloc_id();
    builder.add_entry_point(ExecutionModel::Vertex, main_fn, "main", {gl_pos_var, in_pos_var, out_color_var});

    builder.begin_function(main_fn, void_t, fn_t);
    builder.begin_label();

    // Track active value ID for VGPR registers (v0..v255)
    std::unordered_map<u32, u32> vgpr_map;

    auto get_operand_id = [&](const Operand& op) -> u32 {
        if (op.type == OperandType::VGPR) {
            auto it = vgpr_map.find(op.reg_index);
            if (it != vgpr_map.end()) return it->second;
            return builder.constant_f32(0.0f);
        } else if (op.type == OperandType::InlineConstant || op.type == OperandType::LiteralConstant) {
            return builder.constant_f32(op.float_value);
        }
        return builder.constant_f32(0.0f);
    };

    auto instructions = GcnDecoder::disassemble_program(dwords);
    bool return_emitted = false;

    for (const auto& inst : instructions) {
        switch (inst.opcode) {
            case GcnOpcode::V_MOV_B32: {
                u32 val_id = get_operand_id(inst.src0);
                vgpr_map[inst.dst.reg_index] = val_id;
                break;
            }
            case GcnOpcode::V_ADD_F32: {
                u32 a = get_operand_id(inst.src0);
                u32 b = get_operand_id(inst.src1);
                u32 res = builder.emit_fadd(float_t, a, b);
                vgpr_map[inst.dst.reg_index] = res;
                break;
            }
            case GcnOpcode::V_SUB_F32: {
                u32 a = get_operand_id(inst.src0);
                u32 b = get_operand_id(inst.src1);
                u32 res = builder.emit_fsub(float_t, a, b);
                vgpr_map[inst.dst.reg_index] = res;
                break;
            }
            case GcnOpcode::V_MUL_F32: {
                u32 a = get_operand_id(inst.src0);
                u32 b = get_operand_id(inst.src1);
                u32 res = builder.emit_fmul(float_t, a, b);
                vgpr_map[inst.dst.reg_index] = res;
                break;
            }
            case GcnOpcode::EXP: {
                u32 cx = get_operand_id(inst.exp_src[0]);
                u32 cy = get_operand_id(inst.exp_src[1]);
                u32 cz = get_operand_id(inst.exp_src[2]);
                u32 cw = get_operand_id(inst.exp_src[3]);

                u32 vec_val = builder.emit_composite_construct(vec4_t, {cx, cy, cz, cw});

                if (inst.exp_target == ExportTarget::POS0) {
                    builder.emit_store(gl_pos_var, vec_val);
                } else if (inst.exp_target == ExportTarget::PARAM0) {
                    builder.emit_store(out_color_var, vec_val);
                }
                break;
            }
            case GcnOpcode::S_ENDPGM: {
                builder.emit_return();
                return_emitted = true;
                break;
            }
            default:
                break;
        }
    }

    if (!return_emitted) {
        builder.emit_return();
    }

    builder.end_function();

    auto bytecode = builder.assemble();
    log::info("TRANSLATOR", "Translated Vertex Shader: {} GCN instructions -> {} SPIR-V dwords",
              instructions.size(), bytecode.size());

    return TranslatedSpirV{
        .stage = ShaderStage::Vertex,
        .spirv_bytecode = std::move(bytecode),
        .binding_count = 0
    };
}

Result<TranslatedSpirV> GcnShaderTranslator::translate_pixel(std::span<const u32> dwords) {
    using namespace spirv;
    using namespace isa;

    SpirvBuilder builder;

    u32 void_t = builder.type_void();
    u32 float_t = builder.type_float(32);
    u32 vec4_t = builder.type_vec(float_t, 4);

    u32 out_ptr_t = builder.type_pointer(StorageClass::Output, vec4_t);
    u32 fn_t = builder.type_function(void_t);

    // Output: Location 0 (out_color / FragColor)
    u32 out_color_var = builder.declare_variable(out_ptr_t, StorageClass::Output, "out_color");
    builder.decorate_location(out_color_var, 0);

    // Main function
    u32 main_fn = builder.alloc_id();
    builder.add_entry_point(ExecutionModel::Fragment, main_fn, "main", {out_color_var});
    builder.add_execution_mode(main_fn, 7); // OriginUpperLeft

    builder.begin_function(main_fn, void_t, fn_t);
    builder.begin_label();

    std::unordered_map<u32, u32> vgpr_map;

    auto get_operand_id = [&](const Operand& op) -> u32 {
        if (op.type == OperandType::VGPR) {
            auto it = vgpr_map.find(op.reg_index);
            if (it != vgpr_map.end()) return it->second;
            return builder.constant_f32(0.0f);
        } else if (op.type == OperandType::InlineConstant || op.type == OperandType::LiteralConstant) {
            return builder.constant_f32(op.float_value);
        }
        return builder.constant_f32(0.0f);
    };

    auto instructions = GcnDecoder::disassemble_program(dwords);
    bool return_emitted = false;

    for (const auto& inst : instructions) {
        switch (inst.opcode) {
            case GcnOpcode::V_MOV_B32: {
                u32 val_id = get_operand_id(inst.src0);
                vgpr_map[inst.dst.reg_index] = val_id;
                break;
            }
            case GcnOpcode::V_ADD_F32: {
                u32 a = get_operand_id(inst.src0);
                u32 b = get_operand_id(inst.src1);
                u32 res = builder.emit_fadd(float_t, a, b);
                vgpr_map[inst.dst.reg_index] = res;
                break;
            }
            case GcnOpcode::V_SUB_F32: {
                u32 a = get_operand_id(inst.src0);
                u32 b = get_operand_id(inst.src1);
                u32 res = builder.emit_fsub(float_t, a, b);
                vgpr_map[inst.dst.reg_index] = res;
                break;
            }
            case GcnOpcode::V_MUL_F32: {
                u32 a = get_operand_id(inst.src0);
                u32 b = get_operand_id(inst.src1);
                u32 res = builder.emit_fmul(float_t, a, b);
                vgpr_map[inst.dst.reg_index] = res;
                break;
            }
            case GcnOpcode::EXP: {
                u32 cx = get_operand_id(inst.exp_src[0]);
                u32 cy = get_operand_id(inst.exp_src[1]);
                u32 cz = get_operand_id(inst.exp_src[2]);
                u32 cw = get_operand_id(inst.exp_src[3]);

                u32 vec_val = builder.emit_composite_construct(vec4_t, {cx, cy, cz, cw});

                if (inst.exp_target == ExportTarget::MRT0) {
                    builder.emit_store(out_color_var, vec_val);
                }
                break;
            }
            case GcnOpcode::S_ENDPGM: {
                builder.emit_return();
                return_emitted = true;
                break;
            }
            default:
                break;
        }
    }

    if (!return_emitted) {
        builder.emit_return();
    }

    builder.end_function();

    auto bytecode = builder.assemble();
    log::info("TRANSLATOR", "Translated Pixel Shader: {} GCN instructions -> {} SPIR-V dwords",
              instructions.size(), bytecode.size());

    return TranslatedSpirV{
        .stage = ShaderStage::Pixel,
        .spirv_bytecode = std::move(bytecode),
        .binding_count = 0
    };
}

Result<TranslatedSpirV> GcnShaderTranslator::translate_compute(std::span<const u32> dwords) {
    using namespace spirv;
    using namespace isa;

    SpirvBuilder builder;

    u32 void_t = builder.type_void();
    u32 fn_t = builder.type_function(void_t);

    u32 main_fn = builder.alloc_id();
    builder.add_entry_point(ExecutionModel::GLCompute, main_fn, "main", {});
    builder.add_execution_mode(main_fn, 17, {64, 1, 1}); // LocalSize (64, 1, 1)

    builder.begin_function(main_fn, void_t, fn_t);
    builder.begin_label();
    builder.emit_return();
    builder.end_function();

    return TranslatedSpirV{
        .stage = ShaderStage::Compute,
        .spirv_bytecode = builder.assemble(),
        .binding_count = 0
    };
}

} // namespace papaya::gpu
