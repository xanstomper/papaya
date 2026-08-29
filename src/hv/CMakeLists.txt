add_library(papaya-hv STATIC
    src/hypervisor.cpp
    src/memory_map.cpp
    src/kvm/kvm_hypervisor.cpp
    src/kvm/kvm_vcpu.cpp
    src/kvm/kvm_long_mode.cpp
)

target_include_directories(papaya-hv PUBLIC
    include
)

target_link_libraries(papaya-hv PUBLIC
    papaya-common
)
