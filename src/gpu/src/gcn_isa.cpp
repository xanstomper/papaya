#include "papaya/gpu/gcn_isa.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::gpu::isa {

Operand GcnDecoder::decode_src_operand(u32 src_code, std::span<const u32> bytecode, size_t& current_offset) {
    Operand op{};
    op.raw_value = src_code;

    if (src_code <= 103) {
        // SGPR 0..103
        op.type = OperandType::SGPR;
        op.reg_index = src_code;
    } else if (src_code >= 128 && src_code <= 192) {
        // Positive inline constant (0..64)
        op.type = OperandType::InlineConstant;
        op.raw_value = src_code - 128;
        op.float_value = static_cast<f32>(op.raw_value);
    } else if (src_code >= 193 && src_code <= 208) {
        // Negative inline constant (-1..-16)
        op.type = OperandType::InlineConstant;
        op.raw_value = static_cast<u32>(-(static_cast<s32>(src_code - 192)));
        op.float_value = static_cast<f32>(static_cast<s32>(op.raw_value));
    } else if (src_code == 240) { // 0.5f
        op.type = OperandType::InlineConstant;
        op.float_value = 0.5f;
    } else if (src_code == 241) { // -0.5f
        op.type = OperandType::InlineConstant;
        op.float_value = -0.5f;
    } else if (src_code == 242) { // 1.0f
        op.type = OperandType::InlineConstant;
        op.float_value = 1.0f;
    } else if (src_code == 243) { // -1.0f
        op.type = OperandType::InlineConstant;
        op.float_value = -1.0f;
    } else if (src_code == 244) { // 2.0f
        op.type = OperandType::InlineConstant;
        op.float_value = 2.0f;
    } else if (src_code == 245) { // -2.0f
        op.type = OperandType::InlineConstant;
        op.float_value = -2.0f;
    } else if (src_code == 246) { // 4.0f
        op.type = OperandType::InlineConstant;
        op.float_value = 4.0f;
    } else if (src_code == 247) { // -4.0f
        op.type = OperandType::InlineConstant;
        op.float_value = -4.0f;
    } else if (src_code == 255) {
        // Literal 32-bit constant follows in next dword
        op.type = OperandType::LiteralConstant;
        if (current_offset + 1 < bytecode.size()) {
            op.raw_value = bytecode[++current_offset];
            op.float_value = *reinterpret_cast<const f32*>(&op.raw_value);
        }
    } else if (src_code >= 256 && src_code <= 511) {
        // VGPR 0..255 (in VOP instructions where bit 8 is set)
        op.type = OperandType::VGPR;
        op.reg_index = src_code - 256;
    } else {
        op.type = OperandType::InlineConstant;
    }

    return op;
}

