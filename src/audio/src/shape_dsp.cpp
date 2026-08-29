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
    log::info("AUDIO", "Initialized Xbox SHAPE Audio DSP ({} hardware voice channels)", MAX_AUDIO_VOICES);
    return {};
}

void ShapeDsp::set_voice_buffer(u32 voice_id, GuestPhysAddr gpa, u32 size) {
    if (voice_id >= MAX_AUDIO_VOICES) return;
    voices_[voice_id].buffer_gpa = gpa;
    voices_[voice_id].buffer_size = size;
    log::trace("AUDIO", "Set Voice #{} Buffer: GPA 0x{:X}, Size {} bytes", voice_id, gpa, size);
}

void ShapeDsp::play_voice(u32 voice_id) {
    if (voice_id >= MAX_AUDIO_VOICES) return;
    voices_[voice_id].is_playing = true;
    log::debug("AUDIO", "Playing Voice #{}", voice_id);
}

void ShapeDsp::stop_voice(u32 voice_id) {
    if (voice_id >= MAX_AUDIO_VOICES) return;
    voices_[voice_id].is_playing = false;
    log::debug("AUDIO", "Stopped Voice #{}", voice_id);
}

} // namespace papaya::audio
