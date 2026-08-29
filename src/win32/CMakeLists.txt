add_library(papaya-win32 STATIC
    src/win32_api_hle.cpp
    src/pe_loader.cpp
)

target_include_directories(papaya-win32 PUBLIC
    include
)

target_link_libraries(papaya-win32 PUBLIC
    papaya-common
    papaya-steam
)

target_compile_features(papaya-win32 PUBLIC cxx_std_23)
