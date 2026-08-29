add_library(papaya-profile STATIC
    src/hardware_spoofer.cpp
    src/auto_configurator.cpp
    src/memory_watchdog.cpp
)

target_include_directories(papaya-profile PUBLIC
    include
)

target_link_libraries(papaya-profile PUBLIC
    papaya-common
    papaya-gpu
)

target_compile_features(papaya-profile PUBLIC cxx_std_23)
