#pragma once

#include "papaya/common/types.hpp"

namespace papaya::input {

// Xbox Gamepad Button Flags
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

#pragma pack(push, 1)
struct GamepadState {
    u16 buttons{0};
    u8  left_trigger{0};
    u8  right_trigger{0};
    s16 thumb_lx{0};
    s16 thumb_ly{0};
    s16 thumb_rx{0};
    s16 thumb_ry{0};
};

struct GamepadVibration {
    u16 left_motor_speed{0};
    u16 right_motor_speed{0};
    u16 left_trigger_motor{0};  // Xbox One impulse trigger rumble
    u16 right_trigger_motor{0}; // Xbox One impulse trigger rumble
};
#pragma pack(pop)

} // namespace papaya::input
