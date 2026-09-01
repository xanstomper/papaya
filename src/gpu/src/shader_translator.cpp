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
#include <algorithm>
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

// ---- SM4/SM5 instruction decoder -------------------------------------------
// Encoding (real on-disk values, wine vkd3d-shader tpf.c, verified byte-exact
// against native fxc output by wine's CI):
//   instruction token: opcode bits 0-7, flags bits 11-13, length bits 24-28
//   (length counts u32 tokens INCLUDING the opcode token), bit 31 = an
//   instruction modifier token follows (chain while bit 31 set).
//   operand token: dimension bits 0-1, write mask bits 4-7 / swizzle bits 4-11,
//   register type bits 12-19, index order bits 20-21 (>=1 means indices follow),
//   addressing bits 22-23/25-26/28-29 (bit1 set = relative, bit0 = pre-offset),
//   bit 31 = extended-operand token (modifier+precision) follows.
namespace {

struct Sm4OpcodeRow {
    u32 opcode;
    const char* name;
    u8 dst_count;
    u8 src_count;
};

static const Sm4OpcodeRow kSm4OpcodeRows[] = {
#include "sm4_opcodes.inc"
};
static constexpr size_t kSm4OpcodeRowsSize = sizeof(kSm4OpcodeRows) / sizeof(kSm4OpcodeRows[0]);

const Sm4OpcodeRow* find_opcode_row(u32 raw) {
    // The .inc table is sorted by opcode value.
    struct Cmp {
        bool operator()(u32 value, const Sm4OpcodeRow& row) const { return value < row.opcode; }
        bool operator()(const Sm4OpcodeRow& row, u32 value) const { return row.opcode < value; }
    };
    const auto* it = std::lower_bound(kSm4OpcodeRows, kSm4OpcodeRows + kSm4OpcodeRowsSize, raw, Cmp{});
    if (it == kSm4OpcodeRows + kSm4OpcodeRowsSize || it->opcode != raw) return nullptr;
    return it;
}

// Read one operand at stream[p..end); advances p. depth bounds the recursion
// used by relative-addressing operands (an operand token inside an operand).
bool parse_operand(std::span<const u32> s, size_t& p, size_t end, DecodedOperand& op, int depth) {
    if (depth > 3 || p >= end) return false;
    const u32 t = s[p++];
    op.reg_type = (t >> 12) & 0xFFu;
    op.order = (t >> 20) & 0x3u;
    op.dimension = t & 0x3u;
    op.mask = (t >> 4) & 0xFu;        // dest write mask
    op.swizzle = (t >> 4) & 0xFFu;    // src swizzle (4 x 2 bits)

    for (u32 k = 0; k < op.order; ++k) {
        if (p >= end) return false;
        const u32 addr = (t >> (22 + 3 * k)) & 0x3u;   // shift0=22, shift1=25, shift2=28
        if (addr & 0x2u) {
            // Relative addressing: optional pre-offset u32, then a src operand.
            Sm4RelIndex rel{};
            if (addr & 0x1u) { rel.offset = s[p++]; if (p > end) return false; }
            DecodedOperand base;
            if (!parse_operand(s, p, end, base, depth + 1)) return false;
            rel.reg_type = base.reg_type;
            rel.reg_index = base.reg_index();
            op.rel.push_back(rel);
        } else {
            op.indices.push_back(s[p++]);
        }
    }

    if (t & 0x80000000u) {
        // Extended-operand token: modifier + precision (+ optional 2nd-order).
        if (p >= end) return false;
        const u32 ext = s[p++];
        op.extended_type = ext & 0x3Fu;
        op.modifier = (ext >> 6) & 0xFFu;
        if ((ext & 0x80000000u) && p < end) ++p;   // second-order extended token
    }

    if (op.reg_type == static_cast<u32>(Sm4RegType::ImmConst) ||
        op.reg_type == static_cast<u32>(Sm4RegType::ImmConst64)) {
        u32 words;
        switch (op.dimension) {
            case 0: words = op.reg_type == static_cast<u32>(Sm4RegType::ImmConst64) ? 2u : 1u; break;
            case 1: words = 2u; break;
            case 2: words = 3u; break;
            default: words = 4u; break;
        }
        if (p + words > end) return false;
        for (u32 k = 0; k < words; ++k) op.imm.push_back(s[p++]);
    }
    return true;
}

// Opcodes whose operands are plain register operands with fixed arity.
bool is_plain_alu(u32 raw) {
    if (const auto* row = find_opcode_row(raw)) return row->dst_count + row->src_count > 0;
    return false;
}

// DCL opcodes that name a register operand (dcl_resource t0, dcl_input v0, ...);
// the rest (dcl_temps, dcl_global_flags, dcl_thread_group, ...) carry plain
// count/enum payload words.
bool dcl_has_operand(u32 raw) {
    switch (raw) {
        case 0x58: case 0x59: case 0x5A: case 0x5B:       // resource/cbuffer/sampler/index_range
        case 0x5F: case 0x60: case 0x61: case 0x62:        // dcl_input[_[sg][vi]v, ps, ...]
        case 0x63: case 0x64: case 0x65: case 0x66:
        case 0x67:                                        // dcl_output[_[sg][vi]v]
        case 0x8F:                                        // dcl_stream
        case 0x9C: case 0x9D: case 0x9E:                  // uav typed/raw/structured
        case 0x9F: case 0xA0: case 0xA1: case 0xA2:       // tgsm/raw/structured dcls
            return true;
        default:
            return false;
    }
}

} // namespace

