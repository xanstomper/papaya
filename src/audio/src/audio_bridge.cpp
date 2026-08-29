#include "papaya/audio/audio_bridge.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::audio {

AudioBridge::AudioBridge(const AudioStreamConfig& config)
    : config_(config),
      master_volume_(config.master_volume) {
    ring_buffer_.resize(config_.buffer_frames * config_.channels * 4, 0.0f);
}

AudioBridge::~AudioBridge() {
    shutdown();
}

Result<> AudioBridge::initialize() {
    log::info("AUDIO", "Initializing Low-Latency Audio Stream [Rate: {} Hz, Channels: {}, Buffer: {} frames]",
              config_.sample_rate, config_.channels, config_.buffer_frames);
    is_streaming_ = true;
    return {};
}

void AudioBridge::shutdown() {
    if (is_streaming_) {
        is_streaming_ = false;
        log::info("AUDIO", "Audio Stream stopped cleanly [Frames: W:{}, R:{}]",
                  total_frames_written_.load(), total_frames_read_.load());
    }
}

void AudioBridge::write_pcm_samples(const f32* interleaved_samples, size_t frame_count) {
    if (!is_streaming_ || !interleaved_samples || frame_count == 0) return;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    size_t total_samples = frame_count * config_.channels;
    size_t ring_size = ring_buffer_.size();
    f32 vol = master_volume_.load();

    for (size_t i = 0; i < total_samples; ++i) {
        f32 sample = interleaved_samples[i] * vol;
        if (config_.enable_soft_limiter) {
            // Soft-knee tanh saturation limiter
            sample = std::tanh(sample);
        }
        ring_buffer_[(write_pos_ + i) % ring_size] = sample;
    }

    write_pos_ = (write_pos_ + total_samples) % ring_size;
    available_frames_ = std::min(available_frames_ + frame_count, static_cast<size_t>(config_.buffer_frames * 4));
    total_frames_written_ += frame_count;
}

size_t AudioBridge::read_pcm_samples(f32* out_interleaved, size_t max_frames) {
    if (!is_streaming_ || !out_interleaved || max_frames == 0) return 0;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    size_t frames_to_read = std::min(max_frames, available_frames_);
    if (frames_to_read == 0) {
        std::fill_n(out_interleaved, max_frames * config_.channels, 0.0f);
        return 0;
    }

    size_t samples_to_read = frames_to_read * config_.channels;
    size_t ring_size = ring_buffer_.size();

    for (size_t i = 0; i < samples_to_read; ++i) {
        out_interleaved[i] = ring_buffer_[(read_pos_ + i) % ring_size];
    }

    read_pos_ = (read_pos_ + samples_to_read) % ring_size;
    available_frames_ -= frames_to_read;
    total_frames_read_ += frames_to_read;

    return frames_to_read;
}

} // namespace papaya::audio
