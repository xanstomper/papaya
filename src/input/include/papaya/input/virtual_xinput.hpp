#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"
#include <array>
#include <mutex>

namespace papaya::input {

// Standard XInput Button Bitmasks
constexpr u16 XINPUT_GAMEPAD_DPAD_UP        = 0x0001;
constexpr u16 XINPUT_GAMEPAD_DPAD_DOWN      = 0x0002;
constexpr u16 XINPUT_GAMEPAD_DPAD_LEFT      = 0x0004;
constexpr u16 XINPUT_GAMEPAD_DPAD_RIGHT     = 0x0008;
constexpr u16 XINPUT_GAMEPAD_START          = 0x0010;
constexpr u16 XINPUT_GAMEPAD_BACK           = 0x0020;
constexpr u16 XINPUT_GAMEPAD_LEFT_THUMB     = 0x0040;
constexpr u16 XINPUT_GAMEPAD_RIGHT_THUMB    = 0x0080;
constexpr u16 XINPUT_GAMEPAD_LEFT_SHOULDER  = 0x0100;
constexpr u16 XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr u16 XINPUT_GAMEPAD_A              = 0x1000;
constexpr u16 XINPUT_GAMEPAD_B              = 0x2000;
constexpr u16 XINPUT_GAMEPAD_X              = 0x4000;
constexpr u16 XINPUT_GAMEPAD_Y              = 0x8000;

struct VirtualGamepadState {
    u16 buttons{0};
    u8 left_trigger{0};
    u8 right_trigger{0};
    s16 thumb_lx{0};
    s16 thumb_ly{0};
    s16 thumb_rx{0};
    s16 thumb_ry{0};
};

class VirtualXInputManager {
public:
    VirtualXInputManager();
    ~VirtualXInputManager() = default;

    Result<> initialize();

    void set_pad_state(u32 user_index, const VirtualGamepadState& state);
    bool get_pad_state(u32 user_index, VirtualGamepadState& state) const;

    // Direct helper to set button flag
    void set_button(u32 user_index, u16 button_mask, bool pressed);
    void set_axis(u32 user_index, s16 lx, s16 ly, s16 rx, s16 ry);
    void set_triggers(u32 user_index, u8 lt, u8 rt);

private:
    std::array<VirtualGamepadState, 4> pads_;
    mutable std::mutex mutex_;
    bool is_initialized_{false};
};

} // namespace papaya::input
