#include "papaya/common/logger.hpp"
#include "papaya/orchestrator/orchestrator_service.hpp"
#include <iostream>
#include <fstream>
#include <cstdlib>

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAILED: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

int main() {
    using namespace papaya;
    using namespace papaya::orchestrator;

    log::info("TEST", "Running unit test: test_orchestrator");

    std::filesystem::path test_papaya_root = "./test_papaya_sandbox";
    OrchestratorService service(test_papaya_root);

    // 1. Create a dummy dropped game executable / ISO
    std::filesystem::path mock_game = test_papaya_root / "Games" / "EldenRing_Benchmark.exe";
    {
        std::ofstream out(mock_game);
        out << "MOCK_ELDEN_RING_BINARY_DATA";
    }

    // 2. Deploy game through full 7-phase pipeline
    auto deploy_res = service.deploy_game(mock_game);
    TEST_CHECK(deploy_res.has_value());

    const auto& info = *deploy_res;
    TEST_CHECK(info.title_name == "EldenRing_Benchmark");
    TEST_CHECK(info.game_id == "1245620"); // Elden Ring AppID
    TEST_CHECK(info.fps_limit == 30); // 30 FPS handheld profile
    TEST_CHECK(info.potato_mode_enabled == true);
    TEST_CHECK(std::filesystem::exists(info.prefix_dir));
    TEST_CHECK(std::filesystem::exists(info.staging_dir / "dxvk.conf"));
    TEST_CHECK(std::filesystem::exists(info.staging_dir / "steam_appid.txt"));
    TEST_CHECK(std::filesystem::exists(info.staging_dir / "steam_api64.dll"));
    TEST_CHECK(std::filesystem::exists(info.save_vault_dir));
    TEST_CHECK(std::filesystem::exists(info.desktop_shortcut));

    // Clean up sandbox
    std::error_code ec;
    std::filesystem::remove_all(test_papaya_root, ec);

    log::info("TEST", ">>> test_orchestrator PASSED ALL CHECKS! <<<");
    return 0;
}
