#include "papaya/gpu/spirv_builder.hpp"
#include <cstring>

namespace papaya::gpu::spirv {

SpirvBuilder::SpirvBuilder() {
    add_capability(1); // Shader capability
    set_memory_model(0, 1); // Logical GLSL450
}

u32 SpirvBuilder::alloc_id() {
    return next_id_++;
}

void SpirvBuilder::write_instruction(std::vector<u32>& section, SpvOp op, const std::vector<u32>& operands) {
    u32 word_count = static_cast<u32>(operands.size() + 1);
    u32 header = (word_count << 16) | static_cast<u16>(op);
    section.push_back(header);
    for (u32 word : operands) {
        section.push_back(word);
    }
}

void SpirvBuilder::add_capability(u32 capability) {
    write_instruction(capabilities_, SpvOp::OpCapability, {capability});
}

void SpirvBuilder::set_memory_model(u32 addressing, u32 memory) {
    write_instruction(memory_model_, SpvOp::OpMemoryModel, {addressing, memory});
}

void SpirvBuilder::add_entry_point(ExecutionModel model, u32 fn_id, std::string_view name, const std::vector<u32>& interface_ids) {
    std::vector<u32> ops;
    ops.push_back(static_cast<u32>(model));
    ops.push_back(fn_id);

    // Encode string in words (null-terminated, 4-byte padded)
    size_t name_len = name.size();
    size_t words = (name_len + 4) / 4;
    size_t start_idx = ops.size();
    ops.resize(start_idx + words, 0);
    std::memcpy(&ops[start_idx], name.data(), name_len);

    for (u32 if_id : interface_ids) {
        ops.push_back(if_id);
    }

    write_instruction(entry_points_, SpvOp::OpEntryPoint, ops);
}

void SpirvBuilder::add_execution_mode(u32 fn_id, u32 mode, const std::vector<u32>& literals) {
    std::vector<u32> ops = {fn_id, mode};
    for (u32 lit : literals) ops.push_back(lit);
    write_instruction(execution_modes_, SpvOp::OpExecutionMode, ops);
}

void SpirvBuilder::decorate_location(u32 target_id, u32 location) {
    write_instruction(decorations_, SpvOp::OpDecorate, {target_id, static_cast<u32>(Decoration::Location), location});
}

void SpirvBuilder::decorate_builtin(u32 target_id, BuiltIn builtin) {
    write_instruction(decorations_, SpvOp::OpDecorate, {target_id, static_cast<u32>(Decoration::BuiltIn), static_cast<u32>(builtin)});
}

void SpirvBuilder::decorate_binding(u32 target_id, u32 set, u32 binding) {
    write_instruction(decorations_, SpvOp::OpDecorate, {target_id, static_cast<u32>(Decoration::DescriptorSet), set});
    write_instruction(decorations_, SpvOp::OpDecorate, {target_id, static_cast<u32>(Decoration::Binding), binding});
}

u32 SpirvBuilder::type_void() {
    if (void_type_id_ == 0) {
        void_type_id_ = alloc_id();
        write_instruction(types_and_constants_, SpvOp::OpTypeVoid, {void_type_id_});
    }
    return void_type_id_;
}

u32 SpirvBuilder::type_float(u32 width) {
    if (float_type_id_ == 0) {
        float_type_id_ = alloc_id();
        write_instruction(types_and_constants_, SpvOp::OpTypeFloat, {float_type_id_, width});
    }
    return float_type_id_;
}

u32 SpirvBuilder::type_int(u32 width, bool is_signed) {
    if (int_type_id_ == 0) {
        int_type_id_ = alloc_id();
        write_instruction(types_and_constants_, SpvOp::OpTypeInt, {int_type_id_, width, is_signed ? 1U : 0U});
    }
    return int_type_id_;
}

u32 SpirvBuilder::type_vec(u32 component_type, u32 count) {
    if (component_type == type_float() && count == 4 && vec4_type_id_ != 0) {
        return vec4_type_id_;
    }
    u32 id = alloc_id();
    write_instruction(types_and_constants_, SpvOp::OpTypeVector, {id, component_type, count});
    if (component_type == type_float() && count == 4) {
        vec4_type_id_ = id;
    }
    return id;
}

u32 SpirvBuilder::type_pointer(StorageClass storage, u32 type) {
    u32 id = alloc_id();
    write_instruction(types_and_constants_, SpvOp::OpTypePointer, {id, static_cast<u32>(storage), type});
    return id;
}

u32 SpirvBuilder::type_function(u32 return_type, const std::vector<u32>& param_types) {
    u32 id = alloc_id();
    std::vector<u32> ops = {id, return_type};
    for (u32 p : param_types) ops.push_back(p);
    write_instruction(types_and_constants_, SpvOp::OpTypeFunction, ops);
    return id;
}

u32 SpirvBuilder::constant_f32(f32 val) {
    auto it = f32_constants_.find(val);
    if (it != f32_constants_.end()) return it->second;

    u32 id = alloc_id();
    u32 bit_val = *reinterpret_cast<const u32*>(&val);
    write_instruction(types_and_constants_, SpvOp::OpConstant, {type_float(), id, bit_val});
    f32_constants_[val] = id;
    return id;
}

u32 SpirvBuilder::constant_u32(u32 val) {
    auto it = u32_constants_.find(val);
    if (it != u32_constants_.end()) return it->second;

    u32 id = alloc_id();
    write_instruction(types_and_constants_, SpvOp::OpConstant, {type_int(32, false), id, val});
    u32_constants_[val] = id;
    return id;
}

u32 SpirvBuilder::constant_vec4(f32 x, f32 y, f32 z, f32 w) {
    u32 id_x = constant_f32(x);
    u32 id_y = constant_f32(y);
    u32 id_z = constant_f32(z);
    u32 id_w = constant_f32(w);

    u32 id = alloc_id();
    write_instruction(types_and_constants_, SpvOp::OpConstantComposite, {type_vec(type_float(), 4), id, id_x, id_y, id_z, id_w});
    return id;
}

u32 SpirvBuilder::declare_variable(u32 ptr_type, StorageClass storage, std::string_view name) {
    u32 id = alloc_id();
    write_instruction(variables_, SpvOp::OpVariable, {ptr_type, id, static_cast<u32>(storage)});
    if (!name.empty()) {
        std::vector<u32> ops = {id};
        size_t words = (name.size() + 4) / 4;
        size_t start = ops.size();
        ops.resize(start + words, 0);
        std::memcpy(&ops[start], name.data(), name.size());
        write_instruction(names_, SpvOp::OpName, ops);
    }
    return id;
}

void SpirvBuilder::begin_function(u32 fn_id, u32 return_type, u32 fn_type) {
    write_instruction(function_body_, SpvOp::OpFunction, {return_type, fn_id, 0, fn_type});
}

void SpirvBuilder::end_function() {
    write_instruction(function_body_, SpvOp::OpFunctionEnd, {});
}

u32 SpirvBuilder::begin_label() {
    u32 id = alloc_id();
    write_instruction(function_body_, SpvOp::OpLabel, {id});
    return id;
}

u32 SpirvBuilder::emit_load(u32 result_type, u32 pointer_id) {
    u32 id = alloc_id();
    write_instruction(function_body_, SpvOp::OpLoad, {result_type, id, pointer_id});
    return id;
}

void SpirvBuilder::emit_store(u32 pointer_id, u32 value_id) {
    write_instruction(function_body_, SpvOp::OpStore, {pointer_id, value_id});
}

u32 SpirvBuilder::emit_fadd(u32 result_type, u32 a, u32 b) {
    u32 id = alloc_id();
    write_instruction(function_body_, SpvOp::OpFAdd, {result_type, id, a, b});
    return id;
}

u32 SpirvBuilder::emit_fsub(u32 result_type, u32 a, u32 b) {
    u32 id = alloc_id();
    write_instruction(function_body_, SpvOp::OpFSub, {result_type, id, a, b});
    return id;
}

u32 SpirvBuilder::emit_fmul(u32 result_type, u32 a, u32 b) {
    u32 id = alloc_id();
    write_instruction(function_body_, SpvOp::OpFMul, {result_type, id, a, b});
    return id;
}

u32 SpirvBuilder::emit_composite_construct(u32 result_type, const std::vector<u32>& constituents) {
    u32 id = alloc_id();
    std::vector<u32> ops = {result_type, id};
    for (u32 c : constituents) ops.push_back(c);
    write_instruction(function_body_, SpvOp::OpCompositeConstruct, ops);
    return id;
}

void SpirvBuilder::emit_return() {
    write_instruction(function_body_, SpvOp::OpReturn, {});
}

std::vector<u32> SpirvBuilder::assemble() {
    std::vector<u32> spv;

    // SPIR-V Header
    spv.push_back(0x07230203); // Magic Number
    spv.push_back(0x00010300); // SPIR-V 1.3
    spv.push_back(0x00000000); // Generator
    spv.push_back(next_id_);   // Bound
    spv.push_back(0x00000000); // Schema

    // Sections
    spv.insert(spv.end(), capabilities_.begin(), capabilities_.end());
    spv.insert(spv.end(), memory_model_.begin(), memory_model_.end());
    spv.insert(spv.end(), entry_points_.begin(), entry_points_.end());
    spv.insert(spv.end(), execution_modes_.begin(), execution_modes_.end());
    spv.insert(spv.end(), names_.begin(), names_.end());
    spv.insert(spv.end(), decorations_.begin(), decorations_.end());
    spv.insert(spv.end(), types_and_constants_.begin(), types_and_constants_.end());
    spv.insert(spv.end(), variables_.begin(), variables_.end());
    spv.insert(spv.end(), function_body_.begin(), function_body_.end());

    return spv;
}

} // namespace papaya::gpu::spirv
