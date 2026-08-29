#include "papaya/input/input_manager.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::input {

InputManager::InputManager() = default;
InputManager::~InputManager() = default;

Result<> InputManager::initialize() {
    log::info("INPUT", "Initializing Xbox Controller Input Subsystem (up to {} gamepads)", MAX_CONTROLLERS);
    return {};
}

void InputManager::update() {
    // Poll input devices (SDL / evdev)
}

const GamepadState& InputManager::get_gamepad_state(u32 user_index) const {
    if (user_index >= MAX_CONTROLLERS) {
        static GamepadState dummy{};
        return dummy;
    }
    return states_[user_index];
}

void InputManager::set_gamepad_state(u32 user_index, const GamepadState& state) {
    if (user_index < MAX_CONTROLLERS) {
        states_[user_index] = state;
    }
}

void InputManager::set_vibration(u32 user_index, const GamepadVibration& vib) {
    if (user_index < MAX_CONTROLLERS) {
        vibrations_[user_index] = vib;
        log::trace("INPUT", "Set Controller #{} Vibration: L={}, R={}, TriggerL={}, TriggerR={}",
                   user_index, vib.left_motor_speed, vib.right_motor_speed, vib.left_trigger_motor, vib.right_trigger_motor);
    }
}

} // namespace papaya::input
