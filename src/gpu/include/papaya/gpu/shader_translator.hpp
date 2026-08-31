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

// FourCC helper ('SHEX' = 0x58454853 little-endian).
inline u32 fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(a) | (static_cast<u32>(b) << 8) |
           (static_cast<u32>(c) << 16) | (static_cast<u32>(d) << 24);
}

} // namespace papaya::gpu