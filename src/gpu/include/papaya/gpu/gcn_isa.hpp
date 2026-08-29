#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <span>
#include <string>
#include <vector>

namespace papaya::gpu::isa {

enum class InstructionEncoding : u8 {
    Unknown,
    SOPP,
    SOP1,
    SOP2,
    SOPC,
    SOPK,
    SMRD,
    VOP1,
    VOP2,
    VOP3A,
    VOP3B,
    VOPC,
    VINTRP,
    EXP,
    MIMG,
    MUBUF
};

enum class GcnOpcode : u16 {
    // SOPP
    S_NOP = 0x00,
    S_ENDPGM = 0x01,
    S_BRANCH = 0x02,
    S_CBRANCH_SCC0 = 0x04,
    S_CBRANCH_SCC1 = 0x05,
    S_CBRANCH_VCCZ = 0x06,
    S_CBRANCH_VCCNZ = 0x07,
    S_BARRIER = 0x0A,
    S_WAITCNT = 0x0C,

    // SOP2
    S_ADD_U32 = 0x100,
    S_SUB_U32 = 0x101,
    S_ADD_I32 = 0x102,
    S_SUB_I32 = 0x103,
    S_MIN_I32 = 0x106,
    S_MAX_I32 = 0x108,
    S_AND_B32 = 0x10E,
    S_OR_B32 = 0x10F,
    S_XOR_B32 = 0x110,
    S_MUL_I32 = 0x113,

    // SOP1
    S_MOV_B32 = 0x180,
    S_NOT_B32 = 0x184,

    // SMRD / SMEM
    S_LOAD_DWORD = 0x200,
    S_LOAD_DWORDX2 = 0x201,
    S_LOAD_DWORDX4 = 0x202,
    S_BUFFER_LOAD_DWORD = 0x208,

    // VOP1
    V_MOV_B32 = 0x301,
    V_CVT_F32_I32 = 0x305,
    V_CVT_I32_F32 = 0x308,
    V_RCP_F32 = 0x32A,
    V_RSQ_F32 = 0x32E,
    V_SQRT_F32 = 0x333,
    V_SIN_F32 = 0x335,
    V_COS_F32 = 0x336,
    V_NOT_B32 = 0x337,

    // VOP2
    V_CNDMASK_B32 = 0x400,
    V_ADD_F32 = 0x401,
    V_SUB_F32 = 0x402,
    V_SUBREV_F32 = 0x403,
    V_MUL_F32 = 0x408,
    V_MUL_I32_I24 = 0x409,
    V_MIN_F32 = 0x40F,
    V_MAX_F32 = 0x410,
    V_ADD_I32 = 0x419,
    V_SUB_I32 = 0x41A,
    V_LSHLREV_B32 = 0x420,
    V_LSHRREV_B32 = 0x421,
    V_AND_B32 = 0x423,
    V_OR_B32 = 0x424,
    V_XOR_B32 = 0x425,

    // VOP3
    V_MAD_F32 = 0x5C0,
    V_FMA_F32 = 0x5C1,

    // EXP
    EXP = 0x600,

    // MIMG
    IMAGE_SAMPLE = 0x720,
    IMAGE_SAMPLE_L = 0x724,
    IMAGE_LOAD = 0x700
};

enum class OperandType : u8 {
    SGPR,
    VGPR,
    LiteralConstant,
    InlineConstant,
    VCC,
    M0,
    EXEC
};

struct Operand {
    OperandType type{OperandType::LiteralConstant};
    u32 reg_index{0};
    f32 float_value{0.0f};
    u32 raw_value{0};
};

enum class ExportTarget : u8 {
    MRT0 = 0,
    MRT1 = 1,
    MRT2 = 2,
    MRT3 = 3,
    MRT4 = 4,
    MRT5 = 5,
    MRT6 = 6,
    MRT7 = 7,
    POS0 = 12,
    POS1 = 13,
    POS2 = 14,
    POS3 = 15,
    PARAM0 = 32
};

struct GcnInstruction {
    InstructionEncoding encoding{InstructionEncoding::Unknown};
    GcnOpcode opcode{GcnOpcode::S_NOP};
    u32 dword_count{1};

    Operand dst{};
    Operand src0{};
    Operand src1{};
    Operand src2{};

    // For EXP instructions
    ExportTarget exp_target{ExportTarget::POS0};
    u8 exp_en_mask{0x0F}; // Enabled channels (X, Y, Z, W)
    bool exp_done{false};
    bool exp_compressed{false};
    Operand exp_src[4]{};
};

class GcnDecoder {
public:
    static Result<GcnInstruction> decode_instruction(std::span<const u32> bytecode, size_t dword_offset);
    static std::vector<GcnInstruction> disassemble_program(std::span<const u32> bytecode);

private:
    static Operand decode_src_operand(u32 src_code, std::span<const u32> bytecode, size_t& current_offset);
};

} // namespace papaya::gpu::isa
