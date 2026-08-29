add_library(papaya-frontend STATIC
    src/window_manager.cpp
    src/emulator_runtime.cpp
)

target_include_directories(papaya-frontend PUBLIC
    include
)

target_link_libraries(papaya-frontend PUBLIC
    papaya-common
    papaya-hv
    papaya-storage
    papaya-hle
    papaya-gpu
    papaya-audio
    papaya-input
)
