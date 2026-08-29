add_library(papaya-storage STATIC
    src/vfs.cpp
    src/elf_loader.cpp
    src/param_parser.cpp
    src/kraken.cpp
    src/xvd.cpp
    src/xvd_crypto.cpp
)

target_include_directories(papaya-storage PUBLIC
    include
)

target_link_libraries(papaya-storage PUBLIC
    papaya-common
)
