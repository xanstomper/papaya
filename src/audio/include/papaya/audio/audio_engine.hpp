#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/audio/shape_dsp.hpp"
#include <memory>

namespace papaya::audio {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    Result<> initialize();
    void process_audio_frame();

    ShapeDsp& get_dsp() { return dsp_; }

private:
    ShapeDsp dsp_;
    bool initialized_{false};
};

} // namespace papaya::audio
