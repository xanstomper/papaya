add_executable(papaya
    main.cpp
    kvm_test.cpp
)

target_include_directories(papaya PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(papaya PRIVATE
    papaya-common
    papaya-hv
    papaya-storage
    papaya-hle
    papaya-gpu
    papaya-audio
    papaya-input
)
