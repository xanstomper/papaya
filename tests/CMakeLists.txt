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

add_test(NAME KvmBootTest COMMAND test_kvm_boot)
add_test(NAME GpuPm4Test COMMAND test_gpu_pm4)
