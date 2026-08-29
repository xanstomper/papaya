add_library(papaya-gpu STATIC
    src/gpu_core.cpp
    src/gcn_pm4.cpp
    src/vulkan_backend.cpp
)

target_include_directories(papaya-gpu PUBLIC
    include
)

target_link_libraries(papaya-gpu PUBLIC
    papaya-common
    papaya-hv
)

if (Vulkan_FOUND)
    target_link_libraries(papaya-gpu PUBLIC Vulkan::Vulkan)
    target_compile_definitions(papaya-gpu PUBLIC PAPAYA_HAS_VULKAN=1)
endif()
