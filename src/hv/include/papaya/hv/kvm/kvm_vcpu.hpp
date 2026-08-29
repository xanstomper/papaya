#pragma once

#include "papaya/hv/vcpu.hpp"
#include <linux/kvm.h>

namespace papaya::hv::kvm {

class KvmHypervisor;

class KvmVcpu : public IVcpu {
public:
    KvmVcpu(KvmHypervisor& hv, u32 vcpu_id, int vcpu_fd, struct kvm_run* run_struct);
    ~KvmVcpu() override;

    u32 get_id() const override { return vcpu_id_; }
    Result<> set_registers(const CpuRegisters& regs) override;
    Result<CpuRegisters> get_registers() const override;
    Result<> setup_initial_state(GuestPhysAddr entry_point, GuestPhysAddr stack_top) override;
    Result<VcpuExitInfo> run_once() override;
    void request_interrupt(u8 vector) override;

private:
    Result<> setup_long_mode_segments();

    KvmHypervisor& hv_;
    u32 vcpu_id_{0};
    int vcpu_fd_{-1};
    struct kvm_run* run_struct_{nullptr};
};

} // namespace papaya::hv::kvm
