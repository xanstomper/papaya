#pragma once

#include "papaya/hv/hypervisor.hpp"
#include <linux/kvm.h>

namespace papaya::hv::kvm {

class KvmHypervisor : public IHypervisor {
public:
    KvmHypervisor();
    ~KvmHypervisor() override;

    PlatformBackend get_backend_type() const override { return PlatformBackend::Kvm; }
    Result<> initialize() override;
    bool is_initialized() const override { return vm_fd_ >= 0; }
    Result<> configure_memory(const MemoryMap& mem_map) override;
    Result<std::shared_ptr<IVcpu>> create_vcpu(u32 vcpu_id) override;
    u32 get_max_vcpus() const override;

    int get_vm_fd() const { return vm_fd_; }
    int get_kvm_fd() const { return kvm_fd_; }
    int get_vcpu_mmap_size() const { return vcpu_mmap_size_; }

private:
    int kvm_fd_{-1};
    int vm_fd_{-1};
    int vcpu_mmap_size_{0};
    u32 max_vcpus_{8};
};

} // namespace papaya::hv::kvm
