#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/steam/steam_api_stub.hpp"
#include "papaya/cpu/cpu_translator.hpp"
#include "papaya/gpu/vulkan_layer.hpp"
#include "papaya/profile/auto_configurator.hpp"
#include "papaya/profile/hardware_spoofer.hpp"
#include "papaya/profile/memory_watchdog.hpp"
#include "papaya/kernel/ntsync.hpp"
#include "papaya/kernel/io_uring_streamer.hpp"
#include "papaya/kernel/wine_prefix.hpp"
#include "papaya/audio/audio_bridge.hpp"
#include "papaya/input/virtual_xinput.hpp"
#include "papaya/win32/win32_api_hle.hpp"
#include "papaya/win32/pe_loader.hpp"
#include "papaya/frontend/window_manager.hpp"
#include <memory>
#include <atomic>
#include <filesystem>
#include <sys/types.h>

namespace papaya::frontend {

struct RuntimeConfig {
    DeviceTier device_tier{DeviceTier::DesktopLinux};
    PerformanceMode performance_mode{PerformanceMode::PotatoMode};
    ExecutionMode execution_mode{ExecutionMode::Auto};
    std::string game_executable_path;
    u32 steam_app_id{0};
    bool headless{false};
    bool force_potato_mode{false};
    f32 lod_bias_override{-1.0f};   // Negative = use auto profile default; >= 0 = explicit override
};

class EmulatorRuntime {
public:
    explicit EmulatorRuntime(const RuntimeConfig& config = {});
    ~EmulatorRuntime();

    Result<> initialize();
    Result<> launch_game(std::string_view exe_path);
    Result<> mount_and_launch_rom(std::string_view rom_path_or_uri);

    void step_frame();
    void run();
    void stop();

    bool is_running() const { return is_running_.load(); }
    u64 get_frame_count() const { return frame_count_.load(); }
    f64 get_current_fps() const { return current_fps_.load(); }
    pid_t get_child_pid() const { return child_pid_; }

    steam::SteamApiStub& get_steam() { return *steam_stub_; }
    cpu::CpuTranslator& get_cpu() { return *cpu_translator_; }
    gpu::VulkanLayerInterceptor& get_vulkan() { return *vulkan_layer_; }
    profile::HardwareSpoofer& get_spoofer() { return *spoofer_; }
    profile::MemoryWatchdog& get_watchdog() { return *watchdog_; }
    kernel::NtSyncManager& get_ntsync() { return *ntsync_; }
    kernel::IoUringStreamer& get_io_uring() { return *io_uring_; }
    kernel::WinePrefixManager& get_prefix() { return *prefix_mgr_; }
    audio::AudioBridge& get_audio() { return *audio_bridge_; }
    input::VirtualXInputManager& get_input() { return *input_mgr_; }
    win32::Win32ApiHle& get_win32_hle() { return *win32_hle_; }
    win32::PeLoader& get_pe_loader() { return *pe_loader_; }
    WindowManager& get_window() { return *window_mgr_; }

private:
    RuntimeConfig config_;
    std::atomic<bool> is_running_{false};
    std::atomic<u64> frame_count_{0};
    std::atomic<f64> current_fps_{0.0};
    pid_t child_pid_{-1};

    std::shared_ptr<steam::SteamApiStub> steam_stub_;
    std::shared_ptr<input::VirtualXInputManager> input_mgr_;
    std::shared_ptr<win32::Win32ApiHle> win32_hle_;
    std::unique_ptr<win32::PeLoader> pe_loader_;

    std::unique_ptr<cpu::CpuTranslator> cpu_translator_;
    std::shared_ptr<gpu::PotatoInterceptor> potato_interceptor_;
    std::shared_ptr<gpu::ShaderStripper> shader_stripper_;
    std::shared_ptr<gpu::SwapchainUpscaler> upscaler_;
    std::unique_ptr<gpu::VulkanLayerInterceptor> vulkan_layer_;

    std::unique_ptr<profile::HardwareSpoofer> spoofer_;
    std::unique_ptr<profile::MemoryWatchdog> watchdog_;
    std::unique_ptr<kernel::NtSyncManager> ntsync_;
    std::unique_ptr<kernel::IoUringStreamer> io_uring_;
    std::unique_ptr<kernel::WinePrefixManager> prefix_mgr_;
    std::unique_ptr<audio::AudioBridge> audio_bridge_;
    std::unique_ptr<WindowManager> window_mgr_;
};

} // namespace papaya::frontend
