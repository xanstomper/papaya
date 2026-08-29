#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/hv/memory_map.hpp"
#include "papaya/storage/vfs.hpp"
#include "papaya/hle/kernel.hpp"
#include "papaya/gpu/gpu_core.hpp"
#include "papaya/audio/audio_engine.hpp"
#include "papaya/input/input_manager.hpp"
#include "papaya/frontend/window_manager.hpp"
#include <memory>
#include <string_view>
#include <atomic>

namespace papaya::frontend {

struct EmulatorConfig {
    ConsoleTarget target{ConsoleTarget::XboxOne};
    PlatformBackend backend{PlatformBackend::Kvm};
    bool headless{false};
    u32 target_fps{60};
    std::string boot_title_path;
};

class EmulatorRuntime {
public:
    explicit EmulatorRuntime(const EmulatorConfig& config = {});
    ~EmulatorRuntime();

    Result<> initialize();
    Result<> boot_title(std::string_view exe_path);

    void step_frame();
    void run();
    void stop();

    bool is_running() const { return is_running_.load(); }
    u64 get_frame_count() const { return frame_count_.load(); }
    f64 get_current_fps() const { return current_fps_.load(); }

    hv::IHypervisor& get_hypervisor() { return *hv_; }
    hle::Kernel& get_kernel() { return *kernel_; }
    gpu::GpuCore& get_gpu() { return *gpu_; }
    audio::AudioEngine& get_audio() { return *audio_; }
    input::InputManager& get_input() { return *input_; }
    WindowManager& get_window() { return *window_; }

private:
    EmulatorConfig config_;
    std::atomic<bool> is_running_{false};
    std::atomic<u64> frame_count_{0};
    std::atomic<f64> current_fps_{0.0};

    std::shared_ptr<storage::VirtualFileSystem> vfs_;
    std::unique_ptr<hv::MemoryMap> memory_map_;
    std::shared_ptr<hv::IHypervisor> hv_;
    std::unique_ptr<hle::Kernel> kernel_;
    std::unique_ptr<gpu::GpuCore> gpu_;
    std::unique_ptr<audio::AudioEngine> audio_;
    std::unique_ptr<input::InputManager> input_;
    std::unique_ptr<WindowManager> window_;

    std::shared_ptr<hv::IVcpu> primary_vcpu_;
};

} // namespace papaya::frontend
