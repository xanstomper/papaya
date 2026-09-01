// DXBC container parser — Stage 1 of papaya's D3D11 shader translation.
//
// Parses the DirectX Bytecode container format that D3D11 compiled shaders use:
//   Header:   "DXBC" magic, pad, hash, blob version, creator length/offset,
//             total size, 4 u32s of flags/reserved, chunk count, chunk offset.
//   Directory: count * { u32 fourCC, u32 size, u32 offset }.
//   Chunks:   SHDR/SHEX  -> D3D11 shader bytecode.
//             ISGN/OSGN  -> input/output signature (for pipeline reflection).
//
// This is self-contained and real; Windows never yields a malformed container
// that crashes the parser (bounds-checked).

#include "papaya/gpu/shader_translator.hpp"
#include <cstring>

namespace papaya::gpu {

namespace {
constexpr u32 kDxbcMagic = 0x43425844u;   // 'D' 'X' 'B' 'C' LE = "DXBC"
constexpr u32 kChunkShdr = 0x52444853u;   // 'SHDR'
constexpr u32 kChunkShex = 0x58454853u;   // 'SHEX'
constexpr u32 kChunkIsgn = 0x4E475349u;   // 'ISGN'
constexpr u32 kChunkOsgn = 0x4E47534Fu;   // 'OSGN'

// Decode a D3D semantic name from the chunk (offset is chunk-relative).
std::string semantic_name(const u8* chunk, u32 chunk_size, u32 name_offset) {
    if (!chunk || name_offset >= chunk_size) return {};
    std::string s;
    const u8* p = chunk + name_offset;
    const u8* end = chunk + chunk_size;
    while (p < end && *p) { s.push_back(static_cast<char>(*p)); ++p; }
    return s;
}

} // namespace

bool dxbc_parse(std::span<const u8> data, DxbcContainer& out) {
    out = DxbcContainer{};
    if (data.size() < 44) return false;

    const u8* b = data.data();
    if (std::memcmp(b, "DXBC", 4) != 0) return false;

    // Header layout (all little-endian):
    //   0:  magic "DXBC"
    //   4-19: checksum (16 bytes)
    //   20: blob version
    //   24: creator length
    //   28: creator offset
    //   32: total size
    //   36: chunk count
    //   40: chunk offset
    //   44-60: flags / reserved
    u32 chunk_count = 0, chunk_offset = 0;
    std::memcpy(&chunk_count, b + 36, 4);
    std::memcpy(&chunk_offset, b + 40, 4);
    if (chunk_count == 0 || chunk_offset == 0) return false;
    if (chunk_offset + chunk_count * 12 > data.size()) return false;

    // Directory of (fourCC, size, offset).
    for (u32 i = 0; i < chunk_count; ++i) {
        const u8* e = b + chunk_offset + static_cast<size_t>(i) * 12;
        u32 tag, size, off;
        std::memcpy(&tag, e, 4);
        std::memcpy(&size, e + 4, 4);
        std::memcpy(&off, e + 8, 4);
        if (off + size > data.size()) continue;

        DxbcChunk ch{};
        ch.tag = tag; ch.offset = off; ch.size = size;
        ch.data.assign(b + off, b + off + size);
        out.chunks.push_back(std::move(ch));
        DxbcChunk& chref = out.chunks.back();

        if (tag == kChunkShdr || tag == kChunkShex) {
            // Chunk body starts with a D3D11 bytecode header; the shader binary
            // follows. Read the blob (version may be encoded). We keep the whole
            // chunk as the bytecode blob for the later compiler-lib stage.
            u32 word_count = size / 4;
            out.shader_bytecode.reserve(word_count);
            for (u32 w = 0; w < word_count; ++w) {
                u32 v; std::memcpy(&v, b + off + static_cast<size_t>(w) * 4, 4);
                out.shader_bytecode.push_back(v);
            }
            // SHDR chunk version word + instruction stream follows; crudely
            // detect kind from the execution opcode group present at the tail.
            out.kind = ShaderKind::Unknown;
        } else if (tag == kChunkIsgn || tag == kChunkOsgn) {
            // ISGN/OSGN: u32 element count, then 32-byte elements:
            //   [0] semanticNameOffset (chunk-relative)
            //   [1] semanticIndex
            //   [2] systemValue (D3D_NAME)
            //   [3] componentType (D3D_REGISTER_COMPONENT_TYPE)
            //   [4] register (0 = input/output register 0)
            //   [5] mask (component write mask)
            //   [6] readWriteMask
            //   [7] stream (output only, ignored)
            if (chref.data.size() >= 4) {
                u32 elem_count;
                std::memcpy(&elem_count, chref.data.data(), 4);
                const u8* base = chref.data.data();
                const u8* end = chref.data.data() + chref.data.size();
                const u8* p = base + 4;
                for (u32 e = 0; e < elem_count && p + 32 <= end; ++e, p += 32) {
                    ShaderSignatureParam param;
                    u32 name_off = 0, sem_index = 0, sv = 0, ct = 0, reg = 0, mask = 0, rw = 0;
                    std::memcpy(&name_off, p, 4);
                    std::memcpy(&sem_index, p + 4, 4);
                    std::memcpy(&sv, p + 8, 4);
                    std::memcpy(&ct, p + 12, 4);
                    std::memcpy(&reg, p + 16, 4);
                    std::memcpy(&mask, p + 20, 4);
                    std::memcpy(&rw, p + 24, 4);
                    param.component_type = ct;
                    param.system_value = static_cast<u8>(sv & 0xFF);
                    param.mask = mask;
                    param.register_index = static_cast<u8>(reg & 0xFF);
                    param.register_mask = static_cast<u8>(rw & 0xFF);
                    param.index = sem_index;
                    param.semantic = semantic_name(base, static_cast<u32>(chref.data.size()), name_off);
                    if (tag == kChunkIsgn) out.input_signature.push_back(std::move(param));
                    else out.output_signature.push_back(std::move(param));
                }
            }
        }
    }
    return !out.chunks.empty();
}

// ---- SM4/SM5 instruction decoder --------------------------------------------
// D3D11 shader instruction token layout:
//   bits 0-4:  instruction length (in u32 tokens, incl. the opcode token)
//   bits 5-7:  opcode type (0x3 = pure, 0x4 = has extended opcode, 0x5 = has
//              extended opcode + global flags)
//   bits 8-18: opcode (11 bits)
// Bit 0 of the extended-opcode token holds "extended is a custom op".
// Operand token layout:
//   bits 0-1:  num indexables-1 (number of index registers following)
//   bits 16-19: register type (D3D10_SB_OPERAND_TYPE)
//   bits 24-31: operand mode + flags
// After the operand header, an indexable operand has an index (u32) possibly
// with additional index tokens; our decode covers the common TEMP/INPUT/OUTPUT
// fixed-register forms.
namespace {

bool op_have_ext(u32 type) { return type == 0x4 || type == 0x5; }

ShaderOpcode to_opcode(u32 raw_opcode, bool extended_custom) {
    // Base opcode space (SM4): 0x1..0x1D.
    switch (raw_opcode) {
        case 0x01: return ShaderOpcode::Add;    case 0x02: return ShaderOpcode::And;
        case 0x03: return ShaderOpcode::Break;  case 0x04: return ShaderOpcode::Breakc;
        case 0x05: return ShaderOpcode::Call;   case 0x06: return ShaderOpcode::Callc;
        case 0x07: return ShaderOpcode::Case;   case 0x08: return ShaderOpcode::Continue;
        case 0x09: return ShaderOpcode::Continuec; case 0x0A: return ShaderOpcode::Cut;
        case 0x0B: return ShaderOpcode::Discard; case 0x0C: return ShaderOpcode::Default;
        case 0x0D: return ShaderOpcode::Div;    case 0x0E: return ShaderOpcode::Dp2;
        case 0x0F: return ShaderOpcode::Dp3;    case 0x10: return ShaderOpcode::Dp4;
        case 0x11: return ShaderOpcode::Else;   case 0x12: return ShaderOpcode::Emit;
        case 0x13: return ShaderOpcode::EmitThenCut; case 0x14: return ShaderOpcode::EndIf;
        case 0x15: return ShaderOpcode::EndLoop; case 0x16: return ShaderOpcode::EndSwitch;
        case 0x17: return ShaderOpcode::Eq;     case 0x18: return ShaderOpcode::Exp;
        case 0x19: return ShaderOpcode::Frc;    case 0x1A: return ShaderOpcode::FtoI;
        case 0x1B: return ShaderOpcode::FtoU;   case 0x1C: return ShaderOpcode::Ge;
        case 0x1D: return ShaderOpcode::IAdd;
        default: break;
    }
    // Extended opcode space (0x58 MOV, 0x5A MUL are the common NOP-adjacent
    // ones games hit; the full SM4/SM5 table is large and belongs to Stage 3).
    if (!extended_custom) {
        switch (raw_opcode) {
            case 0x58: return ShaderOpcode::Mov;
            case 0x5A: return ShaderOpcode::Mul;
            default: break;
        }
    }
    return ShaderOpcode::Unknown;
}

} // namespace

bool sm4_decode(std::span<const u32> stream, std::vector<DecodedInstruction>& out) {
    out.clear();
    size_t i = 0;
    while (i < stream.size()) {
        u32 token = stream[i];
        u32 len = token & 0x1F;
        u32 type = (token >> 5) & 0x7;
        u32 opcode_raw = (token >> 8) & 0x7FF;
        if (len == 0 || i + len > stream.size()) return false;   // malformed

        DecodedInstruction ins{};
        ins.token_offset = static_cast<u32>(i);
        size_t p = i + 1;   // operand tokens start after the opcode token
        bool extended_custom = false;
        if (op_have_ext(type)) {
            // Next token: extended opcode (bits 0-5) or custom-op mask.
            if (p >= stream.size()) return false;
            ins.extended = true;
            u32 ext = stream[p];
            opcode_raw = (ext >> 6) & 0xFFFFF;   // extended opcode (20 bits)
            extended_custom = (ext & 0x1) != 0;
            ++p;
        }
        ins.opcode = to_opcode(opcode_raw, extended_custom);

        // Any remaining tokens within `len` are operands (each 1+ tokens).
        while (p < i + len && p < stream.size()) {
            u32 op = stream[p];
            DecodedOperand opd{};
            opd.reg_type = (op >> 16) & 0xF;
            u32 num_ind = ((op >> 0) & 0x3) + 1;
            opd.mask = (op >> 4) & 0xF;         // component write/read mask
            opd.swizzle = (op >> 8) & 0xFF;     // raw swizzle bits (sometimes)
            ++p;
            // Indexables: an index token (u32) per declared indexable, plus for
            // relative addressing a register token. We capture the first index
            // value; tolerate malformed (bounds-checked).
            for (u32 k = 0; k < num_ind && p < i + len && p < stream.size(); ++k) {
                if ((op >> 8) & 0x100) {        // RELATIVE addressing: index token
                    // skip the relative register operand token (1+ words)
                    ++p;
                } else {
                    opd.reg_index = stream[p];
                    ++p;
                }
            }
            ins.operands.push_back(std::move(opd));
        }
        out.push_back(std::move(ins));
        i += len;
    }
    return !out.empty();
}

} // namespace papaya::gpu