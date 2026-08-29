#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/storage/vfs.hpp"
#include "papaya/storage/pe_loader.hpp"
#include "papaya/hle/hle_thunk.hpp"
#include "papaya/hle/sync_primitives.hpp"
#include "papaya/hle/thread_manager.hpp"
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
        audio::AudioEngine* audio = nullptr
    );
    ~Kernel();

    Result<> initialize();

    Result<storage::LoadedPeImage> load_title_executable(
        std::string_view exe_path,
        void* guest_memory_host_base,
        u64 guest_memory_size
    );

    HleThunkManager& get_thunk_manager() { return thunk_manager_; }
    HandleTable& get_handle_table() { return handle_table_; }
    ThreadManager& get_thread_manager() { return thread_manager_; }

    void set_input_manager(input::InputManager* input) { input_ = input; }
    void set_audio_engine(audio::AudioEngine* audio) { audio_ = audio; }

private:
    void register_kernel32_exports();
    void register_ntdll_exports();
    void register_xg_exports();
    void register_synchronization_exports();
    void register_xinput_exports();
    void register_xaudio_exports();

    std::shared_ptr<hv::IHypervisor> hv_;
    std::shared_ptr<storage::VirtualFileSystem> vfs_;
    input::InputManager* input_{nullptr};
    audio::AudioEngine* audio_{nullptr};

    storage::PeLoader pe_loader_;
    HleThunkManager thunk_manager_;
    HandleTable handle_table_;
    ThreadManager thread_manager_;
};

} // namespace papaya::hle
