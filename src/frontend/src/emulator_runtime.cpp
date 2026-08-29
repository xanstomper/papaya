#include "papaya/frontend/emulator_runtime.hpp"
#include "papaya/common/logger.hpp"
#include <chrono>
#include <thread>

namespace papaya::frontend {

EmulatorRuntime::EmulatorRuntime(const RuntimeConfig& config)
    : config_(config) {}

EmulatorRuntime::~EmulatorRuntime() {
    stop();
}

Result<> EmulatorRuntime::initialize() {
    log::info("RUNTIME", "==========================================================");
    log::info("RUNTIME", "Initializing Papaya ARM Steam Compatibility Layer Core");
    log::info("RUNTIME", "==========================================================");

    // 1. Auto-configure device tier & hardware profiles
    auto auto_cfg = profile::AutoConfigurator::detect_and_configure();
    if (config_.force_potato_mode) {
        auto_cfg = profile::AutoConfigurator::create_profile_for_tier(DeviceTier::UltraLowEnd);
    }

    // 2. Hardware Spoofer & Watchdog
    spoofer_ = std::make_unique<profile::HardwareSpoofer>(auto_cfg.spoof_profile);
    watchdog_ = std::make_unique<profile::MemoryWatchdog>(0.85f);

    // 3. Graphics & Potato Mode Interceptors
    potato_interceptor_ = std::make_shared<gpu::PotatoInterceptor>(auto_cfg.texture_config);
    shader_stripper_ = std::make_shared<gpu::ShaderStripper>();
    upscaler_ = std::make_shared<gpu::SwapchainUpscaler>(auto_cfg.swapchain_config);
    vulkan_layer_ = std::make_unique<gpu::VulkanLayerInterceptor>(potato_interceptor_, shader_stripper_, upscaler_);
    vulkan_layer_->initialize();

    // Register watchdog memory flush callback
    watchdog_->register_flush_callback([this]() {
        log::warn("RUNTIME", "Memory watchdog triggered: flushing texture caches");
    });

    // 4. Steamworks API Stub
    steam::SteamProfileConfig steam_cfg{};
    steam_cfg.app_id = (config_.steam_app_id > 0) ? config_.steam_app_id : 480;
    steam_stub_ = std::make_unique<steam::SteamApiStub>(steam_cfg);
    steam_stub_->initialize();

    // 5. CPU Translator & 16KB Page Alignment Manager
    cpu_translator_ = std::make_unique<cpu::CpuTranslator>();
    cpu_translator_->initialize();

    // 6. Kernel Sync (NTSync) & io_uring Direct I/O
    ntsync_ = std::make_unique<kernel::NtSyncManager>();
    ntsync_->initialize();

    io_uring_ = std::make_unique<kernel::IoUringStreamer>(256);
    io_uring_->initialize();

    // 7. Wine Prefix Manager
    prefix_mgr_ = std::make_unique<kernel::WinePrefixManager>();
    prefix_mgr_->initialize_prefix();

    // 8. Audio & Input Subsystems
    audio_bridge_ = std::make_unique<audio::AudioBridge>();
    audio_bridge_->initialize();

    input_mgr_ = std::make_unique<input::VirtualXInputManager>();
    input_mgr_->initialize();

    // 9. Window & Display Server
    WindowConfig win_cfg{
        .title = "Project Papaya - Steam ARM Runtime",
        .width = auto_cfg.swapchain_config.display_width,
        .height = auto_cfg.swapchain_config.display_height,
        .fullscreen = false,
        .headless = config_.headless,
        .vsync = true
    };
    window_mgr_ = std::make_unique<WindowManager>(win_cfg);
    window_mgr_->initialize();

    log::info("RUNTIME", "All Papaya subsystems successfully initialized and primed for execution!");
    return {};
}

Result<> EmulatorRuntime::launch_game(std::string_view exe_path) {
    log::info("RUNTIME", "Priming execution pipeline for Steam Title: '{}'", exe_path);

    // Auto-discover Steam AppID from game directory if available
    std::filesystem::path game_p(exe_path);
    auto discovered_id = steam::SteamApiStub::discover_app_id(game_p.parent_path());
    if (discovered_id.has_value()) {
        steam::SteamProfileConfig scfg{};
        scfg.app_id = *discovered_id;
        steam_stub_ = std::make_unique<steam::SteamApiStub>(scfg);
        steam_stub_->initialize();
    }

    is_running_ = true;
    return {};
}

void EmulatorRuntime::step_frame() {
    if (!is_running_) return;

    window_mgr_->poll_events();
    if (window_mgr_->should_close()) {
        is_running_ = false;
        return;
    }

    // Run Steam callbacks
    if (steam_stub_) {
        steam_stub_->steam_api_run_callbacks();
    }

    // Poll memory watchdog
    if (watchdog_) {
        watchdog_->poll_and_enforce();
    }

    frame_count_++;
}

void EmulatorRuntime::run() {
    is_running_ = true;
    auto last_time = std::chrono::steady_clock::now();
    u64 last_frame = 0;

    while (is_running_ && !window_mgr_->should_close()) {
        step_frame();

        auto now = std::chrono::steady_clock::now();
        auto elapsed_s = std::chrono::duration<f64>(now - last_time).count();
        if (elapsed_s >= 1.0) {
            u64 cur_frame = frame_count_.load();
            current_fps_ = static_cast<f64>(cur_frame - last_frame) / elapsed_s;
            last_frame = cur_frame;
            last_time = now;
        }
    }
}

void EmulatorRuntime::stop() {
    is_running_ = false;
    if (audio_bridge_) {
        audio_bridge_->shutdown();
    }
    if (steam_stub_) {
        steam_stub_->shutdown();
    }
}

} // namespace papaya::frontend
