add_executable(test_kvm_boot
    test_kvm_boot.cpp
)

target_link_libraries(test_kvm_boot PRIVATE
    papaya-common
    papaya-hv
)

add_executable(test_gpu_pm4
    test_gpu_pm4.cpp
)

target_link_libraries(test_gpu_pm4 PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_pe_loader
    test_pe_loader.cpp
)

target_link_libraries(test_pe_loader PRIVATE
    papaya-common
    papaya-storage
)

add_executable(test_hle_thunk
    test_hle_thunk.cpp
)

target_link_libraries(test_hle_thunk PRIVATE
    papaya-common
    papaya-storage
    papaya-hle
    papaya-hv
)

add_executable(test_long_mode_pe_boot
    test_long_mode_pe_boot.cpp
)

target_link_libraries(test_long_mode_pe_boot PRIVATE
    papaya-common
    papaya-storage
    papaya-hle
    papaya-hv
)

add_executable(test_gcn_state
    test_gcn_state.cpp
)

target_link_libraries(test_gcn_state PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_shader_translator
    test_shader_translator.cpp
)

target_link_libraries(test_shader_translator PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_threading_sync
    test_threading_sync.cpp
)

target_link_libraries(test_threading_sync PRIVATE
    papaya-common
    papaya-storage
    papaya-hle
    papaya-hv
)

add_test(NAME KvmBootTest COMMAND test_kvm_boot)
add_test(NAME GpuPm4Test COMMAND test_gpu_pm4)
add_test(NAME PeLoaderTest COMMAND test_pe_loader)
add_test(NAME HleThunkTest COMMAND test_hle_thunk)
add_test(NAME LongModePeBootTest COMMAND test_long_mode_pe_boot)
add_test(NAME GcnStateTest COMMAND test_gcn_state)
add_test(NAME ShaderTranslatorTest COMMAND test_shader_translator)
add_test(NAME ThreadingSyncTest COMMAND test_threading_sync)
