add_library(papaya-kernel STATIC
    src/ntsync.cpp
    src/io_uring_streamer.cpp
    src/wine_prefix.cpp
    src/dotnet_bridge.cpp
)

target_include_directories(papaya-kernel PUBLIC
    include
)

target_link_libraries(papaya-kernel PUBLIC
    papaya-common
    dl
)

target_compile_features(papaya-kernel PUBLIC cxx_std_23)
