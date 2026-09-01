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
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

using papaya::gpu::sm4_decode;
using papaya::gpu::sm4_emit_glsl;
using papaya::gpu::sm4_emit_glsl_shader;
using papaya::gpu::sm4_opcode_info;
using papaya::gpu::sm4_opcode_name;
using papaya::gpu::ShaderOpcode;
using papaya::gpu::Sm4RegType;
using papaya::gpu::DecodedInstruction;
using papaya::gpu::DecodedOperand;
using papaya::u32;

static u32 inst(u32 opcode, u32 len = 0, u32 flags = 0) {
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

    // --- GLSL emission (Stage 2c): decode+emit a small ALU shader ---
    std::vector<u32> g;
    // dcl_input v0
    g.push_back(inst(0x5F, 3));
    g.push_back(operand(0x01, 1, 3, 0xF));
    g.push_back(0);
    // dcl_output o0
    g.push_back(inst(0x65, 3));
    g.push_back(operand(0x02, 1, 3, 0xF));
    g.push_back(0);
    // dcl_temps 2
    g.push_back(inst(0x68, 2));
    g.push_back(2);
    // mov r0.xyzw, v0.xyzw
    g.push_back(inst(0x36, 5));
    g.push_back(operand(0x00, 1, 3, 0xF));
    g.push_back(0);
    g.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));
    g.push_back(0);
    // mul r1.xy, r0.xyzw, l(2,2,2,2)
    g.push_back(inst(0x38, 10));
    g.push_back(operand(0x00, 1, 3, 0x3));               // TEMP r1, mask .xy
    g.push_back(1);
    g.push_back(operand(0x00, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));
    g.push_back(0);
    g.push_back(operand(0x04, 0, 3) | swizzle(0, 1, 2, 3));   // IMMCONST vec4
    g.push_back(fbits(2.0f));
    g.push_back(fbits(2.0f));
    g.push_back(fbits(2.0f));
    g.push_back(fbits(2.0f));
    // add o0.xyzw, r0.xyzw, r1.xyzw  (saturate)
    g.push_back(inst(0x00, 7, 0x4));
    g.push_back(operand(0x02, 1, 3, 0xF));
    g.push_back(0);
    g.push_back(operand(0x00, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));
    g.push_back(0);
    g.push_back(operand(0x00, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));
    g.push_back(1);

    std::vector<DecodedInstruction> gins;
    if (!sm4_decode({g.data(), g.size()}, gins)) return 22;
    if (gins.size() != 6) return 23;
    std::string glsl;
    if (!sm4_emit_glsl({gins.data(), gins.size()}, glsl)) return 24;
    const auto has = [&](const char* what) {
        return glsl.find(what) != std::string::npos;
    };
    if (!has("vec4 v0;") || !has("vec4 o0;") || !has("vec4 r0;") || !has("vec4 r1;")) return 25;
    if (!has("r0 = v0;")) return 26;
    if (!has("r1.xy = (r0.xy * vec4(2.0, 2.0, 2.0, 2.0).xy);")) return 27;
    if (!has("o0 = clamp((r0 + r1), 0.0, 1.0);")) return 28;
    // unsupported opcode (ld) must make emission fail, not emit garbage
    std::vector<DecodedInstruction> lins;
    std::vector<u32> ld = { inst(0x2D, 7) };                 // ld r0, t0, v0
    ld.push_back(operand(0x00, 1, 3, 0xF));                  // dst TEMP r0
    ld.push_back(0);
    ld.push_back(operand(0x07, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));  // RESOURCE t0
    ld.push_back(0);
    ld.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));  // INPUT v0
    ld.push_back(0);
    if (!sm4_decode({ld.data(), ld.size()}, lins)) return 29;
    std::string junk;
    if (sm4_emit_glsl({lins.data(), lins.size()}, junk)) return 30;

    // --- GLSL control flow (Stage 2d) ---
    // if (v0.x) { r0 = v0; } else { r0 = v1; }
    // loop { if (v0.y) break; continue; }
    // discard;  ret
    std::vector<u32> cf;
    cf.push_back(inst(0x5F, 3));                       // dcl_input v0
    cf.push_back(operand(0x01, 1, 3, 0xF)); cf.push_back(0);
    cf.push_back(inst(0x5F, 3));                       // dcl_input v1
    cf.push_back(operand(0x01, 1, 3, 0xF)); cf.push_back(1);
    cf.push_back(inst(0x68, 2));                       // dcl_temps 1
    cf.push_back(1);
    cf.push_back(inst(0x1F, 3));                       // if v0.x
    cf.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 0, 0, 0));
    cf.push_back(0);
    cf.push_back(inst(0x36, 5));                       // mov r0, v0
    cf.push_back(operand(0x00, 1, 3, 0xF)); cf.push_back(0);
    cf.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); cf.push_back(0);
    cf.push_back(inst(0x12, 1));                          // else
    cf.push_back(inst(0x36, 5));                       // mov r0, v1
    cf.push_back(operand(0x00, 1, 3, 0xF)); cf.push_back(0);
    cf.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); cf.push_back(1);
    cf.push_back(inst(0x15, 1));                          // endif
    cf.push_back(inst(0x30, 1));                          // loop
    cf.push_back(inst(0x03, 3));                       // breakc v0.y
    cf.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(1, 1, 1, 1));
    cf.push_back(0);
    cf.push_back(inst(0x07, 1));                          // continue
    cf.push_back(inst(0x16, 1));                          // endloop
    cf.push_back(inst(0x0D, 3));                       // discard v0.x
    cf.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 0, 0, 0));
    cf.push_back(0);
    cf.push_back(inst(0x3E, 1));                          // ret

    std::vector<DecodedInstruction> cins;
    if (!sm4_decode({cf.data(), cf.size()}, cins)) return 31;
    {
        std::string gls;
        if (!sm4_emit_glsl({cins.data(), cins.size()}, gls)) return 32;
        const auto has = [&](const char* what) { return gls.find(what) != std::string::npos; };
        if (!has("if (any(notEqual(v0.xxxx, vec4(0.0)))) {")) return 33;
        if (!has("} else {")) return 34;
        if (!has("for (;;) {")) return 35;
        if (!has("if (any(notEqual(v0.yyyy, vec4(0.0)))) break;")) return 36;
        if (!has("discard;")) return 37;
        if (!has("continue;")) return 42;
        if (!has("    r0 = v0;") || !has("    r0 = v1;")) return 38;   // indented body
    }
    // switch must be refused, not silently dropped
    std::vector<u32> sw = { inst(0x4C, 3) };           // switch v0.x
    sw.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 0, 0, 0));
    sw.push_back(0);
    sw.push_back(inst(0x06, 3));                       // case 5: immconst
    sw.push_back(operand(0x04, 0, 0));                 // scalar imm
    sw.push_back(5);
    sw.push_back(inst(0x17, 1));                          // endswitch
    std::vector<DecodedInstruction> sins;
    if (!sm4_decode({sw.data(), sw.size()}, sins)) return 39;
    if (sm4_emit_glsl({sins.data(), sins.size()}, junk)) return 40;

    // --- constant buffers (Stage 3a) ---
    // dcl_constantbuffer cb0[16]
    // mov r2.xyz, cb0[4].xyz
    // add r3.xy, cb0[4], cb1[2]
    std::vector<u32> cb;
    cb.push_back(inst(0x59, 4));                       // dcl_constantbuffer, len 4
    cb.push_back(operand(0x08, 2, 3, 0));              // CB reg, order 2
    cb.push_back(0);                                   // buffer 0
    cb.push_back(16);                                  // 16 vec4s
    cb.push_back(inst(0x68, 2));                       // dcl_temps 4 (r0..r3)
    cb.push_back(4);
    cb.push_back(inst(0x59, 4));                       // dcl_constantbuffer cb1[8]
    cb.push_back(operand(0x08, 2, 3, 0));
    cb.push_back(1);
    cb.push_back(8);
    cb.push_back(inst(0x36, 6));                       // mov r2.xyz, cb0[4].xyz
    cb.push_back(operand(0x00, 1, 3, 0x7));            // TEMP r2, mask .xyz
    cb.push_back(2);
    cb.push_back(operand(0x08, 2, 3, 0) | swizzle(0, 1, 2, 3));  // cb0 element 4
    cb.push_back(0);
    cb.push_back(4);
    cb.push_back(inst(0x00, 9));                       // add r3.xy, cb0[4], cb1[2]
    cb.push_back(operand(0x00, 1, 3, 0x3));            // TEMP r3, mask .xy
    cb.push_back(3);
    cb.push_back(operand(0x08, 2, 3, 0) | swizzle(0, 1, 2, 3));
    cb.push_back(0);
    cb.push_back(4);
    cb.push_back(operand(0x08, 2, 3, 0) | swizzle(0, 1, 2, 3));
    cb.push_back(1);
    cb.push_back(2);
    std::vector<DecodedInstruction> cbins;
    if (!sm4_decode({cb.data(), cb.size()}, cbins)) return 43;
    if (cbins.size() != 5) return 44;  // 2x dcl_cb + dcl_temps + mov + add
    {
        std::string gls;
        if (!sm4_emit_glsl({cbins.data(), cbins.size()}, gls)) return 45;
        const auto has = [&](const char* what) { return gls.find(what) != std::string::npos; };
        if (!has("uniform cb0_b { vec4 data[16]; } cb0;")) return 46;
        if (!has("r2.xyz = cb0.data[4].xyz;")) return 47;
        if (!has("r3.xy = (cb0.data[4].xy + cb1.data[2].xy);")) return 48;
    }

    // --- textures (Stage 3b) ---
    // dcl_resource t0, 2D;  dcl_sampler s0
    // sample r0.xyzw, v0.xy, t0, s0
    // ld r1.xyzw, v1.xyzw, t0
    std::vector<u32> tx;
    tx.push_back(inst(0x5F, 3));                       // dcl_input v0
    tx.push_back(operand(0x01, 1, 3, 0xF)); tx.push_back(0);
    tx.push_back(inst(0x5F, 3));                       // dcl_input v1
    tx.push_back(operand(0x01, 1, 3, 0xF)); tx.push_back(1);
    tx.push_back(inst(0x68, 2));                       // dcl_temps 2 (r0, r1)
    tx.push_back(2);
    tx.push_back(inst(0x58, 4, 3));                    // dcl_resource: type 2D = 3
    tx.push_back(operand(0x07, 1, 3, 0));              // RESOURCE t0
    tx.push_back(0);
    tx.push_back(0x55555555);                          // component types (unorm)
    tx.push_back(inst(0x5A, 3));                       // dcl_sampler s0
    tx.push_back(operand(0x06, 1, 3, 0));
    tx.push_back(0);
    tx.push_back(inst(0x45, 9));                       // sample
    tx.push_back(operand(0x00, 1, 3, 0xF));            // dst r0
    tx.push_back(0);
    tx.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));  // uv v0
    tx.push_back(0);
    tx.push_back(operand(0x07, 1, 3, 0));              // RESOURCE t0
    tx.push_back(0);
    tx.push_back(operand(0x06, 1, 3, 0));              // SAMPLER s0
    tx.push_back(0);
    tx.push_back(inst(0x2D, 7));                       // ld
    tx.push_back(operand(0x00, 1, 3, 0xF));            // dst r1
    tx.push_back(1);
    tx.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3));  // addr v1
    tx.push_back(1);
    tx.push_back(operand(0x07, 1, 3, 0));              // RESOURCE t0
    tx.push_back(0);
    std::vector<DecodedInstruction> txins;
    if (!sm4_decode({tx.data(), tx.size()}, txins)) return 50;
    {
        std::string gls;
        if (!sm4_emit_glsl({txins.data(), txins.size()}, gls)) return 51;
        const auto has = [&](const char* what) { return gls.find(what) != std::string::npos; };
        if (!has("uniform sampler2D t0;")) return 52;
        if (!has("r0 = texture(t0, v0.xy);")) return 53;
        if (!has("r1 = texelFetch(t0, ivec2(v1.xy), int(v1.z));")) return 54;
    }
    // 1D resource (type 2) must be refused, not silently mis-typed
    std::vector<u32> tx1d = { inst(0x58, 4, 2), operand(0x07, 1, 3, 0), 0, 0x55555555 };
    std::vector<DecodedInstruction> tx1;
    if (!sm4_decode({tx1d.data(), tx1d.size()}, tx1)) return 55;
    if (sm4_emit_glsl({tx1.data(), tx1.size()}, junk)) return 56;

    // --- sample variants (Stage 3c) ---
    // dcl_resource t0, 2D;  dcl_sampler s0 (default);  dcl_resource t1, 2D;
    // dcl_sampler s1 (comparison)
    // sample_b r2, v2.xy, t0, s0, l(0.5)
    // sample_lod r3, v3.xy, t0, s0, v4.x
    // sample_grad r4, v5.xy, t0, s0, v6.xy, v7.xy
    // sample_c r5, v8.xy, t1, s1, v9.x
    std::vector<u32> sv;
    for (u32 vi = 2; vi <= 9; ++vi) {                 // dcl_input v2..v9
        sv.push_back(inst(0x5F, 3));
        sv.push_back(operand(0x01, 1, 3, 0xF));
        sv.push_back(vi);
    }
    sv.push_back(inst(0x68, 2));                       // dcl_temps 6 (r0..r5)
    sv.push_back(6);
    sv.push_back(inst(0x58, 4, 3));                    // dcl_resource t0
    sv.push_back(operand(0x07, 1, 3, 0)); sv.push_back(0); sv.push_back(0x55555555);
    sv.push_back(inst(0x5A, 3));                       // dcl_sampler s0 (default)
    sv.push_back(operand(0x06, 1, 3, 0)); sv.push_back(0);
    sv.push_back(inst(0x58, 4, 3));                    // dcl_resource t1
    sv.push_back(operand(0x07, 1, 3, 0)); sv.push_back(1); sv.push_back(0x55555555);
    sv.push_back(inst(0x5A, 3, 1));                    // dcl_sampler s1 (comparison)
    sv.push_back(operand(0x06, 1, 3, 0)); sv.push_back(1);
    // sample_b
    sv.push_back(inst(0x4A, 11));
    sv.push_back(operand(0x00, 1, 3, 0xF)); sv.push_back(2);         // dst r2
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); sv.push_back(2);  // v2
    sv.push_back(operand(0x07, 1, 3, 0)); sv.push_back(0);          // t0
    sv.push_back(operand(0x06, 1, 3, 0)); sv.push_back(0);          // s0
    sv.push_back(operand(0x04, 0, 0)); sv.push_back(fbits(0.5f));   // bias l(0.5)
    // sample_lod
    sv.push_back(inst(0x48, 11));
    sv.push_back(operand(0x00, 1, 3, 0xF)); sv.push_back(3);         // dst r3
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); sv.push_back(3);  // v3
    sv.push_back(operand(0x07, 1, 3, 0)); sv.push_back(0);          // t0
    sv.push_back(operand(0x06, 1, 3, 0)); sv.push_back(0);          // s0
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 0, 0, 0)); sv.push_back(4);  // v4.x
    // sample_grad
    sv.push_back(inst(0x49, 13));
    sv.push_back(operand(0x00, 1, 3, 0xF)); sv.push_back(4);         // dst r4
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); sv.push_back(5);  // v5
    sv.push_back(operand(0x07, 1, 3, 0)); sv.push_back(0);          // t0
    sv.push_back(operand(0x06, 1, 3, 0)); sv.push_back(0);          // s0
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); sv.push_back(6);  // v6
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); sv.push_back(7);  // v7
    // sample_c
    sv.push_back(inst(0x46, 11));
    sv.push_back(operand(0x00, 1, 3, 0xF)); sv.push_back(5);         // dst r5
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3)); sv.push_back(8);  // v8
    sv.push_back(operand(0x07, 1, 3, 0)); sv.push_back(1);          // t1
    sv.push_back(operand(0x06, 1, 3, 0)); sv.push_back(1);          // s1
    sv.push_back(operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 0, 0, 0)); sv.push_back(9);  // v9.x
    std::vector<DecodedInstruction> svins;
    if (!sm4_decode({sv.data(), sv.size()}, svins)) return 57;
    {
        std::string gls;
        if (!sm4_emit_glsl({svins.data(), svins.size()}, gls)) return 58;
        const auto has = [&](const char* what) { return gls.find(what) != std::string::npos; };
        if (!has("uniform sampler2DShadow t1_shadow;")) return 59;
        if (!has("r2 = texture(t0, v2.xy, vec4(0.5).x);")) return 60;
        if (!has("r3 = textureLod(t0, v3.xy, v4.x);")) return 61;
        if (!has("r4 = textureGrad(t0, v5.xy, v6.xy, v7.xy);")) return 62;
        if (!has("r5 = vec4(texture(t1_shadow, vec3(v8.xy, v9.x)));")) return 63;
    }
    // sample_c with a default (non-comparison) sampler must be refused
    std::vector<u32> sc = {
        inst(0x58, 4, 3), operand(0x07, 1, 3, 0), 0, 0x55555555,
        inst(0x5A, 3), operand(0x06, 1, 3, 0), 0,                       // s0 default
        inst(0x46, 11), operand(0x00, 1, 3, 0xF), 0,
        operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 1, 2, 3), 0,
        operand(0x07, 1, 3, 0), 0,
        operand(0x06, 1, 3, 0), 0,
        operand(0x01, 1, 3, 0, 0, 0, 0) | swizzle(0, 0, 0, 0), 0 };    // ref
    std::vector<DecodedInstruction> scins;
    if (!sm4_decode({sc.data(), sc.size()}, scins)) return 64;
    if (sm4_emit_glsl({scins.data(), scins.size()}, junk)) return 65;

    // --- glslang validation (when GLSLANG_VALIDATOR is set) ---
    // Every complete-shader emission above must COMPILE to SPIR-V with the
    // real compiler: prove the emitted GLSL is not just string-matched.
    if (const char* validator = std::getenv("GLSLANG_VALIDATOR"); validator && *validator) {
        int n = 0;
        const struct { std::vector<DecodedInstruction>* ins; const char* name; } cases[] = {
            { &gins, "alu" }, { &cins, "flow" }, { &cbins, "cb" },
            { &txins, "textures" }, { &svins, "variants" },
        };
        for (const auto& c : cases) {
            std::string gls;
            if (!sm4_emit_glsl_shader({c.ins->data(), c.ins->size()}, gls)) return 66 + n;
            const std::string frag = std::string("/tmp/papaya_") + c.name + ".frag";
            const std::string spv = std::string("/tmp/papaya_") + c.name + ".spv";
            if (FILE* f = std::fopen(frag.c_str(), "w")) { std::fputs(gls.c_str(), f); std::fclose(f); }
            const std::string cmd = std::string(validator) + " -V -S frag -o " + spv + " " + frag +
                                    " >/tmp/papaya_glslang.log 2>&1";
            if (std::system(cmd.c_str()) != 0) {
                std::printf("fail: glslang rejected %s shader\n%s\n---log---\n", c.name, gls.c_str());
                FILE* f = std::fopen("/tmp/papaya_glslang.log", "r");
                if (f) { char buf[512]; while (std::fgets(buf, sizeof buf, f)) std::fputs(buf, stdout); std::fclose(f); }
                return 66 + n;
            }
            ++n;
        }
    }

    std::printf("ok: decode+info+emit+control flow+cb+textures, malformed/unsupported rejected\n");
    return 0;
}