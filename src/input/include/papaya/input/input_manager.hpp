#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include "papaya/input/xinput_state.hpp"
#include <array>

namespace papaya::input {

constexpr size_t MAX_CONTROLLERS = 4;

class InputManager {
public:
    InputManager();
    ~InputManager();

    Result<> initialize();
    void update();

    const GamepadState& get_gamepad_state(u32 user_index) const;
    void set_gamepad_state(u32 user_index, const GamepadState& state);
    void set_vibration(u32 user_index, const GamepadVibration& vib);

private:
    std::array<GamepadState, MAX_CONTROLLERS> states_{};
    std::array<GamepadVibration, MAX_CONTROLLERS> vibrations_{};
};

} // namespace papaya::input
