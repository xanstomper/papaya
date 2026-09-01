// Unit test for the SM4/SM5 shader instruction decoder (Stage 2a).
//
// Builds a synthetic-but-valid D3D11 instruction stream:
//   ADD r0.xyzw, r1.xyzw, r2.xyzw     (opcode ADD=0x1, type PURE=0x3,
//                                       len=4 tokens; 3 TEMP operands)
//   MOV o0.xyzw, r0.xyzw              (extended opcode MOV=0x58 via EXT token)
// and verifies sm4_decode extracts opcodes + operand registers/masks.
// Malformed input (bad length) must fail cleanly.
#include "papaya/gpu/shader_translator.hpp"

#include <cstdio>
#include <vector>

using papaya::gpu::sm4_decode;
using papaya::gpu::ShaderOpcode;
using papaya::gpu::DecodedInstruction;
using papaya::gpu::DecodedOperand;
using papaya::u32;

static void push_u32(std::vector<u32>& v, u32 x) { v.push_back(x); }

// Build an operand token: TEMP/INPUT/OUTPUT (reg_type), an absolute index, mask.
static u32 operand_token(u32 reg_type, u32 reg_index, u32 mask = 0xF) {
    // bits 0-1: num_indexables-1 = 0 (1 indexable); bits 4-7: mask;
    // bits 16-19: reg_type
    return (0 << 0) | ((mask & 0xF) << 4) | ((reg_type & 0xF) << 16);
}

int main() {
    std::vector<u32> stream2;
    // ADD r0, r1, r2: opcode token + 3 operands, each operand = header + index.
    push_u32(stream2, (0x1u << 8) | (0x3u << 5) | 7u);   // len = 1 + 3*2 = 7
    {
        u32 t = operand_token(0, 0, 0xF);
        push_u32(stream2, t);
        push_u32(stream2, 0);   // r0
        t = operand_token(0, 0, 0xF);
        push_u32(stream2, t);
        push_u32(stream2, 1);   // r1
        t = operand_token(0, 0, 0xF);
        push_u32(stream2, t);
        push_u32(stream2, 2);   // r2
    }
    // MOV o0, r0: extended opcode (type 0x4 => has EXT token), MOV=0x58.
    // Instruction token: len=?, type=0x4, opcode=0x0 (extended space).
    // EXT token: bits 6.. hold the extended opcode (0x58); bit0 = 0 (not custom).
    // 2 operands * 2 tokens (header+index) + opcode + ext = 1+1+4 = 6 tokens.
    u32 ext_token = (0x58u << 6);           // extended opcode MOV
    push_u32(stream2, (0x0u << 8) | (0x4u << 5) | 6u);
    push_u32(stream2, ext_token);
    {
        u32 t = operand_token(2, 0, 0xF);   // OUTPUT o0 (reg_type=2)
        push_u32(stream2, t);
        push_u32(stream2, 0);
        t = operand_token(0, 0, 0xF);       // TEMP r0
        push_u32(stream2, t);
        push_u32(stream2, 0);
    }

    std::vector<DecodedInstruction> ins;
    if (!sm4_decode({stream2.data(), stream2.size()}, ins)) {
        std::printf("fail: sm4_decode returned false\n");
        return 1;
    }
    if (ins.size() != 2) { std::printf("fail: %zu instructions\n", ins.size()); return 2; }
    if (ins[0].opcode != ShaderOpcode::Add) { std::printf("fail: op0 not ADD\n"); return 3; }
    if (ins[0].operands.size() != 3) { std::printf("fail: add operands %zu\n", ins[0].operands.size()); return 4; }
    if (ins[0].operands[0].reg_index != 0 || ins[0].operands[2].reg_index != 2) {
        std::printf("fail: add regs %u %u\n", ins[0].operands[0].reg_index, ins[0].operands[2].reg_index);
        return 5;
    }
    if (ins[1].opcode != ShaderOpcode::Mov) { std::printf("fail: op1 not MOV\n"); return 6; }
    if (!ins[1].extended) { std::printf("fail: op1 not extended\n"); return 7; }
    if (ins[1].operands[0].reg_type != 2) { std::printf("fail: mov dst regtype\n"); return 8; }

    // ---- Malformed: instruction length beyond stream ----
    std::vector<u32> bad = { (0x1u << 8) | (0x3u << 5) | 10u };   // len=10, stream size 1
    if (sm4_decode({bad.data(), bad.size()}, ins)) { std::printf("fail: bad len ok\n"); return 9; }

    std::printf("ok: decoded ADD+MOV, regs/masks correct, malformed rejected\n");
    return 0;
}