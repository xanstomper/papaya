#include "papaya/common/logger.hpp"
#include "papaya/common/types.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include <iostream>
#include <string_view>

void print_banner() {
    std::cout << "\033[38;5;208;1m"
              << "  ____                                  \n"
              << " |  _ \\ __ _ _ __   __ _ _   _  __ _    \n"
              << " | |_) / _` | '_ \\ / _` | | | |/ _` |   \n"
              << " |  __/ (_| | |_) | (_| | |_| | (_| |   \n"
              << " |_|   \\__,_| .__/ \\__,_|\\__, |\\__,_|   \n"
              << "            |_|          |___/          \n"
              << " Papaya - ARM Steam Compatibility Layer \n"
              << " High-Performance Handheld & SBC Runtime\n"
              << "\033[0m\n";
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --game <path.exe>     Path to Windows game executable\n"
              << "  --appid <id>          Steam AppID (overrides steam_appid.txt)\n"
              << "  --potato              Force aggressive Potato Mode (1x1 textures, 540p upscaled)\n"
              << "  --tier <tier>         Hardware tier: low (RP5), mid, high (Odin2), deck, desktop\n"
              << "  --lod-bias <bias>     Mipmap LOD bias override (e.g. +3.0)\n"
              << "  --headless            Run in headless benchmark/CLI mode\n"
              << "  --diag                Run hardware detection & auto-configuration diagnostic\n"
              << "  --log-level <level>   trace, debug, info (default), warn, error\n"
              << "  --help                Show this help message\n";
}

int main(int argc, char* argv[]) {
    print_banner();

    bool run_diag = false;
    papaya::frontend::RuntimeConfig config{};
    std::string game_path;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--diag" || arg == "--diagnostics") {
            run_diag = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--potato" || arg == "--potato-mode") {
            config.force_potato_mode = true;
        } else if (arg == "--headless") {
            config.headless = true;
        } else if (arg == "--game" && i + 1 < argc) {
            game_path = argv[++i];
        } else if (arg == "--appid" && i + 1 < argc) {
            config.steam_app_id = static_cast<papaya::u32>(std::stoul(argv[++i]));
        } else if (arg == "--tier" && i + 1 < argc) {
            std::string_view t = argv[++i];
            if (t == "low" || t == "rp5") config.device_tier = papaya::DeviceTier::UltraLowEnd;
            else if (t == "mid") config.device_tier = papaya::DeviceTier::MobileMidTier;
            else if (t == "high" || t == "odin2") config.device_tier = papaya::DeviceTier::MobileHighTier;
            else if (t == "deck") config.device_tier = papaya::DeviceTier::HandheldPC;
            else if (t == "desktop") config.device_tier = papaya::DeviceTier::DesktopLinux;
        } else if (arg == "--log-level" && i + 1 < argc) {
            std::string_view lvl = argv[++i];
            if (lvl == "trace") papaya::log::Logger::instance().set_level(papaya::log::Level::Trace);
            else if (lvl == "debug") papaya::log::Logger::instance().set_level(papaya::log::Level::Debug);
            else if (lvl == "warn") papaya::log::Logger::instance().set_level(papaya::log::Level::Warn);
            else if (lvl == "error") papaya::log::Logger::instance().set_level(papaya::log::Level::Error);
        }
    }

    if (run_diag || argc == 1) {
        auto auto_cfg = papaya::profile::AutoConfigurator::detect_and_configure();
        auto host_cpu = papaya::cpu::CpuTranslator::detect_host_cpu();
        papaya::log::info("DIAG", "Host Architecture: {} (Cores: {}, PageSize: {} KB)",
                          host_cpu.architecture, host_cpu.cpu_core_count, host_cpu.host_page_size / papaya::KiB);
        papaya::log::info("DIAG", "Detected Tier: {}, Default Mode: {}",
                          static_cast<int>(auto_cfg.tier), static_cast<int>(auto_cfg.perf_mode));
        papaya::log::info("DIAG", "Spoofed GPU: '{}' (VRAM: {} MB)",
                          auto_cfg.spoof_profile.device_name, auto_cfg.spoof_profile.dedicated_vram_bytes / papaya::MiB);
        if (argc == 1) {
            papaya::log::info("DIAG", "Run with --help for game launching options.");
            return 0;
        }
    }

    // Initialize and run Emulator Runtime
    papaya::frontend::EmulatorRuntime runtime(config);
    if (!runtime.initialize()) {
        papaya::log::error("MAIN", "Failed to initialize Papaya Runtime");
        return 1;
    }

    if (!game_path.empty()) {
        if (!runtime.launch_game(game_path)) {
            papaya::log::error("MAIN", "Failed to launch game: {}", game_path);
            return 1;
        }
        runtime.run();
    }

    return 0;
}
