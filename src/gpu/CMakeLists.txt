add_library(papaya-gpu STATIC
    src/potato_interceptor.cpp
    src/shader_stripper.cpp
    src/swapchain_upscaler.cpp
    src/vulkan_layer.cpp
)

target_include_directories(papaya-gpu PUBLIC
    include
)

target_link_libraries(papaya-gpu PUBLIC
    papaya-common
)

target_compile_features(papaya-gpu PUBLIC cxx_std_23)
