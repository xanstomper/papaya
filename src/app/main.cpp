#include "papaya/common/logger.hpp"
#include "papaya/common/types.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/hv/memory_map.hpp"
#include "papaya/storage/vfs.hpp"
#include "papaya/storage/xvd.hpp"
#include "papaya/hle/kernel.hpp"
#include "papaya/gpu/gpu_core.hpp"
#include "papaya/audio/audio_engine.hpp"
#include "papaya/input/input_manager.hpp"
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
              << "  --test-kvm            Run KVM hardware virtualization test\n"
              << "  --target <target>     Target console: xboxone (default), seriess, seriesx\n"
              << "  --mount <vpath> <dir> Mount host directory to VFS\n"
              << "  --mount-xvd <file>    Mount XVD/XVC container image\n"
              << "  --boot <exe>          Load and boot Title executable\n"
              << "  --log-level <level>   trace, debug, info (default), warn, error\n"
              << "  --help                Show this help message\n";
}

int main(int argc, char* argv[]) {
    print_banner();

    bool run_test = false;
    papaya::ConsoleTarget target = papaya::ConsoleTarget::XboxOne;
    std::string xvd_path;
    std::string boot_path;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--test-kvm" || arg == "--diag") {
            run_test = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--log-level" && i + 1 < argc) {
            std::string_view lvl = argv[++i];
            if (lvl == "trace") papaya::log::Logger::instance().set_level(papaya::log::Level::Trace);
            else if (lvl == "debug") papaya::log::Logger::instance().set_level(papaya::log::Level::Debug);
            else if (lvl == "warn") papaya::log::Logger::instance().set_level(papaya::log::Level::Warn);
            else if (lvl == "error") papaya::log::Logger::instance().set_level(papaya::log::Level::Error);
        } else if (arg == "--mount-xvd" && i + 1 < argc) {
            xvd_path = argv[++i];
        } else if (arg == "--boot" && i + 1 < argc) {
            boot_path = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            std::string_view tgt = argv[++i];
            if (tgt == "seriess") target = papaya::ConsoleTarget::XboxSeriesS;
            else if (tgt == "seriesx") target = papaya::ConsoleTarget::XboxSeriesX;
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

    papaya::log::info("MAIN", "Initializing Papaya core subsystems...");

    // 1. Storage & VFS
    auto vfs = std::make_shared<papaya::storage::VirtualFileSystem>();
    if (!xvd_path.empty()) {
        auto xvd = std::make_unique<papaya::storage::XvdContainer>();
        if (xvd->open(xvd_path)) {
            vfs->mount_xvd("/xvd", xvd_path);
        }
    }

    // 2. Physical Memory Layout
    auto mem_map = std::make_unique<papaya::hv::MemoryMap>();
    if (target == papaya::ConsoleTarget::XboxOne) {
        if (!mem_map->initialize_xbox_one_layout()) {
            papaya::log::error("MAIN", "Failed to initialize Xbox One memory layout");
            return 1;
        }
    } else {
        if (!mem_map->initialize_series_layout(target == papaya::ConsoleTarget::XboxSeriesX)) {
            papaya::log::error("MAIN", "Failed to initialize Xbox Series memory layout");
            return 1;
        }
    }

    // 3. Hypervisor
    std::shared_ptr<papaya::hv::IHypervisor> hv = papaya::hv::create_hypervisor(papaya::PlatformBackend::Kvm);
    if (!hv || !hv->initialize()) {
        papaya::log::error("MAIN", "Hypervisor initialization failed");
        return 1;
    }
    if (!hv->configure_memory(*mem_map)) {
        papaya::log::error("MAIN", "Hypervisor memory configuration failed");
        return 1;
    }

    // 4. HLE Kernel & OS Runtime
    auto kernel = std::make_unique<papaya::hle::Kernel>(hv, vfs);
    if (!kernel->initialize()) {
        papaya::log::error("MAIN", "Kernel initialization failed");
        return 1;
    }

    // 5. GPU Subsystem
    auto gpu = std::make_unique<papaya::gpu::GpuCore>();
    gpu->initialize();

    // 6. Audio Engine
    auto audio = std::make_unique<papaya::audio::AudioEngine>();
    audio->initialize();

    // 7. Input Subsystem
    auto input = std::make_unique<papaya::input::InputManager>();
    input->initialize();

    papaya::log::info("MAIN", "All Papaya subsystems successfully initialized and ready.");

    if (!boot_path.empty()) {
        papaya::log::info("MAIN", "Booting target title: {}", boot_path);
        kernel->load_title_executable(boot_path);
    }

    return 0;
}
