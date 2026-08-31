add_library(papaya-gpu STATIC
    src/potato_interceptor.cpp
    src/shader_stripper.cpp
    src/swapchain_upscaler.cpp
    src/vulkan_layer.cpp
    src/vulkan_swapchain.cpp
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
