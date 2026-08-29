#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>
#include <span>
#include <mutex>

namespace papaya::audio {

constexpr u32 AUDIO_SAMPLE_RATE_48K = 48000;
constexpr u32 MAX_AUDIO_VOICES      = 256;

struct ShapeVoiceState {
    u32 voice_id{0};
    u32 sample_rate{AUDIO_SAMPLE_RATE_48K};
    u16 channels{2};
    f32 volume_left{1.0f};
    f32 volume_right{1.0f};
    GuestPhysAddr buffer_gpa{0};
    u32 buffer_size{0};
    bool is_playing{false};
    bool loop{false};
    std::vector<f32> pcm_data{};
    size_t playback_position{0};
};

class ShapeDsp {
public:
    ShapeDsp();
    ~ShapeDsp();

    Result<> initialize();
    Result<> set_voice_parameters(u32 voice_id, u32 sample_rate, f32 vol_left, f32 vol_right, bool loop = false);
    Result<> submit_voice_buffer(u32 voice_id, std::span<const f32> samples);
    void set_voice_buffer(u32 voice_id, GuestPhysAddr gpa, u32 size);
    void play_voice(u32 voice_id);
    void stop_voice(u32 voice_id);

    void process_mix_buffer(f32* out_interleaved_stereo, size_t frame_count);

private:
    std::vector<ShapeVoiceState> voices_;
    mutable std::mutex mutex_;
};

} // namespace papaya::audio
