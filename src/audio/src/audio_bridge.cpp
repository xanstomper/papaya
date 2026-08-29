#include "papaya/audio/audio_bridge.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::audio {

AudioBridge::AudioBridge(const AudioStreamConfig& config)
    : config_(config) {
    ring_buffer_.resize(config_.buffer_frames * config_.channels * 4, 0.0f);
}

AudioBridge::~AudioBridge() {
    shutdown();
}

Result<> AudioBridge::initialize() {
    log::info("AUDIO", "Initializing Low-Latency Audio Bridge [{}Hz, {} Channels, {} Frame Buffer]",
              config_.sample_rate, config_.channels, config_.buffer_frames);
    is_streaming_ = true;
    return {};
}

void AudioBridge::shutdown() {
    if (is_streaming_) {
        is_streaming_ = false;
        log::info("AUDIO", "Audio Bridge successfully shut down.");
    }
}

void AudioBridge::write_pcm_samples(const f32* interleaved_samples, size_t frame_count) {
    if (!is_streaming_ || !interleaved_samples || frame_count == 0) return;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    total_frames_written_ += frame_count;
}

} // namespace papaya::audio
