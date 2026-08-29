#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <atomic>
#include <mutex>

namespace papaya::audio {

struct AudioStreamConfig {
    u32 sample_rate{48000};
    u16 channels{2};
    u32 buffer_frames{512};
};

class AudioBridge {
public:
    explicit AudioBridge(const AudioStreamConfig& config = {});
    ~AudioBridge();

    Result<> initialize();
    void shutdown();

    bool is_streaming() const { return is_streaming_.load(); }

    // Push PCM samples from WASAPI / DirectSound
    void write_pcm_samples(const f32* interleaved_samples, size_t frame_count);

    u64 get_total_frames_written() const { return total_frames_written_.load(); }

private:
    AudioStreamConfig config_;
    std::atomic<bool> is_streaming_{false};
    std::atomic<u64> total_frames_written_{0};
    std::vector<f32> ring_buffer_;
    std::mutex buffer_mutex_;
};

} // namespace papaya::audio
