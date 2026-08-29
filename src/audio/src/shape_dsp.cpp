#include "papaya/audio/shape_dsp.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::audio {

ShapeDsp::ShapeDsp() {
    voices_.resize(MAX_AUDIO_VOICES);
    for (u32 i = 0; i < MAX_AUDIO_VOICES; ++i) {
        voices_[i].voice_id = i;
    }
}

ShapeDsp::~ShapeDsp() = default;

Result<> ShapeDsp::initialize() {
    log::info("SHAPE", "Initializing SHAPE DSP 256-voice Mixer");
    return {};
}

Result<> ShapeDsp::set_voice_parameters(u32 voice_id, u32 sample_rate, f32 vol_left, f32 vol_right, bool loop) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_id >= MAX_AUDIO_VOICES) {
        return ErrorCode::InvalidParameter;
    }

    voices_[voice_id].sample_rate = sample_rate;
    voices_[voice_id].volume_left = vol_left;
    voices_[voice_id].volume_right = vol_right;
    voices_[voice_id].loop = loop;
    return {};
}

Result<> ShapeDsp::submit_voice_buffer(u32 voice_id, std::span<const f32> samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_id >= MAX_AUDIO_VOICES) {
        return ErrorCode::InvalidParameter;
    }

    voices_[voice_id].pcm_data.assign(samples.begin(), samples.end());
    voices_[voice_id].playback_position = 0;
    voices_[voice_id].is_playing = true;
    return {};
}

void ShapeDsp::set_voice_buffer(u32 voice_id, GuestPhysAddr gpa, u32 size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_id < MAX_AUDIO_VOICES) {
        voices_[voice_id].buffer_gpa = gpa;
        voices_[voice_id].buffer_size = size;
    }
}

void ShapeDsp::play_voice(u32 voice_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_id < MAX_AUDIO_VOICES) {
        voices_[voice_id].is_playing = true;
    }
}

void ShapeDsp::stop_voice(u32 voice_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_id < MAX_AUDIO_VOICES) {
        voices_[voice_id].is_playing = false;
        voices_[voice_id].playback_position = 0;
    }
}

void ShapeDsp::process_mix_buffer(f32* out_interleaved_stereo, size_t frame_count) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t f = 0; f < frame_count; ++f) {
        f32 left_mix = 0.0f;
        f32 right_mix = 0.0f;

        for (auto& v : voices_) {
            if (!v.is_playing || v.pcm_data.empty()) continue;

            if (v.playback_position < v.pcm_data.size()) {
                f32 sample = v.pcm_data[v.playback_position++];
                left_mix += sample * v.volume_left;
                right_mix += sample * v.volume_right;
            } else if (v.loop) {
                v.playback_position = 0;
                f32 sample = v.pcm_data[v.playback_position++];
                left_mix += sample * v.volume_left;
                right_mix += sample * v.volume_right;
            } else {
                v.is_playing = false;
            }
        }

        out_interleaved_stereo[f * 2]     += left_mix;
        out_interleaved_stereo[f * 2 + 1] += right_mix;
    }
}

} // namespace papaya::audio
