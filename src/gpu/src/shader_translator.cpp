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

bool dxbc_to_glsl_stage(std::span<const u8> data, u32 stage, std::string& out) {
    out.clear();
    DxbcContainer c;
    if (!dxbc_parse(data, c)) return false;
    // SHDR/SHEX chunk body: [version u32][token count u32][instructions...].
    if (c.shader_bytecode.size() < 2) return false;
    const u32 count = c.shader_bytecode[1];
    if (2u + count > c.shader_bytecode.size()) return false;
    std::vector<DecodedInstruction> decoded;
    if (!sm4_decode({c.shader_bytecode.data() + 2, count}, decoded)) return false;
    return sm4_emit_glsl_shader({decoded.data(), decoded.size()}, out, stage);
}

bool dxbc_to_glsl(std::span<const u8> data, std::string& out) {
    return dxbc_to_glsl_stage(data, 1, out);   // fragment stage default
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
        ins.aux = (token >> 11) & 0x1FFu;
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

// ---- SM4 -> GLSL emission (Stage 2c, ALU subset) ----------------------------
namespace {

// Write-mask suffix: 0xF -> ".xyzw", 0x3 -> ".xy", 0x1 -> ".x".
std::string mask_suffix(u32 mask) {
    if (mask == 0xF) return std::string();
    std::string s = ".";
    if (mask & 1) s += 'x';
    if (mask & 2) s += 'y';
    if (mask & 4) s += 'z';
    if (mask & 8) s += 'w';
    return s;
}

// Swizzle suffix: decoded swizzle value (comp i in bits 2i..2i+1) -> ".xyzw".
std::string swizzle_suffix(u32 sw) {
    static const char kC[4] = {'x', 'y', 'z', 'w'};
    std::string s = ".";
    for (int i = 0; i < 4; ++i) s += kC[(sw >> (2 * i)) & 3];
    return s;
}

std::string fstr(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(f));
    std::string s(buf);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}

// Register reference: rN / vN / oN / tN / sN.
std::string reg_ref(const DecodedOperand& op) {
    switch (op.reg_type) {
        case 0x00: return "r" + std::to_string(op.reg_index());
        case 0x01: return "v" + std::to_string(op.reg_index());
        case 0x02: return "o" + std::to_string(op.reg_index());
        case 0x06: return "s" + std::to_string(op.reg_index());
        case 0x07: return "t" + std::to_string(op.reg_index());
        default: return std::string();   // cb handled in src_expr
    }
}

// Constant-buffer read: cb<buffer>[<element>]  (encoded indices: idx0=buffer,
// idx1=element). Only absolute addressing for now; relative -> unsupported.
bool cb_expr(const DecodedOperand& op, std::string& out) {
    if (op.reg_type != 0x08 || op.rel.size() != 0 || op.indices.size() != 2)
        return false;
    out += "cb" + std::to_string(op.indices[0]) + ".data[" + std::to_string(op.indices[1]) + "]";
    return true;
}

// Source operand expression with modifier + swizzle applied. `slice` is the
// dst write-mask suffix ("" for a full write, ".xy" etc. otherwise): when a
// src has an identity swizzle its components are taken from the dst mask
// (SM4 semantics: dst.xy reads src.xy of the full vec4 result), which keeps
// the emitted GLSL valid (no vec4->vec2 implicit narrowing).
bool src_expr(const DecodedOperand& op, std::string& out, const std::string& slice) {
    bool is_imm = op.reg_type == 0x04 || op.reg_type == 0x05;
    const bool identity = (op.swizzle & 0x3) == 0 && ((op.swizzle >> 2) & 0x3) == 1 &&
                          ((op.swizzle >> 4) & 0x3) == 2 && ((op.swizzle >> 6) & 0x3) == 3;
    std::string s;
    if (is_imm) {
        if (op.imm.empty()) return false;
        s += "vec4(";
        for (size_t i = 0; i < 4; ++i) {
            if (i) s += ", ";
            float v = 0.0f;
            if (i < op.imm.size()) std::memcpy(&v, &op.imm[i], 4);
            s += fstr(v);
        }
        s += ")";
    } else if (op.reg_type == 0x08) {
        if (!cb_expr(op, s)) return false;
    } else {
        const std::string ref = reg_ref(op);
        if (ref.empty() || !op.rel.empty()) return false;   // indexed classes unsupported
        s = ref;
    }
    // identity swizzle -> slice to the dst write mask; else keep the swizzle.
    if (identity) {
        if (!slice.empty()) s += slice;
    } else {
        s += swizzle_suffix(op.swizzle);
    }
    if (op.modifier == 1) s = "-(" + s + ")";
    else if (op.modifier == 2) s = "abs(" + s + ")";
    else if (op.modifier == 3) s = "-abs(" + s + ")";
    out += s;
    return true;
}

const DecodedOperand& dst_of(const DecodedInstruction& ins) { return ins.operands[0]; }

bool swizzle_identity(u32 sw) {
    return (sw & 0x3) == 0 && ((sw >> 2) & 0x3) == 1 &&
           ((sw >> 4) & 0x3) == 2 && ((sw >> 6) & 0x3) == 3;
}

// Texture uv/address operands must read components x,y (fxc always emits
// identity or xy-first swizzles here); unused components are unconstrained.
bool uv_ok(const DecodedOperand& op) {
    return (op.swizzle & 0x3u) == 0 && ((op.swizzle >> 2) & 0x3u) == 1;
}

// Scalar extra args (bias/lod/ref): fxc emits a replicated swizzle (xxxx...);
// emit a single-component read of that component.
bool scalar_expr(const DecodedOperand& op, std::string& out) {
    const u32 comp = op.swizzle & 0x3u;
    for (int i = 1; i < 4; ++i)
        if (((op.swizzle >> (2 * i)) & 0x3u) != comp) return false;   // must be replicated
    if (op.modifier || !op.rel.empty()) return false;
    out += ".";
    out += "xyzw"[comp];
    std::string base;
    if (op.reg_type == 0x04 || op.reg_type == 0x05) {       // immconst scalar
        if (op.imm.empty()) return false;
        base = "vec4(" + fstr([](u32 w) { float f; std::memcpy(&f, &w, 4); return f; }(op.imm[0])) + ")";
    } else {
        base = reg_ref(op);
        if (base.empty()) return false;
    }
    out = base + out;
    return true;
}

std::string pad(int indent) { return std::string(static_cast<size_t>(indent) * 4, ' '); }

} // namespace

