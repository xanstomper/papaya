#include "papaya/input/virtual_xinput.hpp"
#include "papaya/common/logger.hpp"

namespace papaya::input {

VirtualXInputManager::VirtualXInputManager() {
    pads_.fill(VirtualGamepadState{});
}

Result<> VirtualXInputManager::initialize() {
    log::info("INPUT", "Initializing Virtual XInput Controller Subsystem (4 Gamepad Slots)");
    is_initialized_ = true;
    return {};
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
    pads_[user_index].thumb_lx = lx;
    pads_[user_index].thumb_ly = ly;
    pads_[user_index].thumb_rx = rx;
    pads_[user_index].thumb_ry = ry;
}

void VirtualXInputManager::set_triggers(u32 user_index, u8 lt, u8 rt) {
    if (user_index >= pads_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pads_[user_index].left_trigger = lt;
    pads_[user_index].right_trigger = rt;
}

} // namespace papaya::input
