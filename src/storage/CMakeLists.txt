add_library(papaya-storage STATIC
    src/vfs.cpp
    src/xvd.cpp
    src/xvd_crypto.cpp
    src/pe_loader.cpp
)

target_include_directories(papaya-storage PUBLIC
    include
)

target_link_libraries(papaya-storage PUBLIC
    papaya-common
)