Result<GcnInstruction> GcnDecoder::decode_instruction(std::span<const u32> bytecode, size_t dword_offset) {
    if (dword_offset >= bytecode.size()) {
        return ErrorCode::InvalidParameter;
    }

    u32 dword = bytecode[dword_offset];
    size_t curr = dword_offset;
    GcnInstruction inst{};

    // 1. SOPP (Scalar ALU with 16-bit constant/rel)
    // Prefix 0xBF800000 (bits 31..23 = 0b101111111)
    if ((dword & 0xFF800000) == 0xBF800000) {
        inst.encoding = InstructionEncoding::SOPP;
        u8 op_code = static_cast<u8>((dword >> 16) & 0x7F);
        inst.opcode = static_cast<GcnOpcode>(op_code);
        inst.src0.type = OperandType::InlineConstant;
        inst.src0.raw_value = dword & 0xFFFF;
        inst.dword_count = static_cast<u32>(curr - dword_offset + 1);
        return inst;
    }

    // 2. SOP1 (Scalar ALU 1 in, 1 out)
    // Prefix 0xBE800000 (bits 31..23 = 0b101111101)
    if ((dword & 0xFF800000) == 0xBE800000) {
        inst.encoding = InstructionEncoding::SOP1;
        u8 op_code = static_cast<u8>((dword >> 8) & 0xFF);
        inst.opcode = static_cast<GcnOpcode>(0x180 + op_code);
        inst.dst.type = OperandType::SGPR;
        inst.dst.reg_index = (dword >> 16) & 0x7F;
        inst.src0 = decode_src_operand(dword & 0xFF, bytecode, curr);
        inst.dword_count = static_cast<u32>(curr - dword_offset + 1);
        return inst;
    }

    // 3. SOP2 (Scalar ALU 2 in, 1 out)
    // Prefix: (dword & 0xE0000000) == 0x80000000
    if ((dword & 0xE0000000) == 0x80000000) {
        inst.encoding = InstructionEncoding::SOP2;
        u8 op_code = static_cast<u8>((dword >> 23) & 0x7F);
        inst.opcode = static_cast<GcnOpcode>(0x100 + op_code);
        inst.dst.type = OperandType::SGPR;
        inst.dst.reg_index = (dword >> 16) & 0x7F;
        inst.src0 = decode_src_operand(dword & 0xFF, bytecode, curr);
        inst.src1 = decode_src_operand((dword >> 8) & 0xFF, bytecode, curr);
        inst.dword_count = static_cast<u32>(curr - dword_offset + 1);
        return inst;
    }

    // 4. VOP1 (Vector ALU 1 in, 1 out)
    // Prefix: (dword & 0xFE000000) == 0x7E000000
    if ((dword & 0xFE000000) == 0x7E000000) {
        inst.encoding = InstructionEncoding::VOP1;
        u8 op_code = static_cast<u8>((dword >> 9) & 0xFF);
        inst.opcode = static_cast<GcnOpcode>(0x300 + op_code);
        inst.dst.type = OperandType::VGPR;
        inst.dst.reg_index = (dword >> 17) & 0xFF;
        inst.src0 = decode_src_operand(dword & 0x1FF, bytecode, curr);
        inst.dword_count = static_cast<u32>(curr - dword_offset + 1);
        return inst;
    }

    // 5. VOP2 (Vector ALU 2 in, 1 out)
    // Prefix: bit 31 = 0
    if ((dword & 0x80000000) == 0x00000000) {
        inst.encoding = InstructionEncoding::VOP2;
        u8 op_code = static_cast<u8>((dword >> 25) & 0x3F);
        inst.opcode = static_cast<GcnOpcode>(0x400 + op_code);
        inst.dst.type = OperandType::VGPR;
        inst.dst.reg_index = (dword >> 17) & 0xFF;
        inst.src0 = decode_src_operand(dword & 0x1FF, bytecode, curr);
        inst.src1.type = OperandType::VGPR;
        inst.src1.reg_index = (dword >> 9) & 0xFF;
        inst.dword_count = static_cast<u32>(curr - dword_offset + 1);
        return inst;
    }

    // 6. EXP (Export, 64-bit / 2 dwords)
    // Prefix: (dword & 0xFC000000) == 0xF8000000
    if ((dword & 0xFC000000) == 0xF8000000) {
        if (dword_offset + 1 >= bytecode.size()) {
            return ErrorCode::InvalidParameter;
        }
        u32 dword1 = bytecode[dword_offset + 1];

        inst.encoding = InstructionEncoding::EXP;
        inst.opcode = GcnOpcode::EXP;
        inst.dword_count = 2;

        inst.exp_en_mask = static_cast<u8>(dword & 0x0F);
        inst.exp_target = static_cast<ExportTarget>((dword >> 4) & 0x3F);
        inst.exp_compressed = ((dword >> 10) & 1) != 0;
        inst.exp_done = ((dword >> 11) & 1) != 0;

        for (int c = 0; c < 4; ++c) {
            inst.exp_src[c].type = OperandType::VGPR;
            inst.exp_src[c].reg_index = (dword1 >> (c * 8)) & 0xFF;
        }

        return inst;
    }

    // Default fallback NOP
    inst.encoding = InstructionEncoding::Unknown;
    inst.opcode = GcnOpcode::S_NOP;
    inst.dword_count = 1;
    return inst;
}

std::vector<GcnInstruction> GcnDecoder::disassemble_program(std::span<const u32> bytecode) {
    std::vector<GcnInstruction> program;
    size_t offset = 0;

    while (offset < bytecode.size()) {
        auto inst_res = decode_instruction(bytecode, offset);
        if (!inst_res) {
            break;
        }
        program.push_back(*inst_res);
        offset += inst_res->dword_count;

        if (inst_res->opcode == GcnOpcode::S_ENDPGM) {
            break;
        }
    }

    log::debug("GCN_ISA", "Disassembled {} instructions from {} dwords bytecode",
               program.size(), offset);
    return program;
}

} // namespace papaya::gpu::isa