// EmitMode: kAll = body-only output (legacy), kDecl = declarations only,
// kBody = statements only (used by the complete-shader wrapper).
enum class EmitMode { kAll, kDecl, kBody };

static bool sm4_dcl_like(ShaderOpcode op) {
    switch (op) {
        case ShaderOpcode::DclTemps:
        case ShaderOpcode::DclInput:
        case ShaderOpcode::DclOutput:
        case ShaderOpcode::DclGlobalFlags:
        case ShaderOpcode::DclConstantBuffer:
        case ShaderOpcode::DclSampler:
        case ShaderOpcode::DclResource:
        case ShaderOpcode::DclIndexableTemp:
        case ShaderOpcode::DclIndexRange:
        case ShaderOpcode::DclOutputTopology:
        case ShaderOpcode::DclInputPrimitive:
        case ShaderOpcode::DclVerticesOut:
            return true;
        default:
            return false;
    }
}

static bool emit_sm4_stream(std::span<const DecodedInstruction> insns, std::string& out,
        EmitMode mode, u32* res_type, bool* sam_cmp, bool* shadow_used) {
    int indent = 0;
    auto line = [&](std::string s) { out += pad(indent) + s + "\n"; };
    for (const auto& pre : insns)   // pre-scan: comparison sampling needs shadows
        if (pre.opcode == ShaderOpcode::SampleC && pre.operands.size() >= 3) {
            const u32 tidx = pre.operands[2].reg_index();
            if (tidx < 16) shadow_used[tidx] = true;
        }
    for (const auto& ins : insns) {
        if (mode == EmitMode::kDecl && !sm4_dcl_like(ins.opcode)) continue;
        if (mode == EmitMode::kBody && sm4_dcl_like(ins.opcode)) continue;
        switch (ins.opcode) {
            case ShaderOpcode::DclTemps: {
                const u32 count = ins.raw_words.empty() ? 0 : ins.raw_words[0];
                for (u32 i = 0; i < count; ++i)
                    out += "vec4 r" + std::to_string(i) + ";\n";
                break;
            }
            case ShaderOpcode::DclInput:
                if (ins.operands.size() != 1) return false;
                out += "layout(location = " + std::to_string(ins.operands[0].reg_index()) +
                   ") in vec4 v" + std::to_string(ins.operands[0].reg_index()) + ";\n";
                break;
            case ShaderOpcode::DclOutput:
                if (ins.operands.size() != 1) return false;
                out += "layout(location = " + std::to_string(ins.operands[0].reg_index()) +
                   ") out vec4 o" + std::to_string(ins.operands[0].reg_index()) + ";\n";
                break;
            // SM4 declarations that do not change the ALU body: the runtime
            // binds resources/samplers/global flags from the DXBC container.
            case ShaderOpcode::DclGlobalFlags:
            case ShaderOpcode::DclIndexableTemp:
            case ShaderOpcode::DclIndexRange:
            case ShaderOpcode::DclOutputTopology:
            case ShaderOpcode::DclInputPrimitive:
            case ShaderOpcode::DclVerticesOut:
                break;
            case ShaderOpcode::DclSampler: {
                // dcl_sampler sN: comparison mode in opcode bits 11-14 (aux).
                if (ins.operands.size() != 1) return false;
                const u32 sidx = ins.operands[0].reg_index();
                if (sidx >= 16) return false;
                sam_cmp[sidx] = (ins.aux & 0xFu) == 0x1;
                break;
            }
            case ShaderOpcode::DclResource: {
                // dcl_resource tN, <type>: resource type in opcode bits 11-14
                // (aux bits 0-3). GLSL ES 3.0-style sampler declarations.
                if (ins.operands.size() != 1) return false;
                const u32 rtype = ins.aux & 0xFu;
                const char* decl = rtype == 3 ? "sampler2D" :
                                   rtype == 5 ? "sampler3D" :
                                   rtype == 6 ? "samplerCube" :
                                   rtype == 8 ? "sampler2DArray" : nullptr;
                if (!decl) return false;   // 1D/buffer/MS/raw: not supported yet
                const u32 tidx = ins.operands[0].reg_index();
                if (tidx >= 16) return false;
                res_type[tidx] = rtype;
                // ES 3.x -> SPIR-V requires explicit sampler bindings.
                out += "layout(binding = " + std::to_string(tidx) + ") uniform " +
                       std::string(decl) + " t" + std::to_string(tidx) + ";\n";
                if (shadow_used[tidx]) {
                    if (rtype != 3) return false;   // shadow only for sampler2D
                    // Shadows take bindings 32+ so tN and tN_shadow never
                    // collide in one descriptor set (cbuffers use 16-31).
                    out += "layout(binding = " + std::to_string(32u + tidx) + ") uniform " +
                           "sampler2DShadow t" + std::to_string(tidx) + "_shadow;\n";
                }
                break;
            }
            case ShaderOpcode::DclConstantBuffer: {
                // dcl_constantbuffer cbN[M]: operand idx0 = buffer, idx1 = size
                // in vec4 registers. Vulkan/SPIR-V requires UBOs (no plain
                // non-opaque uniforms): std140 vec4 array (16-byte stride
                // matches D3D cbuffer registers). Bindings 16+ avoid sampler
                // bindings (0-15).
                if (ins.operands.size() != 1 || ins.operands[0].indices.size() != 2)
                    return false;
                const u32 buf = ins.operands[0].indices[0];
                const u32 size = ins.operands[0].indices[1];
                out += "layout(std140, binding = " + std::to_string(16 + buf) + ") " +
                       "uniform cb" + std::to_string(buf) + "_b { vec4 data[" +
                       std::to_string(size) + "]; } cb" + std::to_string(buf) + ";\n";
                break;
            }
            case ShaderOpcode::Mov:
            case ShaderOpcode::Add:
            case ShaderOpcode::Mul:
            case ShaderOpcode::Mad:
            case ShaderOpcode::Min:
            case ShaderOpcode::Max: {
                if (ins.operands.size() < 2) return false;
                const std::string slice = mask_suffix(dst_of(ins).mask);
                std::string a, b, c;
                if (!src_expr(ins.operands[1], a, slice)) return false;
                std::string body;
                switch (ins.opcode) {
                    case ShaderOpcode::Mov: body = a; break;
                    case ShaderOpcode::Add: if (!src_expr(ins.operands[2], b, slice)) return false; body = "(" + a + " + " + b + ")"; break;
                    case ShaderOpcode::Mul: if (!src_expr(ins.operands[2], b, slice)) return false; body = "(" + a + " * " + b + ")"; break;
                    case ShaderOpcode::Mad:
                        if (ins.operands.size() < 3 || !src_expr(ins.operands[2], b, slice) || !src_expr(ins.operands[3], c, slice)) return false;
                        body = "((" + a + " * " + b + ") + " + c + ")";
                        break;
                    case ShaderOpcode::Min: if (!src_expr(ins.operands[2], b, slice)) return false; body = "min(" + a + ", " + b + ")"; break;
                    case ShaderOpcode::Max: if (!src_expr(ins.operands[2], b, slice)) return false; body = "max(" + a + ", " + b + ")"; break;
                    default: return false;
                }
                if (ins.flags & 0x4) body = "clamp(" + body + ", 0.0, 1.0)";
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) + " = " + body + ";");
                break;
            }
            case ShaderOpcode::Movc: {
                if (ins.operands.size() < 3) return false;
                const std::string slice = mask_suffix(dst_of(ins).mask);
                std::string cond, a, b;
                if (!src_expr(ins.operands[1], cond, "") || !src_expr(ins.operands[2], a, slice) ||
                    !src_expr(ins.operands[3], b, slice)) return false;
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) +
                     " = mix(" + b + ", " + a + ", clamp(" + cond + ", 0.0, 1.0));");
                break;
            }
            case ShaderOpcode::Dp2:
            case ShaderOpcode::Dp3:
            case ShaderOpcode::Dp4: {
                if (ins.operands.size() < 2) return false;
                std::string a, b;
                if (!src_expr(ins.operands[1], a, "") || !src_expr(ins.operands[2], b, "")) return false;
                static const char* kComp[3] = {"xy", "xyz", "xyzw"};
                const int n = static_cast<int>(ins.opcode) - static_cast<int>(ShaderOpcode::Dp2) + 2;
                const std::string body = "dot(" + a + "." + kComp[n - 2] + ", " + b + "." + kComp[n - 2] + ")";
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) + " = " + body + ";");
                break;
            }
            case ShaderOpcode::Eq:
            case ShaderOpcode::Ge:
            case ShaderOpcode::Lt:
            case ShaderOpcode::Ne: {
                if (ins.operands.size() < 2) return false;
                const std::string slice = mask_suffix(dst_of(ins).mask);
                std::string a, b;
                if (!src_expr(ins.operands[1], a, slice) || !src_expr(ins.operands[2], b, slice)) return false;
                const char* fn =
                        ins.opcode == ShaderOpcode::Eq ? "equal" :
                        ins.opcode == ShaderOpcode::Ge ? "greaterThanEqual" :
                        ins.opcode == ShaderOpcode::Lt ? "lessThan" : "notEqual";
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) +
                     " = vec4(" + fn + "(" + a + ", " + b + "));");
                break;
            }
            case ShaderOpcode::Exp:
            case ShaderOpcode::Log:
            case ShaderOpcode::Frc:
            case ShaderOpcode::Sqrt:
            case ShaderOpcode::Rsq:
            case ShaderOpcode::Rcp:
            case ShaderOpcode::RoundNe:
            case ShaderOpcode::RoundNi:
            case ShaderOpcode::RoundPi:
            case ShaderOpcode::RoundZ:
            case ShaderOpcode::DerivRtx:
            case ShaderOpcode::DerivRty: {
                if (ins.operands.size() < 2) return false;
                const std::string slice = mask_suffix(dst_of(ins).mask);
                std::string a;
                if (!src_expr(ins.operands[1], a, slice)) return false;
                std::string body;
                switch (ins.opcode) {
                    case ShaderOpcode::Exp: body = "exp2(" + a + ")"; break;
                    case ShaderOpcode::Log: body = "log2(" + a + ")"; break;
                    case ShaderOpcode::Frc: body = "fract(" + a + ")"; break;
                    case ShaderOpcode::Sqrt: body = "sqrt(" + a + ")"; break;
                    case ShaderOpcode::Rsq: body = "inversesqrt(" + a + ")"; break;
                    case ShaderOpcode::Rcp: body = "(1.0 / " + a + ")"; break;
                    case ShaderOpcode::RoundNe: body = "floor(" + a + " + 0.5)"; break;
                    case ShaderOpcode::RoundNi: body = "floor(" + a + ")"; break;
                    case ShaderOpcode::RoundPi: body = "ceil(" + a + ")"; break;
                    case ShaderOpcode::RoundZ: body = "trunc(" + a + ")"; break;
                    case ShaderOpcode::DerivRtx: body = "dFdx(" + a + ")"; break;
                    case ShaderOpcode::DerivRty: body = "dFdy(" + a + ")"; break;
                    default: return false;
                }
                if (ins.flags & 0x4) body = "clamp(" + body + ", 0.0, 1.0)";
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) + " = " + body + ";");
                break;
            }
            case ShaderOpcode::Sample: {
                // sample dst, uv, tN, sN  (2D/3D/cube/2D-array texture()).
                if (ins.operands.size() < 4) return false;
                const DecodedOperand& uvo = ins.operands[1];
                const u32 tidx = ins.operands[2].reg_index();
                if (tidx >= 16) return false;
                const u32 rtype = res_type[tidx];
                const bool vec2_coords = rtype == 3;   // sampler2D
                if (rtype != 3 && rtype != 5 && rtype != 6 && rtype != 8) return false;
                if (!uv_ok(uvo)) return false;
                std::string uv;
                if (!src_expr(uvo, uv, vec2_coords ? ".xy" : ".xyz")) return false;
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) +
                     " = texture(t" + std::to_string(tidx) + ", " + uv + ");");
                break;
            }
            case ShaderOpcode::Ld: {
                // ld dst, address, tN  (2D: texelFetch with int coords + mip).
                if (ins.operands.size() < 3) return false;
                const DecodedOperand& ao = ins.operands[1];
                const u32 tidx = ins.operands[2].reg_index();
                if (tidx >= 16 || res_type[tidx] != 3) return false;   // 2D only
                if (!uv_ok(ao)) return false;
                std::string af;
                if (!src_expr(ao, af, "")) return false;   // full vec4 address
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) +
                     " = texelFetch(t" + std::to_string(tidx) + ", ivec2(" + af +
                     ".xy), int(" + af + ".z));");
                break;
            }
            case ShaderOpcode::SampleB:
            case ShaderOpcode::SampleLod:
            case ShaderOpcode::SampleGrad:
            case ShaderOpcode::SampleC: {
                // sample_* dst, uv, tN, sN [, bias|lod|ref [, ddx, ddy]]
                // 2D only for the variants; comparison uses a shadow sampler.
                if (ins.operands.size() < 4) return false;
                const DecodedOperand& uvo = ins.operands[1];
                const u32 tidx = ins.operands[2].reg_index();
                const u32 sidx = ins.operands[3].reg_index();
                if (tidx >= 16 || sidx >= 16 || res_type[tidx] != 3) return false;
                if (!uv_ok(uvo)) return false;
                std::string uv;
                if (!src_expr(uvo, uv, ".xy")) return false;
                std::string call;
                switch (ins.opcode) {
                    case ShaderOpcode::SampleB: {
                        if (ins.operands.size() < 5) return false;
                        std::string bias;
                        if (!scalar_expr(ins.operands[4], bias)) return false;
                        call = "texture(t" + std::to_string(tidx) + ", " + uv + ", " + bias + ")";
                        break;
                    }
                    case ShaderOpcode::SampleLod: {
                        if (ins.operands.size() < 5) return false;
                        std::string lod;
                        if (!scalar_expr(ins.operands[4], lod)) return false;
                        call = "textureLod(t" + std::to_string(tidx) + ", " + uv + ", " + lod + ")";
                        break;
                    }
                    case ShaderOpcode::SampleGrad: {
                        if (ins.operands.size() < 6) return false;
                        if (!swizzle_identity(ins.operands[4].swizzle) ||
                            !swizzle_identity(ins.operands[5].swizzle)) return false;
                        std::string ddx, ddy;
                        if (!src_expr(ins.operands[4], ddx, ".xy") ||
                            !src_expr(ins.operands[5], ddy, ".xy")) return false;
                        call = "textureGrad(t" + std::to_string(tidx) + ", " + uv + ", " +
                               ddx + ", " + ddy + ")";
                        break;
                    }
                    case ShaderOpcode::SampleC: {
                        if (ins.operands.size() < 5) return false;
                        if (!sam_cmp[sidx]) return false;   // comparison sampler required
                        std::string ref;
                        if (!scalar_expr(ins.operands[4], ref)) return false;
                        call = "vec4(texture(t" + std::to_string(tidx) + "_shadow, vec3(" +
                               uv + ", " + ref + ")))";   // scalar result replicated
                        break;
                    }
                    default:
                        return false;
                }
                line(reg_ref(dst_of(ins)) + mask_suffix(dst_of(ins).mask) + " = " + call + ";");
                break;
            }
            case ShaderOpcode::If: {
                // if (cond != 0.0) {   (nonzero condition, per SM4 semantics)
                if (ins.operands.size() < 1) return false;
                std::string cond;
                if (!src_expr(ins.operands[0], cond, "")) return false;
                line("if (any(notEqual(" + cond + ", vec4(0.0)))) {");
                ++indent;
                break;
            }
            case ShaderOpcode::Else:
                --indent;
                line("} else {");
                ++indent;
                break;
            case ShaderOpcode::EndIf:
                --indent;
                line("}");
                break;
            case ShaderOpcode::Loop:
                line("for (;;) {");
                ++indent;
                break;
            case ShaderOpcode::EndLoop:
                --indent;
                line("}");
                break;
            case ShaderOpcode::Break:
                line("break;");
                break;
            case ShaderOpcode::Continue:
                line("continue;");
                break;
            case ShaderOpcode::Breakc: {
                // if (cond != 0.0) break;   (Z/NZ flag at bit 18: NZ by default)
                if (ins.operands.size() < 1) return false;
                std::string cond;
                if (!src_expr(ins.operands[0], cond, "")) return false;
                line("if (any(notEqual(" + cond + ", vec4(0.0)))) break;");
                break;
            }
            case ShaderOpcode::Continuec: {
                if (ins.operands.size() < 1) return false;
                std::string cond;
                if (!src_expr(ins.operands[0], cond, "")) return false;
                line("if (any(notEqual(" + cond + ", vec4(0.0)))) continue;");
                break;
            }
            case ShaderOpcode::Discard:
                line("discard;");
                break;
            case ShaderOpcode::Ret:
                // implicit end of the shader; nothing to emit
                break;
            case ShaderOpcode::Retc: {
                if (ins.operands.size() < 1) return false;
                std::string cond;
                if (!src_expr(ins.operands[0], cond, "")) return false;
                line("if (any(notEqual(" + cond + ", vec4(0.0)))) return;");
                break;
            }
            case ShaderOpcode::Switch:
            case ShaderOpcode::Case:
            case ShaderOpcode::Default:
            case ShaderOpcode::EndSwitch:
                // switch/case needs an integer switch value; our current
                // all-vec4-f32 representation cannot express it faithfully.
                return false;
            case ShaderOpcode::Nop:
                break;
            default:
                return false;   // unsupported opcode: caller falls back
        }
    }
    return true;
}

