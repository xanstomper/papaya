#pragma once

#include "papaya/common/types.hpp"

// DirectInput8 keyboard/mouse for the native Win32 HLE.
//
// DirectInput8Create returns an IDirectInput8 (ms_abi vtable); CreateDevice
// returns an IDirectInputDevice8 whose GetDeviceState fills the guest's key/
// button buffer from the current X11 keyboard/mouse state. The standard game
// flow (DirectInput8Create -> CreateDevice(GUID_SysKeyboard) -> SetDataFormat
// -> Acquire -> GetDeviceState) returns real data. No mocks.
namespace papaya::win32 {

// DirectInput8Create -> *di8_out = IDirectInput8. Returns DI_OK (0).
int dinput8_create(void* hinst, u32 version, const void* iid, void** di8_out, void* unk_outer);

} // namespace papaya::win32