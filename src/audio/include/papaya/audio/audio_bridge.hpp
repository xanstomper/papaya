#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <atomic>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace papaya::audio {

struct AudioStreamConfig {
    u32 sample_rate{48000};
    u16 channels{2};
    u32 buffer_frames{1024};
    f32 master_volume{1.0f};
    bool enable_soft_limiter{true};
};

class AudioBridge {
public:
    explicit AudioBridge(const AudioStreamConfig& config = {});
    ~AudioBridge();

    Result<> initialize();
    void shutdown();

    bool is_streaming() const { return is_streaming_.load(); }
    void set_volume(f32 vol) { master_volume_ = std::clamp(vol, 0.0f, 2.0f); }
    f32 get_volume() const { return master_volume_.load(); }

    // Push PCM samples from WASAPI / DirectSound
    void write_pcm_samples(const f32* interleaved_samples, size_t frame_count);

    // Read PCM samples to send to AAudio / PulseAudio
    size_t read_pcm_samples(f32* out_interleaved, size_t max_frames);

    u64 get_total_frames_written() const { return total_frames_written_.load(); }
    u64 get_total_frames_read() const { return total_frames_read_.load(); }

private:
    AudioStreamConfig config_;
    std::atomic<bool> is_streaming_{false};
    std::atomic<f32> master_volume_{1.0f};
    std::atomic<u64> total_frames_written_{0};
    std::atomic<u64> total_frames_read_{0};

    std::vector<f32> ring_buffer_;
    size_t write_pos_{0};
    size_t read_pos_{0};
    size_t available_frames_{0};
    std::mutex buffer_mutex_;
};

} // namespace papaya::audio
