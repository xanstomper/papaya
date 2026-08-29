#include "papaya/hv/kvm/kvm_hypervisor.hpp"
#include "papaya/hv/kvm/kvm_vcpu.hpp"
#include "papaya/common/logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>

namespace papaya::hv::kvm {

KvmHypervisor::KvmHypervisor() = default;

KvmHypervisor::~KvmHypervisor() {
    if (vm_fd_ >= 0) {
        close(vm_fd_);
        vm_fd_ = -1;
    }
    if (kvm_fd_ >= 0) {
        close(kvm_fd_);
        kvm_fd_ = -1;
    }
}

Result<> KvmHypervisor::initialize() {
    log::info("KVM", "Opening /dev/kvm device node");
    kvm_fd_ = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd_ < 0) {
        log::error("KVM", "Failed to open /dev/kvm: {}", strerror(errno));
        return ErrorCode::KvmUnavailable;
    }

    int api_version = ioctl(kvm_fd_, KVM_GET_API_VERSION, 0);
    if (api_version != KVM_API_VERSION) {
        log::error("KVM", "Unsupported KVM API version: {} (expected {})", api_version, KVM_API_VERSION);
        return ErrorCode::HypervisorInitFailed;
    }

    vcpu_mmap_size_ = ioctl(kvm_fd_, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (vcpu_mmap_size_ <= 0) {
        log::error("KVM", "Failed to query KVM_GET_VCPU_MMAP_SIZE");
        return ErrorCode::HypervisorInitFailed;
    }

    int max_vcpus = ioctl(kvm_fd_, KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS);
    if (max_vcpus > 0) {
        max_vcpus_ = static_cast<u32>(max_vcpus);
    }

    log::info("KVM", "KVM API v{} initialized, vcpu_mmap_size: {} bytes, max_vcpus: {}",
              api_version, vcpu_mmap_size_, max_vcpus_);

    // Create Virtual Machine instance
    vm_fd_ = ioctl(kvm_fd_, KVM_CREATE_VM, 0);
    if (vm_fd_ < 0) {
        log::error("KVM", "KVM_CREATE_VM failed: {}", strerror(errno));
        return ErrorCode::HypervisorInitFailed;
    }

    log::info("KVM", "KVM Virtual Machine instance created successfully (vm_fd: {})", vm_fd_);
    return {};
}

Result<> KvmHypervisor::configure_memory(const MemoryMap& mem_map) {
    log::info("KVM", "Registering guest memory regions with KVM");

    for (const auto& region : mem_map.get_regions()) {
        struct kvm_userspace_memory_region kvm_region{};
        kvm_region.slot = region.slot_id;
        kvm_region.flags = region.is_readonly ? KVM_MEM_READONLY : 0;
        kvm_region.guest_phys_addr = region.guest_base;
        kvm_region.memory_size = region.size;
        kvm_region.userspace_addr = reinterpret_cast<u64>(region.host_ptr);

        if (ioctl(vm_fd_, KVM_SET_USER_MEMORY_REGION, &kvm_region) < 0) {
            log::error("KVM", "Failed to register slot #{} ('{}', size {} MB) with KVM: {}",
                       region.slot_id, region.name, region.size / MiB, strerror(errno));
            return ErrorCode::MemoryMappingFailed;
        }

        log::debug("KVM", "Registered memory slot #{}: GPA [0x{:X} - 0x{:X}] -> HVA 0x{:X}",
                   region.slot_id, region.guest_base, region.guest_base + region.size, kvm_region.userspace_addr);
    }

    return {};
}

Result<std::shared_ptr<IVcpu>> KvmHypervisor::create_vcpu(u32 vcpu_id) {
    if (vm_fd_ < 0) {
        return ErrorCode::HypervisorInitFailed;
    }

    log::info("KVM", "Creating vCPU #{}", vcpu_id);
    int vcpu_fd = ioctl(vm_fd_, KVM_CREATE_VCPU, vcpu_id);
    if (vcpu_fd < 0) {
        log::error("KVM", "KVM_CREATE_VCPU failed for vCPU #{}: {}", vcpu_id, strerror(errno));
        return ErrorCode::VcpuCreationFailed;
    }

    // Map kvm_run shared memory structure
    auto* run_struct = static_cast<struct kvm_run*>(
        mmap(nullptr, vcpu_mmap_size_, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0)
    );

    if (run_struct == MAP_FAILED) {
        log::error("KVM", "Failed to mmap kvm_run for vCPU #{}: {}", vcpu_id, strerror(errno));
        close(vcpu_fd);
        return ErrorCode::VcpuCreationFailed;
    }

    return std::static_pointer_cast<IVcpu>(std::make_shared<KvmVcpu>(*this, vcpu_id, vcpu_fd, run_struct));
}

u32 KvmHypervisor::get_max_vcpus() const {
    return max_vcpus_;
}

} // namespace papaya::hv::kvm