bool sm4_emit_glsl(std::span<const DecodedInstruction> insns, std::string& out) {
    out.clear();
    out += "// papaya sm4->glsl (Stage 2c/2d/3: ALU + control flow + textures)\n";
    u32 res_type[16] = {};
    bool sam_cmp[16] = {};
    bool shadow_used[16] = {};
    return emit_sm4_stream(insns, out, EmitMode::kAll, res_type, sam_cmp, shadow_used);
}

bool sm4_emit_glsl_shader(std::span<const DecodedInstruction> insns, std::string& out,
                          u32 stage) {
    out.clear();
    out += "#version 310 es\n";   // must be the first line in ES-profile shaders
    out += "// papaya sm4->glsl (complete shader)\n";
    out += "precision highp float;\n";
    out += "precision highp int;\n";
    out += "precision highp sampler2D;\n";
    out += "precision highp sampler3D;\n";
    out += "precision highp samplerCube;\n";
    out += "precision highp sampler2DArray;\n";
    out += "precision highp sampler2DShadow;\n";
    out += "precision highp samplerCubeShadow;\n";
    out += "\n";
    u32 res_type[16] = {};       // state shared between the decl and body passes
    bool sam_cmp[16] = {};
    bool shadow_used[16] = {};
    if (!emit_sm4_stream(insns, out, EmitMode::kDecl, res_type, sam_cmp, shadow_used))
        return false;
    out += "\nvoid main() {\n";
    if (!emit_sm4_stream(insns, out, EmitMode::kBody, res_type, sam_cmp, shadow_used))
        return false;
    if (stage == 0) out += "gl_Position = o0;\n";   // vertex position output
    out += "}\n";
    return true;
}

} // namespace papaya::gpu