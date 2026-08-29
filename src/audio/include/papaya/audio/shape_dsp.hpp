#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <vector>

namespace papaya::audio {

// Xbox One SHAPE Audio hardware parameters
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
};

class ShapeDsp {
public:
    ShapeDsp();
    ~ShapeDsp();

    Result<> initialize();
    void set_voice_buffer(u32 voice_id, GuestPhysAddr gpa, u32 size);
    void play_voice(u32 voice_id);
    void stop_voice(u32 voice_id);

private:
    std::vector<ShapeVoiceState> voices_;
};

} // namespace papaya::audio
