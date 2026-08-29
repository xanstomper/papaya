add_library(papaya-hle STATIC
    src/kernel.cpp
    src/hle_thunk.cpp
    src/sync_primitives.cpp
    src/thread_manager.cpp
    src/syscalls.cpp
)

target_include_directories(papaya-hle PUBLIC
    include
)

target_link_libraries(papaya-hle PUBLIC
    papaya-common
    papaya-hv
    papaya-storage
)
