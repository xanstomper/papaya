#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/storage/vfs.hpp"
#include "papaya/storage/pe_loader.hpp"
#include "papaya/hle/syscalls.hpp"
#include "papaya/hle/hle_thunk.hpp"
#include <memory>
#include <vector>

namespace papaya::hle {

class Kernel {
public:
    Kernel(std::shared_ptr<hv::IHypervisor> hypervisor, std::shared_ptr<storage::VirtualFileSystem> vfs);
    ~Kernel();

    Result<> initialize();
    Result<storage::LoadedPeImage> load_title_executable(std::string_view exe_path, void* host_ram_base, u64 ram_size);

    SyscallDispatcher& get_syscall_dispatcher() { return dispatcher_; }
    HleThunkManager& get_thunk_manager() { return thunk_mgr_; }
    std::shared_ptr<hv::IHypervisor> get_hypervisor() const { return hypervisor_; }
    std::shared_ptr<storage::VirtualFileSystem> get_vfs() const { return vfs_; }

private:
    void register_standard_syscalls();
    void register_standard_hle_apis();

    std::shared_ptr<hv::IHypervisor> hypervisor_;
    std::shared_ptr<storage::VirtualFileSystem> vfs_;
    SyscallDispatcher dispatcher_;
    HleThunkManager thunk_mgr_;
    storage::PeLoader pe_loader_;
    std::vector<std::shared_ptr<hv::IVcpu>> vcpus_;
};

} // namespace papaya::hle
