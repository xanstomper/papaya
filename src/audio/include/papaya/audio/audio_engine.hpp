#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/audio/shape_dsp.hpp"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <span>

namespace papaya::audio {

constexpr u32 AUDIO_CHANNELS_STEREO = 2;
constexpr u32 AUDIO_FRAMES_PER_BLOCK = 256; // 5.33ms latency

struct AudioBufferView {
    const f32* data{nullptr};
    size_t frame_count{0};
    u32 channels{2};
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    Result<> initialize();
    Result<> start_stream();
    void stop_stream();

    void render_mix_block(std::span<f32> output_buffer);
    void set_master_volume(f32 volume);
    f32 get_master_volume() const { return master_volume_.load(); }

    bool is_streaming() const { return is_streaming_.load(); }
    u64 get_total_frames_rendered() const { return total_frames_rendered_.load(); }

    ShapeDsp& get_dsp() { return dsp_; }
    const ShapeDsp& get_dsp() const { return dsp_; }

private:
    void stream_worker_loop();

    ShapeDsp dsp_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> is_streaming_{false};
    std::atomic<f32> master_volume_{1.0f};
    std::atomic<u64> total_frames_rendered_{0};

    std::thread stream_thread_;
    mutable std::mutex mix_mutex_;
    std::vector<f32> block_buffer_;
};

} // namespace papaya::audio
