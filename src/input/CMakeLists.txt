add_library(papaya-input STATIC
    src/virtual_xinput.cpp
)

target_include_directories(papaya-input PUBLIC
    include
)

target_link_libraries(papaya-input PUBLIC
    papaya-common
)

target_compile_features(papaya-input PUBLIC cxx_std_23)
