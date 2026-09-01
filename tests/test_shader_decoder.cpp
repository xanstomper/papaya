// Unit test for the SM4/SM5 instruction decoder (Stage 2b).
//
// Builds synthetic instructions encoded in the REAL on-disk token format
// (verified against wine vkd3d-shader tpf.c, byte-exact with native fxc):
//   instruction token: (length << 24) | (flags << 11) | opcode
//   operand token: register_type << 12 | order << 20 | addressing << (22+3k)
//                  | dim (bits 0-1) | mask/swizzle (bits 4-7/4-11)
//   index words follow for each indexable; immconst registers carry data words.
//
// Covered: MOV (dst TEMP r0 <- src INPUT v0), ADD with an immconst vec4,
// dcl_temps payload, relative addressing (x0[r0.x]), opcode-name/arity lookup,
// and malformed streams (length 0, length past end, operand past instruction).
#include "papaya/gpu/shader_translator.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using papaya::gpu::sm4_decode;
using papaya::gpu::sm4_opcode_info;
using papaya::gpu::sm4_opcode_name;
using papaya::gpu::ShaderOpcode;
using papaya::gpu::Sm4RegType;
using papaya::gpu::DecodedInstruction;
using papaya::gpu::DecodedOperand;
using papaya::u32;

static u32 inst(u32 opcode, u32 len, u32 flags = 0) {
    return ((len & 0x1Fu) << 24) | ((flags & 0x7u) << 11) | (opcode & 0xFFu);
}
static u32 operand(u32 reg_type, u32 order = 1, u32 dim = 3, u32 mask = 0,
                   u32 addr0 = 0, u32 addr1 = 0, u32 addr2 = 0) {
    return (reg_type << 12) | ((order & 0x3) << 20) | ((addr0 & 0x3) << 22) |
           ((addr1 & 0x3) << 25) | ((addr2 & 0x3) << 28) | (dim & 0x3) |
           ((mask & 0xF) << 4);
}
static u32 swizzle(u32 x, u32 y, u32 z, u32 w) {
    return (x & 3) << 4 | (y & 3) << 6 | (z & 3) << 8 | (w & 3) << 10;
}
static u32 fbits(float f) { u32 b; std::memcpy(&b, &f, 4); return b; }

