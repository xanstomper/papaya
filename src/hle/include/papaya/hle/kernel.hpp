#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/storage/vfs.hpp"
#include "papaya/storage/elf_loader.hpp"
#include "papaya/hle/hle_thunk.hpp"
#include "papaya/hle/sync_primitives.hpp"
#include "papaya/hle/thread_manager.hpp"
#include "papaya/hle/memory_manager.hpp"
#include "papaya/hle/freebsd_syscalls.hpp"
#include "papaya/input/input_manager.hpp"
#include "papaya/audio/audio_engine.hpp"
#include <memory>
#include <string>

namespace papaya::hle {

class Kernel {
public:
    Kernel(
        std::shared_ptr<hv::IHypervisor> hv,
        std::shared_ptr<storage::VirtualFileSystem> vfs,
        input::InputManager* input = nullptr,
        audio::AudioEngine* audio = nullptr,
        ConsoleTarget target = ConsoleTarget::PlayStation4
    );
    ~Kernel();

    Result<> initialize(void* guest_memory_host_base, u64 guest_memory_size);

    Result<storage::LoadedElfImage> load_title_executable(
        std::string_view exe_path,
        void* guest_memory_host_base,
        u64 guest_memory_size
    );

    HleThunkManager& get_thunk_manager() { return thunk_manager_; }
    HandleTable& get_handle_table() { return handle_table_; }
    ThreadManager& get_thread_manager() { return thread_manager_; }
    MemoryManager& get_memory_manager() { return memory_manager_; }
    FreeBsdSyscallDispatcher& get_syscall_dispatcher() { return syscall_dispatcher_; }

    void set_input_manager(input::InputManager* input) { input_ = input; }
    void set_audio_engine(audio::AudioEngine* audio) { audio_ = audio; }

private:
    void register_libkernel_exports();
    void register_libscesysmodule_exports();
    void register_libscesavedata_exports();
    void register_libscepad_exports();
    void register_libsceaudioout_exports();
    void register_libsceagc_gnm_exports();
    void register_libscefios2_exports();

    ConsoleTarget target_{ConsoleTarget::PlayStation4};
    std::shared_ptr<hv::IHypervisor> hv_;
    std::shared_ptr<storage::VirtualFileSystem> vfs_;
    input::InputManager* input_{nullptr};
    audio::AudioEngine* audio_{nullptr};

    storage::ElfLoader elf_loader_;
    HleThunkManager thunk_manager_;
    HandleTable handle_table_;
    ThreadManager thread_manager_;
    MemoryManager memory_manager_;
    FreeBsdSyscallDispatcher syscall_dispatcher_;
};

} // namespace papaya::hle
