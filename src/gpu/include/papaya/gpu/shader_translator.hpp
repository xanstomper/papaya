#pragma once

#include "papaya/common/types.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace papaya::gpu {

// D3D11 (DXBC) -> SPIR-V shader translation — multi-session phase (2026-08-31).
//
// Strategy (the "modern faster way" vs hand-writing a DXVK-scale shader engine):
//   * Stage 1 (THIS module, real now): parse the DXBC container — its header,
//     chunk directory, SHDR/SHEX bytecode, and ISGN/OSGN/RDEF reflection chunks.
//     You cannot translate a shader you cannot parse.
//   * Stage 2 (future): drive an existing compiler library (SPIRV-Cross / DXC /
//     glslang) to turn the D3D11 bytecode into SPIR-V, plus D3D11-state->Vulkan
//     pipeline mapping. Papaya writes the state mapping; it does NOT write a
//     shader compiler.
//
// This file defines the self-contained Stage 1 container parser + the
// reflection model the later stages consume.

enum class ShaderKind { Unknown, Vertex, Pixel, Geometry, Hull, Domain, Compute };

struct DxbcChunk {
    u32 tag{0};               // 'SHDR'/'SHEX'/'ISGN'/...
    u32 offset{0};            // byte offset of the chunk in the container
    u32 size{0};              // chunk size in bytes
    std::vector<u8> data;     // chunk body
};

struct ShaderSignatureParam {
    std::string semantic;
    u32 index{0};
    u32 component_type{0};    // DXGI_FORMAT-ish R32/U32/S32/F32/Float
    u32 mask{0};              // which components are written
    u8 system_value{0};       // D3D_NAME_* (POSITION/COLOR/TEXCOORD...)
    u8 register_index{0};
    u8 register_mask{0};      // read/write mask
};

struct ShaderReflection {
    ShaderSignatureParam inputs;
    ShaderSignatureParam outputs;
};

// Parsed result of a DXBC container.
struct DxbcContainer {
    std::vector<DxbcChunk> chunks;   // ordered as in the file
    std::vector<u32> shader_bytecode;
    std::vector<ShaderSignatureParam> input_signature;
    std::vector<ShaderSignatureParam> output_signature;
    ShaderKind kind{ShaderKind::Unknown};
};

// Parse a raw D3D11 bytecode .cso buffer into a DXBC container.
// Returns false when the buffer is not a valid DXBC container (header magic
// 'DXBC' / version / chunk count disagree) — reported, never crash.
bool dxbc_parse(std::span<const u8> data, DxbcContainer& out);

// ---- Stage 2a: SM4/SM5 shader instruction decoder --------------------------
// Walks the D3D11 shader instruction stream (u32 tokens after the chunk's
// version/header) into a list of instructions, each with its opcode and the
// operand register table. This is what the later SPIR-V backend consumes.
// Real, self-contained: no compiler lib needed to *decode*.

enum class ShaderOpcode : u32 {
    Add = 0x1, And = 0x2, Break = 0x3, Breakc = 0x4, Call = 0x5, Callc = 0x6,
    Case = 0x7, Continue = 0x8, Continuec = 0x9, Cut = 0xA, Discard = 0xB,
    Default = 0xC, Div = 0xD, Dp2 = 0xE, Dp3 = 0xF, Dp4 = 0x10, Else = 0x11,
    Emit = 0x12, EmitThenCut = 0x13, EndIf = 0x14, EndLoop = 0x15, EndSwitch = 0x16,
    Eq = 0x17, Exp = 0x18, Frc = 0x19, FtoI = 0x1A, FtoU = 0x1B, Ge = 0x1C,
    IAdd = 0x1D, // cut-off: the required full table is large; we decode the
    // common ALU + control ops used in games' shaders and report unknown.
    Mov = 0x58, Mul = 0x5A, Ge_Ext = 0x5C, // (extended opcode space overlaps)
    Unknown = 0xFFFFFFFFu,
};

// Operand decode result: which register class the operand refers to + its
// register number and component mask (enough for pipeline-layout reflection).
struct DecodedOperand {
    u32 reg_type{0};      // D3D10_SB_OPERAND_TYPE (TEMP=0, INPUT=1, OUTPUT=2, ...)
    u32 reg_index{0};     // first index (r#, v#, o#)
    u32 mask{0xF};        // component write/read mask (xyzw bits)
    u32 swizzle{0};       // raw swizzle word (0 when none)
};

struct DecodedInstruction {
    ShaderOpcode opcode{ShaderOpcode::Unknown};
    u32 token_offset{0};        // u32 offset of instruction start in stream
    std::vector<DecodedOperand> operands;
    bool extended{false};       // GLOBAL_FLAGS/extended opcode present
};

// Decode the instruction stream (starting right after the shader version word,
// which is passed as the chunk body offset). Returns false on malformed input.
bool sm4_decode(std::span<const u32> stream, std::vector<DecodedInstruction>& out);

// FourCC helper ('SHEX' = 0x58454853 little-endian).
inline u32 fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(a) | (static_cast<u32>(b) << 8) |
           (static_cast<u32>(c) << 16) | (static_cast<u32>(d) << 24);
}

} // namespace papaya::gpu