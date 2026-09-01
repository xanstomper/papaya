// In-process GLSL -> SPIR-V via glslang (see shader_compile.hpp).
#include "papaya/gpu/shader_compile.hpp"
#include "papaya/gpu/shader_translator.hpp"

#ifdef PAPAYA_HAS_GLSLANG
#include "glslang/Public/ShaderLang.h"
#include "SPIRV/SpvTools.h"
#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#endif

namespace papaya::gpu {

#ifdef PAPAYA_HAS_GLSLANG

namespace {

bool compile_impl(const std::string& glsl, u32 stage, std::vector<u32>& spirv,
                  std::string& err) {
    if (stage != kStageVertex && stage != kStageFragment) {
        err = "unsupported stage";
        return false;
    }
    const EShLanguage lang =
            stage == kStageVertex ? EShLangVertex : EShLangFragment;
    const char* src = glsl.c_str();
    glslang::TShader shader(lang);
    // "#version 310 es" in the source sets the profile; env targets SPIR-V 1.0.
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setStrings(&src, 1);
    if (!shader.parse(GetDefaultResources(), 100, false, EShMsgDefault)) {
        err = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        err = program.getInfoLog();
        return false;
    }
    spirv.clear();
    glslang::SpvOptions options;
    options.generateDebugInfo = false;
    options.validate = false;   // spirv-val is a separate tool; glslang emits valid SPIR-V
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &options);
    if (!logger.getAllMessages().empty()) {
        err = logger.getAllMessages();
        return false;
    }
    return !spirv.empty();
}

} // namespace

bool compile_glsl_to_spirv(const std::string& glsl, u32 stage,
                           std::vector<u32>& spirv, std::string& err) {
    static const bool init = glslang::InitializeProcess();
    if (!init) {
        err = "glslang init failed";
        return false;
    }
    return compile_impl(glsl, stage, spirv, err);
}

#else   // !PAPAYA_HAS_GLSLANG

bool compile_glsl_to_spirv(const std::string&, u32, std::vector<u32>&, std::string& err) {
    err = "glslang not linked (build with -DPAPAYA_GLSLANG_ROOT=...)";
    return false;
}

#endif

bool dxbc_to_spirv(std::span<const u8> dxbc, u32 stage,
                   std::vector<u32>& spirv, std::string& err) {
    std::string glsl;
    if (!dxbc_to_glsl(dxbc, glsl)) {
        err = "dxbc_to_glsl failed (unsupported instruction set?)";
        spirv.clear();
        return false;
    }
    return compile_glsl_to_spirv(glsl, stage, spirv, err);
}

} // namespace papaya::gpu