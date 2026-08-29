add_library(papaya-input STATIC
    src/input_manager.cpp
)

target_include_directories(papaya-input PUBLIC
    include
)

target_link_libraries(papaya-input PUBLIC
    papaya-common
)
