#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/vcpu.hpp"
#include "papaya/hv/memory_map.hpp"
#include <memory>
#include <vector>

namespace papaya::hv {

class IHypervisor {
public:
    virtual ~IHypervisor() = default;

    virtual PlatformBackend get_backend_type() const = 0;
    virtual Result<> initialize() = 0;
    virtual bool is_initialized() const = 0;
    virtual Result<> configure_memory(const MemoryMap& mem_map) = 0;
    virtual Result<std::shared_ptr<IVcpu>> create_vcpu(u32 vcpu_id) = 0;
    virtual u32 get_max_vcpus() const = 0;
};

std::unique_ptr<IHypervisor> create_hypervisor(PlatformBackend backend = PlatformBackend::Kvm);

} // namespace papaya::hv
