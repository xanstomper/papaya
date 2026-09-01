#pragma once

#include "papaya/common/types.hpp"
#include <span>
#include <string>
#include <vector>

// In-process GLSL -> SPIR-V compilation (Stage 4c).
//
// Drives the glslang library (the same "drive existing compiler libs"
// approach as the rest of the shader pipeline) to turn the GLSL emitted by
// sm4_emit_glsl_shader into real SPIR-V inside the process. Compiled when
// PAPAYA_HAS_GLSLANG is set (find the libs via -DPAPAYA_GLSLANG_ROOT=...);
// without it, the functions fail cleanly so tests can skip.

namespace papaya::gpu {

// Shader stage: 0 = vertex, 1 = fragment (matches D3D11 usage).
constexpr u32 kStageVertex = 0;
constexpr u32 kStageFragment = 1;

// Compile a GLSL shader (ES 3.10, as the emitter produces) to SPIR-V words.
// Returns false and fills `err` when the shader does not compile.
bool compile_glsl_to_spirv(const std::string& glsl, u32 stage,
                           std::vector<u32>& spirv, std::string& err);

// End-to-end: DXBC container bytes -> SPIR-V words (dxbc_to_glsl + compile).
bool dxbc_to_spirv(std::span<const u8> dxbc, u32 stage,
                   std::vector<u32>& spirv, std::string& err);

} // namespace papaya::gpu