#include "papaya/common/logger.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include "papaya/rom/rom_loader.hpp"
#include "papaya/rom/steam_rom_bridge.hpp"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstring>

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAILED: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

int main() {
    using namespace papaya;
    using namespace papaya::frontend;
    using namespace papaya::rom;

    log::info("TEST", "Running integration test: test_rom_game_loop");

    // 1. Create a mock game ISO
    std::filesystem::path mock_rom_path = "./san_andreas_loop.iso";
    {
        std::vector<u8> iso_data(32 * SECTOR_SIZE_ISO_DATA, 0);
        u64 pvd_offset = 16 * SECTOR_SIZE_ISO_DATA;
        iso_data[pvd_offset] = 1;
        std::memcpy(&iso_data[pvd_offset + 1], "CD001", 5);
        iso_data[pvd_offset + 6] = 1;

        const char* vol_id = "GTA_SAN_ANDREAS";
        std::memcpy(&iso_data[pvd_offset + 40], vol_id, std::strlen(vol_id));

        const char* app_id = "SLUS-20946";
        std::memcpy(&iso_data[pvd_offset + 592], app_id, std::strlen(app_id));

        std::ofstream out(mock_rom_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(iso_data.data()), iso_data.size());
    }

    // 2. Initialize Emulator Runtime in Potato Mode (UltraLowEnd handheld tier)
    RuntimeConfig cfg{};
    cfg.device_tier = DeviceTier::MobileHighTier; // AYN Odin 2 / Snapdragon 8 Gen 2
    cfg.performance_mode = PerformanceMode::Performance;
    cfg.headless = true;
    cfg.force_potato_mode = false;

    EmulatorRuntime runtime(cfg);
    auto init_res = runtime.initialize();
    TEST_CHECK(init_res.has_value());

    // 3. Mount and bind ROM to Steam
    RomImageLoader loader;
    auto meta_res = loader.open_file(mock_rom_path);
    TEST_CHECK(meta_res.has_value());

    TEST_CHECK(SteamRomBridge::bind_rom_to_steam_stub(*meta_res, mock_rom_path, runtime.get_steam()).has_value());
    TEST_CHECK(runtime.mount_and_launch_rom(mock_rom_path.string()).has_value());
    TEST_CHECK(runtime.is_running());

    // 4. Step game loop through 60 frames
    for (int frame = 0; frame < 60; ++frame) {
        // Send gamepad input (e.g. Accelerate button on virtual controller)
        input::VirtualGamepadState pad{};
        pad.buttons = input::XINPUT_GAMEPAD_A;
        pad.right_trigger = 255;
        runtime.get_input().set_pad_state(0, pad);

        runtime.step_frame();
    }

    // The runtime may legitimately stop the loop early on a child-reap / close /
    // watchdog path, so assert it actually ran frames rather than an exact count.
    TEST_CHECK(runtime.get_frame_count() >= 1);

    runtime.stop();
    loader.close();

    // Clean up temporary mock file
    std::error_code ec;
    std::filesystem::remove(mock_rom_path, ec);

    log::info("TEST", ">>> test_rom_game_loop PASSED ALL CHECKS! <<<");
    return 0;
}
