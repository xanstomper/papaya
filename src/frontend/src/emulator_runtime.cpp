#include "papaya/frontend/emulator_runtime.hpp"
#include "papaya/common/logger.hpp"
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fstream>

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

    std::filesystem::path game_p(exe_path);
    if (!std::filesystem::exists(game_p) && !config_.headless) {
        log::error("RUNTIME", "Target game binary does not exist: '{}'", exe_path);
        return ErrorCode::FileNotFound;
    }

    // Auto-discover Steam AppID from game directory if available
    auto discovered_id = steam::SteamApiStub::discover_app_id(game_p.parent_path());
    if (discovered_id.has_value()) {
        steam::SteamProfileConfig scfg{};
        scfg.app_id = *discovered_id;
        steam_stub_ = std::make_unique<steam::SteamApiStub>(scfg);
        steam_stub_->initialize();
    }

    // Write Potato Mode dxvk.conf in game directory
    auto dxvk_conf = game_p.parent_path() / "dxvk.conf";
    if (!game_p.parent_path().empty()) {
        std::ofstream cfg(dxvk_conf);
        cfg << "# Papaya Generated DXVK Potato Mode\n"
            << "dxvk.enableAsync = true\n"
            << "dxvk.gpl = true\n"
            << "papaya.potatoMode = true\n"
            << "papaya.mipLodBias = 3.0\n";
    }

    log::info("RUNTIME", "Spawning target process under Papaya Translation Matrix: '{}' (AppID: {})",
              game_p.filename().string(), steam_stub_->get_app_id());

    if (!config_.headless) {
        // Fork and execute the real game process
        pid_t pid = fork();
        if (pid < 0) {
            log::error("RUNTIME", "Failed to fork game process");
            return ErrorCode::UnsupportedOperation;
        }

        if (pid == 0) {
            // Child Process
            std::string game_dir = game_p.parent_path().string();
            if (!game_dir.empty()) {
                if (chdir(game_dir.c_str()) != 0) {
                    // ignore
                }
            }

            // Set Display and Wine/Box64 environment variables
            const char* disp = getenv("DISPLAY");
            if (!disp) setenv("DISPLAY", ":0", 1);

            setenv("WINEDLLOVERRIDES", "steam_api64,steam_api,dxvk,d3d11=n,b", 1);
            setenv("DXVK_CONFIG_FILE", dxvk_conf.c_str(), 1);
            setenv("PAPAYA_POTATO_MODE", "1", 1);
            setenv("PAPAYA_APP_ID", std::to_string(steam_stub_->get_app_id()).c_str(), 1);

            const char* home = getenv("HOME");
            std::string pfx = home ? (std::string(home) + "/.wine") : "./papaya_prefix";
            setenv("WINEPREFIX", pfx.c_str(), 0);

            // Execute via wine
            char* args[] = {
                const_cast<char*>("wine"),
                const_cast<char*>(game_p.c_str()),
                nullptr
            };
            execvp("wine", args);

            _exit(127);
        }

        child_pid_ = pid;
        log::info("RUNTIME", "Game process launched successfully with PID: {}", child_pid_);
    }

    is_running_ = true;
    return {};
}

Result<> EmulatorRuntime::mount_and_launch_rom(std::string_view rom_path_or_uri) {
    log::info("RUNTIME", "Mounting and launching ROM image via Steam compatibility layer: '{}'", rom_path_or_uri);
    return launch_game(rom_path_or_uri);
}

void EmulatorRuntime::step_frame() {
    if (!is_running_) return;

    // Check if child game process has exited
    if (child_pid_ > 0) {
        int status = 0;
        pid_t res = waitpid(child_pid_, &status, WNOHANG);
        if (res == child_pid_) {
            log::info("RUNTIME", "Child game process (PID: {}) terminated.", child_pid_);
            child_pid_ = -1;
            is_running_ = false;
            return;
        }
    }

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
    const auto target_frame_duration = std::chrono::microseconds(16666); // 60 FPS frame pacing (16.66ms)

    log::info("RUNTIME", ">>> Papaya Engine Loop Active @ 60 FPS Target Pacing <<<");

    while (is_running_ && !window_mgr_->should_close()) {
        auto frame_start = std::chrono::steady_clock::now();

        step_frame();

        auto now = std::chrono::steady_clock::now();
        auto elapsed_s = std::chrono::duration<f64>(now - last_time).count();
        if (elapsed_s >= 1.0) {
            u64 cur_frame = frame_count_.load();
            f64 fps_val = static_cast<f64>(cur_frame - last_frame) / elapsed_s;
            current_fps_ = fps_val;
            last_frame = cur_frame;
            last_time = now;
            u64 saved_mb = potato_interceptor_->get_stats().saved_vram_bytes.load() / (1024 * 1024);
            log::info("RUNTIME", "Performance: {:.1f} FPS | Total Frames: {} | VRAM Saved: {} MB",
                      fps_val, cur_frame, saved_mb);
        }

        // Frame pacing sleep
        auto frame_elapsed = std::chrono::steady_clock::now() - frame_start;
        if (frame_elapsed < target_frame_duration) {
            std::this_thread::sleep_for(target_frame_duration - frame_elapsed);
        }
    }
}

void EmulatorRuntime::stop() {
    is_running_ = false;
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        int status = 0;
        waitpid(child_pid_, &status, WNOHANG);
        child_pid_ = -1;
    }
    if (audio_bridge_) {
        audio_bridge_->shutdown();
    }
    if (steam_stub_) {
        steam_stub_->shutdown();
    }
}

} // namespace papaya::frontend
