add_library(papaya-cpu STATIC
    src/cpu_translator.cpp
)

target_include_directories(papaya-cpu PUBLIC
    include
)

target_link_libraries(papaya-cpu PUBLIC
    papaya-common
)

target_compile_features(papaya-cpu PUBLIC cxx_std_23)
