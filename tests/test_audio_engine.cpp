#include "papaya/common/logger.hpp"
#include "papaya/audio/audio_engine.hpp"
#include <cassert>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::audio;

    log::info("TEST", "Running unit test: test_audio_engine");

    AudioEngine engine;
    assert(engine.initialize().has_value());

    // 1. Configure a DSP test voice
    auto& dsp = engine.get_dsp();
    assert(dsp.set_voice_parameters(0, 48000, 1.0f, 0.0f, true).has_value());

    // Generate 480 samples of a test sine wave
    std::vector<f32> pcm(480);
    for (size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = std::sin(2.0f * 3.14159f * 440.0f * i / 48000.0f);
    }
    assert(dsp.submit_voice_buffer(0, pcm).has_value());

    // 2. Test Rendering Mix Blocks
    std::vector<f32> out_block(256 * 2, 0.0f);
    engine.render_mix_block(out_block);

    // Verify audio samples were rendered
    bool has_audio_signal = false;
    for (f32 s : out_block) {
        if (std::abs(s) > 0.001f) {
            has_audio_signal = true;
            break;
        }
    }
    assert(has_audio_signal);
    log::info("TEST", "SHAPE DSP mix block rendered valid audio signal");

    // 3. Test Master Volume
    engine.set_master_volume(0.5f);
    assert(engine.get_master_volume() == 0.5f);

    // 4. Test Real-time Background Playback Stream Thread
    assert(engine.start_stream().has_value());
    assert(engine.is_streaming() == true);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(engine.get_total_frames_rendered() > 0);

    engine.stop_stream();
    assert(engine.is_streaming() == false);

    log::info("TEST", "Total audio frames rendered: {}", engine.get_total_frames_rendered());
    log::info("TEST", ">>> test_audio_engine PASSED ALL CHECKS! <<<");
    return 0;
}
