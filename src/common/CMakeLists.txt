add_library(papaya-common STATIC
    src/logger.cpp
)

target_include_directories(papaya-common PUBLIC
    include
)

target_compile_features(papaya-common PUBLIC cxx_std_23)