int main() {
    std::vector<u32> s;

    // mov r0.xyzw, v0.xyzw
    s.push_back(inst(0x36, 5));                          // MOV, len 5
    s.push_back(operand(0x00, 1, 3, 0xF));               // TEMP r0, mask xyzw
    s.push_back(0);                                      // r0
    s.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));  // INPUT v0
    s.push_back(0);                                      // v0

    // add r1.xyzw, r0.xyzw, l(1, 2, 3, 4)   (saturate flag 0x4)
    s.push_back(inst(0x00, 10, 0x4));                    // ADD, len 10, saturate
    s.push_back(operand(0x00, 1, 3, 0xF));               // TEMP r1
    s.push_back(1);
    s.push_back(operand(0x00, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));  // TEMP r0
    s.push_back(0);
    s.push_back(operand(0x04, 0, 3));                    // IMMCONST vec4
    s.push_back(fbits(1.0f));
    s.push_back(fbits(2.0f));
    s.push_back(fbits(3.0f));
    s.push_back(fbits(4.0f));

    // dcl_temps 4
    s.push_back(inst(0x68, 2));                          // DCL_TEMPS, len 2
    s.push_back(4);

    // mov r3.x, x0[r0.x]   (indexable temp, relative addressing)
    s.push_back(inst(0x36, 5));                          // MOV, len 5
    s.push_back(operand(0x00, 1, 0, 0x1));               // TEMP r3, mask .x
    s.push_back(3);
    s.push_back(operand(0x03, 1, 3, 0, 2));              // INDEXABLE_TEMP x0, rel
    s.push_back(operand(0x00, 0, 3) | swizzle(0, 0, 0, 0));  // r0.x

    std::vector<DecodedInstruction> ins;
    if (!sm4_decode({s.data(), s.size()}, ins)) { std::printf("fail: decode\n"); return 1; }
    if (ins.size() != 4) { std::printf("fail: %zu instructions\n", ins.size()); return 2; }

    // --- mov r0, v0 ---
    if (ins[0].opcode != ShaderOpcode::Mov || std::strcmp(sm4_opcode_name(ins[0].opcode), "mov"))
        return 3;
    if (ins[0].operands.size() != 2 || ins[0].operands[0].reg_type != 0x00 ||
        ins[0].operands[0].reg_index() != 0 || ins[0].operands[0].mask != 0xF)
        return 4;
    const DecodedOperand& src = ins[0].operands[1];
    if (src.reg_type != 0x01 || src.reg_index() != 0 || src.swizzle != 0xE4u)
        return 5;   // stored swizzle = token bits 4-11 >> 4; 0xE4 = comps x,y,z,w

    // --- add r1, r0, l(1,2,3,4) with saturate ---
    if (ins[1].opcode != ShaderOpcode::Add || ins[1].flags != 0x4)
        return 6;
    if (ins[1].operands.size() != 3 || ins[1].operands[2].reg_type != 0x04)
        return 7;
    const auto& imm = ins[1].operands[2].imm;
    if (imm.size() != 4 || std::fabs(std::bit_cast<float>(imm[0]) - 1.0f) > 1e-6f ||
        std::fabs(std::bit_cast<float>(imm[3]) - 4.0f) > 1e-6f)
        return 8;

    // --- dcl_temps 4 ---
    if (ins[2].opcode != ShaderOpcode::DclTemps || ins[2].raw_words.size() != 1 ||
        ins[2].raw_words[0] != 4)
        return 9;

    // --- mov r3.x, x0[r0.x] (relative) ---
    if (ins[3].operands.size() != 2 || ins[3].operands[1].rel.size() != 1)
        return 10;
    const auto& rel = ins[3].operands[1].rel[0];
    if (rel.reg_type != 0x00 || rel.reg_index != 0 || rel.offset != 0)
        return 11;

    // --- opcode info table ---
    const auto* add_info = sm4_opcode_info(0x00);
    if (!add_info || std::strcmp(add_info->name, "add") || add_info->dst_count != 1 ||
        add_info->src_count != 2)
        return 12;
    const auto* mov_info = sm4_opcode_info(0x36);
    if (!mov_info || std::strcmp(mov_info->name, "mov") || mov_info->dst_count != 1 ||
        mov_info->src_count != 1)
        return 13;
    const auto* dcl_info = sm4_opcode_info(0x68);
    if (!dcl_info || std::strcmp(dcl_info->name, "dcl_temps"))
        return 14;
    if (!sm4_opcode_info(0xea))   // check_access_fully_mapped (last entry)
        return 15;
    if (sm4_opcode_info(0x04) || sm4_opcode_info(0x05) || sm4_opcode_info(0x52) ||
        sm4_opcode_info(0xff))    // reserved opcodes -> unknown
        return 16;
    if (std::strcmp(sm4_opcode_name(ShaderOpcode::Unknown), "")) return 17;

    // --- malformed streams ---
    std::vector<u32> bad0 = { inst(0x36, 0) };                 // length 0
    if (sm4_decode({bad0.data(), bad0.size()}, ins)) return 18;
    std::vector<u32> bad1 = { inst(0x36, 9), 0, 0 };           // length past end
    if (sm4_decode({bad1.data(), bad1.size()}, ins)) return 19;
    std::vector<u32> bad2 = { inst(0x36, 3),                    // operand needs
                              operand(0x00, 1, 3, 0xF) };       // an index word
    if (sm4_decode({bad2.data(), bad2.size()}, ins)) return 20;
    std::vector<u32> empty;
    if (sm4_decode({empty.data(), empty.size()}, ins)) return 21;

    std::printf("ok: mov/add/dcl_temps/relative decode, info table, malformed rejected\n");
    return 0;
}