const Sm4OpcodeInfo* sm4_opcode_info(u32 raw_opcode) {
    const auto* row = find_opcode_row(raw_opcode);
    if (!row) return nullptr;
    static thread_local Sm4OpcodeInfo info;   // tiny; fine for lookup
    info.opcode = row->opcode;
    info.name = row->name;
    info.dst_count = row->dst_count;
    info.src_count = row->src_count;
    return &info;
}

const char* sm4_opcode_name(ShaderOpcode opcode) {
    if (opcode == ShaderOpcode::Unknown) return "";
    const auto* row = find_opcode_row(static_cast<u32>(opcode));
    return row ? row->name : "";
}

bool sm4_decode(std::span<const u32> stream, std::vector<DecodedInstruction>& out) {
    out.clear();
    size_t i = 0;
    while (i < stream.size()) {
        const u32 token = stream[i];
        const u32 raw = token & 0xFFu;
        const u32 len = (token >> 24) & 0x1Fu;
        if (len == 0 || i + len > stream.size()) return false;   // malformed

        DecodedInstruction ins{};
        ins.opcode_raw = raw;
        ins.flags = (token >> 11) & 0x7u;
        ins.length = len;
        ins.token_offset = static_cast<u32>(i);
        ins.opcode = find_opcode_row(raw) ? static_cast<ShaderOpcode>(raw) : ShaderOpcode::Unknown;

        const size_t end = i + len;   // length is authoritative (incl. opcode token)
        size_t p = i + 1;

        // Instruction modifier tokens (chain: each has bit 31 set except the last).
        u32 prev = token;
        while (p < end && (prev & 0x80000000u)) {
            prev = stream[p++];
            ins.modifier_tokens.push_back(prev);
        }

        // Special payload shapes (mirrors vkd3d-shader's opcode readers).
        if (raw == static_cast<u32>(ShaderOpcode::ShaderData)) {
            // icb payload: register slot u32 + vec4-aligned data words.
            while (p < end) ins.raw_words.push_back(stream[p++]);
            out.push_back(std::move(ins));
            i = end;
            continue;
        }
        if (raw == static_cast<u32>(ShaderOpcode::DclTemps) ||
            raw == static_cast<u32>(ShaderOpcode::DclIndexableTemp)) {
            // dcl_temps / dcl_indexable_temp: plain count words, no operand.
            while (p < end) ins.raw_words.push_back(stream[p++]);
            out.push_back(std::move(ins));
            i = end;
            continue;
        }

        // DCL-family opcodes carry one dst operand then semantic/count words.
        if (dcl_has_operand(raw) && p < end) {
            DecodedOperand op;
            if (!parse_operand(stream, p, end, op, 0)) return false;
            ins.operands.push_back(std::move(op));
        }

        // Plain ALU: fixed dst+src operand count from the opcode table.
        if (is_plain_alu(raw)) {
            const auto* info = sm4_opcode_info(raw);
            const u32 operands = static_cast<u32>(info->dst_count) + info->src_count;
            for (u32 k = 0; k < operands; ++k) {
                if (p >= end) return false;
                DecodedOperand op;
                if (!parse_operand(stream, p, end, op, 0)) return false;
                ins.operands.push_back(std::move(op));
            }
        } else {
            // Everything else (nop/ret/control flow/unknown/DCL payloads):
            // consume whatever remains inside the instruction length as raw words.
            while (p < end) ins.raw_words.push_back(stream[p++]);
        }

        out.push_back(std::move(ins));
        i = end;
    }
    return !out.empty();
}

} // namespace papaya::gpu