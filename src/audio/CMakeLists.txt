add_library(papaya-audio STATIC
    src/audio_bridge.cpp
)

target_include_directories(papaya-audio PUBLIC
    include
)

target_link_libraries(papaya-audio PUBLIC
    papaya-common
)

target_compile_features(papaya-audio PUBLIC cxx_std_23)
