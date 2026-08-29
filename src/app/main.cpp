#include "papaya/common/logger.hpp"
#include "papaya/common/types.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include "kvm_test.hpp"
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
              << " Project Papaya - Next-Gen Xbox Emulator\n"
              << " Version 0.1.0-dev (Linux x86-64 / KVM)  \n"
              << "\033[0m\n";
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --test-kvm            Run KVM hardware virtualization diagnostic\n"
              << "  --target <target>     Target console: xboxone (default), seriess, seriesx\n"
              << "  --headless            Run in headless CLI mode\n"
              << "  --boot <exe>          Load and boot Title executable\n"
              << "  --log-level <level>   trace, debug, info (default), warn, error\n"
              << "  --help                Show this help message\n";
}

int main(int argc, char* argv[]) {
    print_banner();

    bool run_test = false;
    papaya::frontend::EmulatorConfig config{};
    std::string boot_path;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--test-kvm" || arg == "--diag") {
            run_test = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--headless") {
            config.headless = true;
        } else if (arg == "--log-level" && i + 1 < argc) {
            std::string_view lvl = argv[++i];
            if (lvl == "trace") papaya::log::Logger::instance().set_level(papaya::log::Level::Trace);
            else if (lvl == "debug") papaya::log::Logger::instance().set_level(papaya::log::Level::Debug);
            else if (lvl == "warn") papaya::log::Logger::instance().set_level(papaya::log::Level::Warn);
            else if (lvl == "error") papaya::log::Logger::instance().set_level(papaya::log::Level::Error);
        } else if (arg == "--boot" && i + 1 < argc) {
            boot_path = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            std::string_view tgt = argv[++i];
            if (tgt == "seriess") config.target = papaya::ConsoleTarget::XboxSeriesS;
            else if (tgt == "seriesx") config.target = papaya::ConsoleTarget::XboxSeriesX;
        }
    }

    if (run_test || argc == 1) {
        auto test_res = papaya::app::run_kvm_diagnostics();
        if (!test_res) {
            papaya::log::error("MAIN", "KVM diagnostics failed");
            return 1;
        }
        if (argc == 1) {
            papaya::log::info("MAIN", "Default diagnostics completed. Run with --help for options.");
            return 0;
        }
    }

    // Initialize and run Emulator Runtime
    papaya::frontend::EmulatorRuntime runtime(config);
    if (!runtime.initialize()) {
        papaya::log::error("MAIN", "Failed to initialize Papaya Emulator Runtime");
        return 1;
    }

    if (!boot_path.empty()) {
        papaya::log::info("MAIN", "Booting title executable: {}", boot_path);
        if (!runtime.boot_title(boot_path)) {
            papaya::log::error("MAIN", "Failed to boot Title");
            return 1;
        }
        runtime.run();
    }

    return 0;
}
