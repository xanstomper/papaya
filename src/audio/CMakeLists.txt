add_library(papaya-audio STATIC
    src/shape_dsp.cpp
    src/audio_engine.cpp
)

target_include_directories(papaya-audio PUBLIC
    include
)

target_link_libraries(papaya-audio PUBLIC
    papaya-common
)
