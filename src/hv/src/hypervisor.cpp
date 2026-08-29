#include "papaya/hv/hypervisor.hpp"
#include "papaya/hv/kvm/kvm_hypervisor.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::hv {

std::unique_ptr<IHypervisor> create_hypervisor(PlatformBackend backend) {
    switch (backend) {
        case PlatformBackend::Kvm:
            log::info("HV", "Instantiating Linux KVM backend");
            return std::make_unique<kvm::KvmHypervisor>();
        case PlatformBackend::Whvp:
            log::warn("HV", "WHVP backend is not supported on Linux host");
            return nullptr;
        case PlatformBackend::Arm64Jit:
            log::warn("HV", "ARM64 JIT backend selected (target: Android/ARM64)");
            return nullptr;
    }
    return nullptr;
}

} // namespace papaya::hv
