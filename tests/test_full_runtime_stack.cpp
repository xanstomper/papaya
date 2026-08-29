#include "papaya/common/logger.hpp"
#include "papaya/frontend/emulator_runtime.hpp"
#include <iostream>
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
    using namespace papaya::frontend;

    log::info("TEST", "Running integration test: test_full_runtime_stack");

    RuntimeConfig cfg{
        .device_tier = DeviceTier::UltraLowEnd, // Raspberry Pi 5 / Potato Mode
        .performance_mode = PerformanceMode::PotatoMode,
        .game_executable_path = "test_game.exe",
        .steam_app_id = 480, // SpaceWar
        .headless = true,
        .force_potato_mode = true
    };

    EmulatorRuntime runtime(cfg);
    auto init_res = runtime.initialize();
    TEST_CHECK(init_res.has_value());

    // 1. Verify subsystem states
    TEST_CHECK(runtime.get_steam().is_initialized());
    TEST_CHECK(runtime.get_window().is_headless());
    TEST_CHECK(runtime.get_audio().is_streaming());

    // 2. Launch game
    auto launch_res = runtime.launch_game("test_game.exe");
    TEST_CHECK(launch_res.has_value());
    TEST_CHECK(runtime.is_running());

    // 3. Step frames and verify game loop
    for (int i = 0; i < 60; ++i) {
        runtime.step_frame();
    }
    TEST_CHECK(runtime.get_frame_count() == 60);

    // 4. Test Gamepad state interaction during loop
    input::VirtualGamepadState pad{};
    pad.buttons = input::XINPUT_GAMEPAD_B | input::XINPUT_GAMEPAD_Y;
    runtime.get_input().set_pad_state(0, pad);

    input::VirtualGamepadState read_pad{};
    TEST_CHECK(runtime.get_input().get_pad_state(0, read_pad));
    TEST_CHECK((read_pad.buttons & input::XINPUT_GAMEPAD_B) != 0);
    TEST_CHECK((read_pad.buttons & input::XINPUT_GAMEPAD_Y) != 0);

    runtime.stop();
    TEST_CHECK(!runtime.is_running());
    TEST_CHECK(!runtime.get_audio().is_streaming());

    log::info("TEST", ">>> test_full_runtime_stack PASSED ALL CHECKS! <<<");
    return 0;
}
