#include "papaya/common/logger.hpp"
#include "papaya/audio/audio_bridge.hpp"
#include "papaya/input/virtual_xinput.hpp"
#include <vector>
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

    log::info("TEST", "Running unit test: test_audio_input");

    // 1. Test Audio Bridge with read/write and volume limiter
    audio::AudioStreamConfig acfg{
        .sample_rate = 48000,
        .channels = 2,
        .buffer_frames = 1024,
        .master_volume = 1.0f,
        .enable_soft_limiter = true
    };
    audio::AudioBridge audio_bridge(acfg);
    TEST_CHECK(audio_bridge.initialize().has_value());
    TEST_CHECK(audio_bridge.is_streaming());

    std::vector<f32> pcm_samples(1024, 0.5f);
    audio_bridge.write_pcm_samples(pcm_samples.data(), 512);
    TEST_CHECK(audio_bridge.get_total_frames_written() == 512);

    std::vector<f32> read_buf(1024, 0.0f);
    size_t read_frames = audio_bridge.read_pcm_samples(read_buf.data(), 512);
    TEST_CHECK(read_frames == 512);
    TEST_CHECK(audio_bridge.get_total_frames_read() == 512);

    audio_bridge.shutdown();
    TEST_CHECK(!audio_bridge.is_streaming());

    // 2. Test Virtual XInput Manager with deadzones & rumble
    input::VirtualXInputManager input_mgr;
    TEST_CHECK(input_mgr.initialize().has_value());

    input::VirtualGamepadState pad{};
    pad.buttons = input::XINPUT_GAMEPAD_A | input::XINPUT_GAMEPAD_START;
    pad.thumb_lx = 16384;
    pad.right_trigger = 255;
    input_mgr.set_pad_state(0, pad);

    input::VirtualGamepadState read_pad{};
    TEST_CHECK(input_mgr.get_pad_state(0, read_pad));
    TEST_CHECK((read_pad.buttons & input::XINPUT_GAMEPAD_A) != 0);
    TEST_CHECK((read_pad.buttons & input::XINPUT_GAMEPAD_START) != 0);
    TEST_CHECK(read_pad.thumb_lx == 16384);
    TEST_CHECK(read_pad.right_trigger == 255);

    // Test Deadzones
    s16 dead_val = input::VirtualXInputManager::apply_deadzone(3000, input::XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    TEST_CHECK(dead_val == 0);

    // Test Rumble Motors
    input_mgr.set_vibration(0, 32000, 48000);
    auto vib = input_mgr.get_vibration(0);
    TEST_CHECK(vib.left_motor_speed == 32000);
    TEST_CHECK(vib.right_motor_speed == 48000);

    log::info("TEST", ">>> test_audio_input PASSED ALL CHECKS! <<<");
    return 0;
}
