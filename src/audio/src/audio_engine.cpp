#include "papaya/audio/audio_engine.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::audio {

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() = default;

Result<> AudioEngine::initialize() {
    log::info("AUDIO", "Initializing Papaya Audio Engine");
    auto res = dsp_.initialize();
    if (!res) {
        return res;
    }
    initialized_ = true;
    return {};
}

void AudioEngine::process_audio_frame() {
    // Process audio mixing cycle
}

} // namespace papaya::audio
