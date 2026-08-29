#include "papaya/input/virtual_xinput.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::input {

VirtualXInputManager::VirtualXInputManager() {
    pads_.fill(VirtualGamepadState{});
    vibrations_.fill(VirtualVibrationState{});
}

Result<> VirtualXInputManager::initialize() {
    log::info("INPUT", "Initializing Virtual XInput Controller Subsystem (4 Gamepad Slots Ready)");
    is_initialized_ = true;
    return {};
}

s16 VirtualXInputManager::apply_deadzone(s16 value, s16 deadzone) {
    if (std::abs(value) < deadzone) return 0;
    return value;
}

void VirtualXInputManager::set_pad_state(u32 user_index, const VirtualGamepadState& state) {
    if (user_index >= pads_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pads_[user_index] = state;
}

bool VirtualXInputManager::get_pad_state(u32 user_index, VirtualGamepadState& state) const {
    if (user_index >= pads_.size()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    state = pads_[user_index];
    return true;
}

void VirtualXInputManager::set_button(u32 user_index, u16 button_mask, bool pressed) {
    if (user_index >= pads_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (pressed) {
        pads_[user_index].buttons |= button_mask;
    } else {
        pads_[user_index].buttons &= ~button_mask;
    }
}

void VirtualXInputManager::set_axis(u32 user_index, s16 lx, s16 ly, s16 rx, s16 ry) {
    if (user_index >= pads_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pads_[user_index].thumb_lx = apply_deadzone(lx, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    pads_[user_index].thumb_ly = apply_deadzone(ly, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    pads_[user_index].thumb_rx = apply_deadzone(rx, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    pads_[user_index].thumb_ry = apply_deadzone(ry, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
}

void VirtualXInputManager::set_triggers(u32 user_index, u8 lt, u8 rt) {
    if (user_index >= pads_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pads_[user_index].left_trigger = (lt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) ? lt : 0;
    pads_[user_index].right_trigger = (rt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) ? rt : 0;
}

void VirtualXInputManager::set_vibration(u32 user_index, u16 left_speed, u16 right_speed) {
    if (user_index >= vibrations_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    vibrations_[user_index].left_motor_speed = left_speed;
    vibrations_[user_index].right_motor_speed = right_speed;
}

VirtualVibrationState VirtualXInputManager::get_vibration(u32 user_index) const {
    if (user_index >= vibrations_.size()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    return vibrations_[user_index];
}

} // namespace papaya::input
