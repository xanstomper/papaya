add_library(papaya-gpu STATIC
    src/potato_interceptor.cpp
    src/shader_stripper.cpp
    src/swapchain_upscaler.cpp
    src/vulkan_layer.cpp
    src/vulkan_swapchain.cpp
    src/shader_translator.cpp
    src/pipeline_map.cpp
    src/shader_compile.cpp
    src/vulkan_pipeline.cpp
)

target_include_directories(papaya-gpu PUBLIC
    include
)

find_package(Vulkan QUIET)
if (Vulkan_FOUND)
    target_link_libraries(papaya-gpu PUBLIC Vulkan::Vulkan)
    target_compile_definitions(papaya-gpu PUBLIC PAPAYA_HAS_VULKAN=1)
    message(STATUS "Papaya GPU: Vulkan found -> D3D->Vulkan swapchain backend enabled")
else()
    target_compile_definitions(papaya-gpu PUBLIC PAPAYA_HAS_VULKAN=0)
    message(STATUS "Papaya GPU: Vulkan NOT found -> vulkan_swapchain compiled out")
endif()

target_link_libraries(papaya-gpu PUBLIC
    papaya-common
)

target_compile_features(papaya-gpu PUBLIC cxx_std_23)

# Optional in-process GLSL->SPIR-V via glslang (Stage 4c).
# Point PAPAYA_GLSLANG_ROOT at a glslang source checkout whose build/ contains
# the static libs (default: the /tmp checkout used by scripts/verify_shader_glsl.sh).
set(PAPAYA_GLSLANG_ROOT "/tmp/glslang-src" CACHE PATH "glslang source root with build/libglslang.a")
set(PAPAYA_SPIRV_HEADERS_ROOT "/tmp/spirv-headers" CACHE PATH "SPIRV-Headers checkout root")
if(EXISTS "${PAPAYA_GLSLANG_ROOT}/build/glslang/libglslang.a"
   AND EXISTS "${PAPAYA_SPIRV_HEADERS_ROOT}/include/spirv/unified1/spirv.hpp")
    target_include_directories(papaya-gpu PRIVATE
        ${PAPAYA_GLSLANG_ROOT}
        ${PAPAYA_GLSLANG_ROOT}/glslang/Include
        ${PAPAYA_GLSLANG_ROOT}/glslang/Public
        ${PAPAYA_GLSLANG_ROOT}/SPIRV
        ${PAPAYA_SPIRV_HEADERS_ROOT}/include)
    target_link_libraries(papaya-gpu PRIVATE
        ${PAPAYA_GLSLANG_ROOT}/build/glslang/libglslang.a
        ${PAPAYA_GLSLANG_ROOT}/build/glslang/libglslang-default-resource-limits.a)
    target_compile_definitions(papaya-gpu PUBLIC PAPAYA_HAS_GLSLANG=1)
    message(STATUS "Papaya GPU: glslang found -> in-process GLSL->SPIR-V enabled")
else()
    message(STATUS "Papaya GPU: glslang NOT found -> in-process compile unavailable")
endif()
