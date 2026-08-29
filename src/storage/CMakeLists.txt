add_library(papaya-storage STATIC
    src/xvd.cpp
    src/vfs.cpp
)

target_include_directories(papaya-storage PUBLIC
    include
)

target_link_libraries(papaya-storage PUBLIC
    papaya-common
)

if (ZSTD_FOUND)
    target_link_libraries(papaya-storage PUBLIC ${ZSTD_LIBRARIES})
    target_include_directories(papaya-storage PUBLIC ${ZSTD_INCLUDE_DIRS})
endif()
