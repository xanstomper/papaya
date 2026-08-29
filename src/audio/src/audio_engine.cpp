#include "papaya/audio/audio_engine.hpp"
#include "papaya/common/logger.hpp"
#include <chrono>
#include <cstring>
#include <algorithm>

namespace papaya::audio {

AudioEngine::AudioEngine() {
    block_buffer_.resize(AUDIO_FRAMES_PER_BLOCK * AUDIO_CHANNELS_STEREO, 0.0f);
}

AudioEngine::~AudioEngine() {
    stop_stream();
}

Result<> AudioEngine::initialize() {
    log::info("AUDIO", "Initializing Papaya Audio Engine (SHAPE DSP 256-voice Mixer @ 48kHz Stereo)");
    auto dsp_res = dsp_.initialize();
    if (!dsp_res) {
        log::error("AUDIO", "SHAPE DSP initialization failed");
        return dsp_res;
    }

    initialized_ = true;
    return {};
}

Result<> AudioEngine::start_stream() {
    if (!initialized_) {
        auto init_res = initialize();
        if (!init_res) return init_res;
    }

    if (is_streaming_) {
        return {};
    }

    is_streaming_ = true;
    stream_thread_ = std::thread(&AudioEngine::stream_worker_loop, this);
    log::info("AUDIO", "Started real-time background audio playback stream");
    return {};
}

void AudioEngine::stop_stream() {
    if (is_streaming_) {
        is_streaming_ = false;
        if (stream_thread_.joinable()) {
            stream_thread_.join();
        }
        log::info("AUDIO", "Stopped background audio playback stream");
    }
}

void AudioEngine::set_master_volume(f32 volume) {
    master_volume_ = std::clamp(volume, 0.0f, 2.0f);
}

void AudioEngine::render_mix_block(std::span<f32> output_buffer) {
    std::lock_guard<std::mutex> lock(mix_mutex_);

    // Zero output buffer
    std::fill(output_buffer.begin(), output_buffer.end(), 0.0f);

    // Mix 256 hardware voices from SHAPE DSP
    dsp_.process_mix_buffer(output_buffer.data(), output_buffer.size() / AUDIO_CHANNELS_STEREO);

    // Apply master volume
    f32 vol = master_volume_.load();
    if (vol != 1.0f) {
        for (f32& sample : output_buffer) {
            sample *= vol;
        }
    }

    total_frames_rendered_ += (output_buffer.size() / AUDIO_CHANNELS_STEREO);
}

void AudioEngine::stream_worker_loop() {
    // 256 samples @ 48kHz = 5.333 milliseconds
    const auto block_duration = std::chrono::microseconds(5333);

    while (is_streaming_) {
        auto start_time = std::chrono::steady_clock::now();

        render_mix_block(block_buffer_);

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed < block_duration) {
            std::this_thread::sleep_for(block_duration - elapsed);
        }
    }
}

} // namespace papaya::audio
