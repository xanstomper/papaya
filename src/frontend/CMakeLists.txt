add_library(papaya-frontend STATIC
    src/window_manager.cpp
    src/emulator_runtime.cpp
)

target_include_directories(papaya-frontend PUBLIC
    include
)

target_link_libraries(papaya-frontend PUBLIC
    papaya-common
    papaya-steam
    papaya-cpu
    papaya-gpu
    papaya-profile
    papaya-kernel
    papaya-audio
    papaya-input
)

target_compile_features(papaya-frontend PUBLIC cxx_std_23)
