add_library(papaya-common STATIC
    src/logger.cpp
)

target_include_directories(papaya-common PUBLIC
    include
)

target_link_libraries(papaya-common PUBLIC
    Threads::Threads
)
