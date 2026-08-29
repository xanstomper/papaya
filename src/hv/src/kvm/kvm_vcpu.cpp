#include "papaya/hv/kvm/kvm_vcpu.hpp"
#include "papaya/hv/kvm/kvm_hypervisor.hpp"
#include "papaya/common/logger.hpp"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

namespace papaya::hv::kvm {

KvmVcpu::KvmVcpu(KvmHypervisor& hv, u32 vcpu_id, int vcpu_fd, struct kvm_run* run_struct)
    : hv_(hv), vcpu_id_(vcpu_id), vcpu_fd_(vcpu_fd), run_struct_(run_struct) {}

KvmVcpu::~KvmVcpu() {
    if (run_struct_ && run_struct_ != MAP_FAILED) {
        munmap(run_struct_, hv_.get_vcpu_mmap_size());
        run_struct_ = nullptr;
    }
    if (vcpu_fd_ >= 0) {
        close(vcpu_fd_);
        vcpu_fd_ = -1;
    }
}

Result<> KvmVcpu::set_registers(const CpuRegisters& regs) {
    struct kvm_regs k_regs{};
    k_regs.rax = regs.rax;
    k_regs.rbx = regs.rbx;
    k_regs.rcx = regs.rcx;
    k_regs.rdx = regs.rdx;
    k_regs.rsi = regs.rsi;
    k_regs.rdi = regs.rdi;
    k_regs.rsp = regs.rsp;
    k_regs.rbp = regs.rbp;
    k_regs.r8  = regs.r8;
    k_regs.r9  = regs.r9;
    k_regs.r10 = regs.r10;
    k_regs.r11 = regs.r11;
    k_regs.r12 = regs.r12;
    k_regs.r13 = regs.r13;
    k_regs.r14 = regs.r14;
    k_regs.r15 = regs.r15;
    k_regs.rip = regs.rip;
    k_regs.rflags = regs.rflags;

    if (ioctl(vcpu_fd_, KVM_SET_REGS, &k_regs) < 0) {
        log::error("KVM", "KVM_SET_REGS failed on vCPU #{}: {}", vcpu_id_, strerror(errno));
        return ErrorCode::VcpuRunFailed;
    }
    return {};
}

Result<CpuRegisters> KvmVcpu::get_registers() const {
    struct kvm_regs k_regs{};
    if (ioctl(vcpu_fd_, KVM_GET_REGS, &k_regs) < 0) {
        log::error("KVM", "KVM_GET_REGS failed on vCPU #{}: {}", vcpu_id_, strerror(errno));
        return ErrorCode::VcpuRunFailed;
    }

    CpuRegisters regs{};
    regs.rax = k_regs.rax;
    regs.rbx = k_regs.rbx;
    regs.rcx = k_regs.rcx;
    regs.rdx = k_regs.rdx;
    regs.rsi = k_regs.rsi;
    regs.rdi = k_regs.rdi;
    regs.rsp = k_regs.rsp;
    regs.rbp = k_regs.rbp;
    regs.r8  = k_regs.r8;
    regs.r9  = k_regs.r9;
    regs.r10 = k_regs.r10;
    regs.r11 = k_regs.r11;
    regs.r12 = k_regs.r12;
    regs.r13 = k_regs.r13;
    regs.r14 = k_regs.r14;
    regs.r15 = k_regs.r15;
    regs.rip = k_regs.rip;
    regs.rflags = k_regs.rflags;

    return regs;
}

Result<> KvmVcpu::setup_initial_state(GuestPhysAddr entry_point, GuestPhysAddr stack_top) {
    struct kvm_sregs sregs{};
    if (ioctl(vcpu_fd_, KVM_GET_SREGS, &sregs) < 0) {
        log::error("KVM", "KVM_GET_SREGS failed on vCPU #{}", vcpu_id_);
        return ErrorCode::VcpuRunFailed;
    }

    // Configure CS base to 0 for flat real-mode entry
    sregs.cs.base = 0;
    sregs.cs.selector = 0;

    if (ioctl(vcpu_fd_, KVM_SET_SREGS, &sregs) < 0) {
        log::error("KVM", "KVM_SET_SREGS failed on vCPU #{}", vcpu_id_);
        return ErrorCode::VcpuRunFailed;
    }

    CpuRegisters regs{};
    regs.rip = entry_point;
    regs.rsp = stack_top;
    regs.rflags = 0x2; // Standard reserved bit

    return set_registers(regs);
}

Result<VcpuExitInfo> KvmVcpu::run_once() {
    if (ioctl(vcpu_fd_, KVM_RUN, 0) < 0) {
        if (errno == EINTR) {
            return VcpuExitInfo{.reason = ExitReason::InternalError};
        }
        log::error("KVM", "KVM_RUN ioctl failed on vCPU #{}: {}", vcpu_id_, strerror(errno));
        return ErrorCode::VcpuRunFailed;
    }

    VcpuExitInfo exit_info{};

    switch (run_struct_->exit_reason) {
        case KVM_EXIT_IO:
            exit_info.reason = (run_struct_->io.direction == KVM_EXIT_IO_OUT) ? ExitReason::IoOut : ExitReason::IoIn;
            exit_info.address = run_struct_->io.port;
            exit_info.size = run_struct_->io.size;
            exit_info.is_write = (run_struct_->io.direction == KVM_EXIT_IO_OUT);
            if (exit_info.is_write) {
                u8* p = reinterpret_cast<u8*>(run_struct_) + run_struct_->io.data_offset;
                if (exit_info.size == 1) exit_info.data = *p;
                else if (exit_info.size == 2) exit_info.data = *reinterpret_cast<u16*>(p);
                else if (exit_info.size == 4) exit_info.data = *reinterpret_cast<u32*>(p);
            }
            break;

        case KVM_EXIT_MMIO:
            exit_info.reason = run_struct_->mmio.is_write ? ExitReason::MmioWrite : ExitReason::MmioRead;
            exit_info.address = run_struct_->mmio.phys_addr;
            exit_info.size = run_struct_->mmio.len;
            exit_info.is_write = (run_struct_->mmio.is_write != 0);
            if (exit_info.is_write) {
                std::memcpy(&exit_info.data, run_struct_->mmio.data, run_struct_->mmio.len);
            }
            break;

        case KVM_EXIT_HLT:
            exit_info.reason = ExitReason::Halt;
            break;

        case KVM_EXIT_SHUTDOWN:
            exit_info.reason = ExitReason::Shutdown;
            break;

        case KVM_EXIT_HYPERCALL:
            exit_info.reason = ExitReason::Hypercall;
            exit_info.data = run_struct_->hypercall.nr;
            break;

        default:
            exit_info.reason = ExitReason::Unknown;
            log::warn("KVM", "Unhandled KVM Exit Reason: {}", run_struct_->exit_reason);
            break;
    }

    return exit_info;
}

void KvmVcpu::request_interrupt(u8 vector) {
    struct kvm_interrupt irq{};
    irq.irq = vector;
    ioctl(vcpu_fd_, KVM_INTERRUPT, &irq);
}

} // namespace papaya::hv::kvm
